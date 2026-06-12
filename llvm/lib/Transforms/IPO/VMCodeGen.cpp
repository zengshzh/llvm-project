//===-- VMCodeGen.cpp - Generate bytecode for VMP-annotated functions -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Detects functions annotated with __attribute__((annotate("VMP"))) via
// @llvm.global.annotations, translates their IR to VM bytecode, and inserts
// a VMExecute(bytecode, size) call at the function entry.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/VMCodeGen.h"
#include "vminterpreter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/Debug.h"
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "vm-codegen"

//===----------------------------------------------------------------------===//
// Annotation detection
//===----------------------------------------------------------------------===//
static bool hasVMPAnnotation(Function *F, Module &M) {
  auto *Annotations = M.getGlobalVariable("llvm.global.annotations");
  if (!Annotations)
    return false;

  auto *C = dyn_cast_or_null<Constant>(Annotations);
  if (!C || C->getNumOperands() != 1)
    return false;

  C = cast<Constant>(C->getOperand(0));

  for (auto &Op : C->operands()) {
    auto *OpC = dyn_cast<ConstantStruct>(&Op);
    if (!OpC || OpC->getNumOperands() < 4)
      continue;

    auto *AnnotatedFn =
        dyn_cast<Function>(OpC->getOperand(0)->stripPointerCasts());
    if (AnnotatedFn != F)
      continue;

    auto *StrC = dyn_cast<GlobalValue>(OpC->getOperand(1)->stripPointerCasts());
    if (!StrC)
      continue;

    auto *StrData = dyn_cast<ConstantDataSequential>(StrC->getOperand(0));
    if (!StrData)
      continue;

    if (StrData->getAsCString() == "VMP")
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Bytecode emission helpers
//===----------------------------------------------------------------------===//

// Encode one instruction: [op(1) flags(1) dst(2) src1(2) src2(2)] = 8 bytes
static void emitInsn(std::vector<uint8_t> &BC, uint8_t Op, uint16_t Dst,
                     uint16_t Src1, uint16_t Src2, uint8_t Flags = 0) {
  auto put16 = [&](uint16_t V) {
    BC.push_back(V & 0xFF);
    BC.push_back((V >> 8) & 0xFF);
  };
  BC.push_back(Op);
  BC.push_back(Flags);
  put16(Dst);
  put16(Src1);
  put16(Src2);
}

// Encode LI32: [op(1) flags(1) dst(2) value_lo(2) value_hi(2)] = 8 bytes
static void emitInsn32(std::vector<uint8_t> &BC, uint8_t Op, uint16_t Dst,
                       uint32_t Val) {
  auto put16 = [&](uint16_t V) {
    BC.push_back(V & 0xFF);
    BC.push_back((V >> 8) & 0xFF);
  };
  BC.push_back(Op);
  BC.push_back(0);
  put16(Dst);
  put16(Val & 0xFFFF);
  put16((Val >> 16) & 0xFFFF);
}

//===----------------------------------------------------------------------===//
// Translate IR function to VM bytecode
//===----------------------------------------------------------------------===//

// RegisterAllocator: 3-phase dynamic register management.
// - UseCount: number of remaining operand references.
// - FreeList: recycled registers, popped by alloc, pushed by freeReg.
// - DefBB: basic block where each value is defined (cross-BB values never freed).
// - PHINode registers are never freed (loop-carried values).
// r0-r7 are reserved for function args and never enter FreeList.
struct RegisterAllocator {
  unsigned NextReg = 8; // r0-r7 reserved for function args
  unsigned MaxReg = 8;  // at least 8 for r0-r7
  DenseMap<Value *, unsigned> Map;
  SmallVector<unsigned> FreeList;
  DenseMap<Value *, unsigned> *UseCount;     // not owned
  DenseMap<Value *, Value *> AliasParent;    // GEP alias → base
  DenseMap<Value *, BasicBlock *> DefBB;     // BB where each value is defined
  BasicBlock *CurrentBB = nullptr;           // BB currently being translated

  // Allocate a register and map it to value V (needed for lookups)
  unsigned alloc(Value *V) {
    unsigned r = allocRaw();
    Map[V] = r;
    if (auto *I = dyn_cast<Instruction>(V))
      DefBB[V] = I->getParent();
    return r;
  }

  // Allocate a temporary register (no Map entry, freed manually)
  unsigned allocRaw() {
    unsigned r;
#ifdef VM_NO_RECYCLE
    r = NextReg++;
#else
    if (!FreeList.empty()) {
      r = FreeList.pop_back_val();
    } else {
      r = NextReg++;
    }
#endif
    if (r + 1 > MaxReg)
      MaxReg = r + 1;
    return r;
  }

  void freeReg(unsigned r) {
#ifndef VM_NO_RECYCLE
    FreeList.push_back(r);
#endif
  }

  // Consume an operand: decrement use count, auto-free when done.
  unsigned consume(Value *V) {
    unsigned r = Map.lookup(V);
    Value *realV = V;
    while (AliasParent.count(realV))
      realV = AliasParent[realV];
    if (UseCount) {
      auto It = UseCount->find(realV);
      if (It != UseCount->end() && --It->second == 0 && !isa<PHINode>(realV)) {
        // Only free values defined in the current BB. Cross-BB values (e.g.
        // defined in entry, used in a loop) are never freed — a back-edge
        // would reuse the register while the value is still live.
        auto DB = DefBB.find(realV);
        if (DB != DefBB.end() && DB->second == CurrentBB)
          freeReg(r);
      }
    }
    return r;
  }

  // Look up an already-allocated register (no consume)
  unsigned lookupReg(Value *V) {
    return Map.lookup(V);
  }

  void setArg(unsigned ArgIdx, Value *V) {
    assert(ArgIdx < 8 && "Only 8 argument registers supported");
    Map[V] = ArgIdx;
  }

  // Alias a GEP result to its base pointer (zero offset)
  void aliasValue(Value *Alias, Value *Target) {
    if (UseCount) {
      (*UseCount)[Target] += (*UseCount)[Alias];
      UseCount->erase(Alias);
    }
    AliasParent[Alias] = Target;
    Map[Alias] = Map[Target];
  }
};

// Map LLVM FCmpInst predicate to VM FCMP predicate encoding (flags bits 0-3)
static uint8_t mapFCmpPred(CmpInst::Predicate Pred) {
  switch (Pred) {
  case CmpInst::FCMP_OEQ: return 0;
  case CmpInst::FCMP_OGT: return 1;
  case CmpInst::FCMP_OGE: return 2;
  case CmpInst::FCMP_OLT: return 3;
  case CmpInst::FCMP_OLE: return 4;
  case CmpInst::FCMP_ONE: return 5;
  case CmpInst::FCMP_ORD: return 6;
  case CmpInst::FCMP_UNO: return 7;
  case CmpInst::FCMP_UEQ: return 8;
  case CmpInst::FCMP_UGT: return 9;
  case CmpInst::FCMP_UGE: return 10;
  case CmpInst::FCMP_ULT: return 11;
  case CmpInst::FCMP_ULE: return 12;
  case CmpInst::FCMP_UNE: return 13;
  default:
    report_fatal_error(Twine("[VMCodeGen] unsupported fcmp predicate: ") +
                       Twine(Pred) + "\n");
  }
}

// Map LLVM ICmpInst predicate to VM CMP predicate encoding (flags bits 0-3)
static uint8_t mapICmpPred(CmpInst::Predicate Pred) {
  switch (Pred) {
  case CmpInst::ICMP_EQ:  return 0;
  case CmpInst::ICMP_NE:  return 1;
  case CmpInst::ICMP_UGT: return 2;
  case CmpInst::ICMP_UGE: return 3;
  case CmpInst::ICMP_ULT: return 4;
  case CmpInst::ICMP_ULE: return 5;
  case CmpInst::ICMP_SGT: return 6;
  case CmpInst::ICMP_SGE: return 7;
  case CmpInst::ICMP_SLT: return 8;
  case CmpInst::ICMP_SLE: return 9;
  default:
    report_fatal_error(Twine("[VMCodeGen] unsupported icmp predicate: ") +
                       Twine(Pred) + "\n");
  }
}

// One global variable referenced by the bytecode + its VM register number.
struct VMGlobalRef {
    GlobalVariable *GV;
    unsigned        Reg;
};

static unsigned genBytecode(Function *F, std::vector<uint8_t> &BC,
                            std::vector<std::string> &FuncNames,
                            DenseMap<StringRef, unsigned> &FuncNameMap,
                            std::vector<VMGlobalRef> &Globals) {
  // --- Phase 0: count operand uses for liveness ---
  DenseMap<Value *, unsigned> UseCount;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      for (auto &Op : I.operands()) {
        if (auto *OpI = dyn_cast<Instruction>(Op)) {
          if (!OpI->getType()->isVoidTy() && !isa<AllocaInst>(OpI))
            UseCount[OpI]++;
        }
      }
    }
  }

  // --- Phase 1: register allocation (function args only) ---
  RegisterAllocator Regs;
  Regs.UseCount = &UseCount;

  {
    unsigned ArgIdx = 0;
    for (auto &Arg : F->args()) {
      if (ArgIdx >= 8)
        break;
      Regs.setArg(ArgIdx, &Arg);
      ArgIdx++;
    }
  }

  // Global variable register mapping.
  // Globals get a dedicated register at the tail of the register file, one
  // per unique GlobalVariable referenced by the bytecode.  We allocate via
  // NextReg++ (bypassing FreeList) to avoid collisions with alloca/phi
  // registers that are drawn from FreeList early in translation.
  // insertVmpcall fills these registers with the runtime address of the
  // global before VMExecute runs.
  DenseMap<GlobalVariable *, unsigned> GlobalRegMap;
  auto getGlobalReg = [&](GlobalVariable *GV) -> unsigned {
    auto [It, New] = GlobalRegMap.try_emplace(GV, 0);
    if (New) {
      unsigned reg = Regs.NextReg++;
      if (reg + 1 > Regs.MaxReg) Regs.MaxReg = reg + 1;
      It->second = reg;
      Globals.push_back({GV, reg});
    }
    return It->second;
  };

  // --- Phase 2: translate to bytecode ---
  // BB layout order index (used to skip redundant JMP to next block)
  DenseMap<BasicBlock *, unsigned> BBOrder;
  unsigned BIdx = 0;
  for (auto &BB : *F)
    BBOrder[&BB] = BIdx++;

  // Pre-allocate phi node registers BEFORE Phase 2 main loop, so that phi
  // lowering at predecessor branches (which happens when the predecessor BB is
  // processed, earlier than the phi's own BB) can look up the correct register.
  for (auto &BB : *F)
    for (auto &I : BB)
      if (isa<PHINode>(&I))
        Regs.alloc(&I);

  // Track actual BB bytecode offsets (recorded at first non-PHI of each BB)
  DenseMap<BasicBlock *, uint32_t> ActualBBOffset;
  // Branch patches: placeholder offsets filled in after Phase 2
  struct BranchPatch {
    unsigned InstrStart; // BC.size() before emitInsn
    BasicBlock *Target;
    bool IsBr;           // false=JMP (src1@+4), true=BR (src2@+6)
  };
  std::vector<BranchPatch> Patches;
  // Invoke patches: VM_INVOKE_PREP has a placeholder 32-bit absolute offset
  // that is filled with the unwind destination's bytecode offset in Phase 3.
  struct InvokePatch {
    unsigned InstrStart;    // BC.size() before emitInsn of VM_INVOKE_PREP
    BasicBlock *Target;     // unwind destination BB
  };
  std::vector<InvokePatch> InvokePatches;

  const DataLayout &DL = F->getParent()->getDataLayout();
  for (auto &BB : *F) {
    Regs.CurrentBB = &BB;
    ActualBBOffset[&BB] = BC.size();
    for (auto &I : BB) {
      if (isa<PHINode>(&I)) {
        continue; // already allocated in pre-pass above
      } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        unsigned RDst = Regs.alloc(AI);
        uint64_t AllocSize = DL.getTypeAllocSize(AI->getAllocatedType());
        emitInsn(BC, VM_ALLOCA, RDst, 0, static_cast<uint16_t>(AllocSize));
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *ValOp = SI->getValueOperand();
        Value *PtrOp = SI->getPointerOperand();
        // Resolve value: handle constants (int → LI, float → LI32)
        unsigned RVal;
        if (auto *CI = dyn_cast<ConstantInt>(ValOp)) {
          RVal = Regs.allocRaw();
          emitInsn(BC, VM_LI, RVal, 0, static_cast<uint16_t>(CI->getZExtValue()));
        } else if (auto *CF = dyn_cast<ConstantFP>(ValOp)) {
          RVal = Regs.allocRaw();
          uint32_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RVal, Bits);
        } else {
          RVal = Regs.consume(ValOp);
        }
        unsigned StoreSize = DL.getTypeStoreSize(ValOp->getType());
        uint8_t SFlags = (StoreSize - 1) & 0x0F;        // bits 0-3: size-1

        // All addresses are real pointers after ALLOCA simplification.
        unsigned RAddr;
        if (auto *GV = dyn_cast<GlobalVariable>(PtrOp->stripPointerCasts()))
          RAddr = getGlobalReg(GV);
        else
          RAddr = Regs.consume(PtrOp);
        emitInsn(BC, VM_STORE, RAddr, RVal, 0, SFlags);
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *PtrOp = LI->getPointerOperand();
        unsigned RDst = Regs.alloc(&I);
        unsigned LoadSize = DL.getTypeStoreSize(LI->getType());
        uint8_t LFlags = (LoadSize - 1) & 0x0F;        // bits 0-3: size-1
        // Float loads set bit 3 to prevent LOAD from sign-extending.
        if (LI->getType()->isFloatTy()) LFlags |= VM_FLAG_FLOAT;

        // All addresses are real pointers after ALLOCA simplification.
        unsigned RAddr;
        if (auto *GV = dyn_cast<GlobalVariable>(PtrOp->stripPointerCasts()))
          RAddr = getGlobalReg(GV);
        else
          RAddr = Regs.consume(PtrOp);
        emitInsn(BC, VM_LOAD, RDst, RAddr, 0, LFlags);
      } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        // GEP: compute pointer = base + byte_offset
        Value *PtrOp = GEP->getPointerOperand();
        APInt Offset(DL.getPointerSizeInBits(), 0);

        // When the pointer operand is a GlobalVariable, use a dedicated global
        // register (filled with the runtime address by VMExecute init).  The
        // normal RegisterAllocator has no entry for globals, so lookupReg
        // would return 0 (r0) which is wrong.
        bool IsGlobal = false;
        unsigned RBase;
        if (auto *GV = dyn_cast<GlobalVariable>(PtrOp->stripPointerCasts())) {
          RBase = getGlobalReg(GV);
          IsGlobal = true;
        } else {
          RBase = Regs.lookupReg(PtrOp);
        }

        if (GEP->accumulateConstantOffset(DL, Offset)) {
          if (Offset == 0) {
            if (IsGlobal) {
              // Global + 0: MOV the global address to the GEP result register
              unsigned RDst = Regs.alloc(&I);
              emitInsn(BC, VM_MOV, RDst, RBase, 0);
            } else {
              // Zero offset → alias to base register (no bytecode emitted)
              Regs.aliasValue(&I, PtrOp);
              Regs.consume(PtrOp);
            }
          } else {
            // Non-zero offset: rdst = rbase + #offset
            unsigned RDst = Regs.alloc(&I);
            emitInsn(BC, VM_ADD, RDst, RBase,
                     static_cast<uint16_t>(Offset.getZExtValue()),
                     VM_FLAG_IMM);
            if (!IsGlobal) Regs.consume(PtrOp);
          }
        } else {
          // Variable-offset GEP: assemble pointer = base + sum(idx_i * elem_size_i)
          unsigned RDst = Regs.alloc(&I);
	  unsigned RAcc = Regs.allocRaw();
          emitInsn(BC, VM_MOV, RAcc, RBase, 0);
          if (!IsGlobal) Regs.consume(PtrOp);

          gep_type_iterator GTI = gep_type_begin(GEP);
          for (unsigned IdxNo = 1; IdxNo < GEP->getNumOperands(); ++IdxNo, ++GTI) {
            Value *Idx = GEP->getOperand(IdxNo);
	    uint64_t ElemSize = DL.getTypeAllocSize(GTI.getIndexedType());

            if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
              uint64_t Off = CI->getZExtValue() * ElemSize;
              if (Off == 0) continue;
              if (Off <= 0xFFFF) {
                emitInsn(BC, VM_ADD, RAcc, RAcc,
                         static_cast<uint16_t>(Off), VM_FLAG_IMM);
              } else {
                unsigned RTmp = Regs.allocRaw();
                emitInsn32(BC, VM_LI32, RTmp, static_cast<uint32_t>(Off));
                emitInsn(BC, VM_ADD, RAcc, RAcc, RTmp, 0);
                Regs.freeReg(RTmp);
              }
            } else {
              unsigned RIdx = Regs.consume(Idx);
              if (ElemSize == 1) {
                emitInsn(BC, VM_ADD, RAcc, RAcc, RIdx, 0);
              } else if (ElemSize <= 0xFFFF) {
                unsigned RTmp = Regs.allocRaw();
                emitInsn(BC, VM_LI, RTmp, 0, static_cast<uint16_t>(ElemSize));
                emitInsn(BC, VM_MUL, RTmp, RIdx, RTmp, 0);
                emitInsn(BC, VM_ADD, RAcc, RAcc, RTmp, 0);
                Regs.freeReg(RTmp);
              }
            }
          }

          emitInsn(BC, VM_MOV, RDst, RAcc, 0);
          Regs.freeReg(RAcc);
        }
      } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        // Resolve src1: handle int/float constants
        unsigned RSrc1;
        bool Src1IsConst = false;
        if (auto *CI = dyn_cast<ConstantInt>(BO->getOperand(0))) {
          RSrc1 = Regs.allocRaw();
          Src1IsConst = true;
          emitInsn(BC, VM_LI, RSrc1, 0, static_cast<uint16_t>(CI->getZExtValue()));
        } else if (auto *CF = dyn_cast<ConstantFP>(BO->getOperand(0))) {
          RSrc1 = Regs.allocRaw();
          Src1IsConst = true;
          uint32_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RSrc1, Bits);
        } else {
          RSrc1 = Regs.consume(BO->getOperand(0));
        }
        // Resolve src2: int constant → IMM, float constant → LI32
        uint8_t ArithFlags = 0;
        unsigned RSrc2;
        bool Src2IsFP = false;
        if (auto *CI = dyn_cast<ConstantInt>(BO->getOperand(1))) {
          RSrc2 = static_cast<unsigned>(CI->getZExtValue());
          ArithFlags |= VM_FLAG_IMM;
        } else if (auto *CF = dyn_cast<ConstantFP>(BO->getOperand(1))) {
          RSrc2 = Regs.allocRaw();
          Src2IsFP = true;
          uint32_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RSrc2, Bits);
        } else {
          RSrc2 = Regs.consume(BO->getOperand(1));
        }

        unsigned RDst = Regs.alloc(&I);
        // Float double-width: bit 0 = 1 for double-precision ops
        if (I.getType()->isDoubleTy()) ArithFlags |= 1;

        switch (BO->getOpcode()) {
        case Instruction::Add:
          emitInsn(BC, VM_ADD, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::FAdd:
          emitInsn(BC, VM_FADD, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Sub:
          emitInsn(BC, VM_SUB, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::FSub:
          emitInsn(BC, VM_FSUB, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Mul:
          emitInsn(BC, VM_MUL, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::FMul:
          emitInsn(BC, VM_FMUL, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::UDiv:
          emitInsn(BC, VM_UDIV, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::SDiv:
          emitInsn(BC, VM_SDIV, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::FDiv:
          emitInsn(BC, VM_FDIV, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::URem:
          emitInsn(BC, VM_UREM, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::SRem:
          emitInsn(BC, VM_SREM, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Shl:
          emitInsn(BC, VM_SHL, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::LShr:
          emitInsn(BC, VM_LSHR, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::AShr:
          emitInsn(BC, VM_ASHR, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::And:
          emitInsn(BC, VM_AND, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Or:
          emitInsn(BC, VM_OR, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Xor:
          emitInsn(BC, VM_XOR, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        default: {
          std::string Msg =
              "[VMCodeGen] unsupported binary op: " +
              std::string(BO->getOpcodeName()) + "\n";
          report_fatal_error(StringRef(Msg));
        }
        }
        // Free LI-allocated constant source registers
        if (Src1IsConst) Regs.freeReg(RSrc1);
        if (Src2IsFP) Regs.freeReg(RSrc2);
      } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        unsigned RVal = 0;
        if (RI->getReturnValue()) {
          Value *RetVal = RI->getReturnValue();
          if (auto *CI = dyn_cast<ConstantInt>(RetVal)) {
            RVal = Regs.allocRaw();
            emitInsn(BC, VM_LI, RVal, 0, static_cast<uint16_t>(CI->getZExtValue()));
          } else if (auto *CF = dyn_cast<ConstantFP>(RetVal)) {
            RVal = Regs.allocRaw();
            uint32_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
            emitInsn32(BC, VM_LI32, RVal, Bits);
          } else {
            RVal = Regs.consume(RetVal);
          }
        }
        emitInsn(BC, VM_RET, RVal, 0, 0);
      } else if (auto *CI = dyn_cast<CastInst>(&I)) {
        unsigned RDst = Regs.alloc(&I);
        // Resolve source operand (may be ConstantInt/ConstantFP)
        unsigned RSrc;
        bool SrcIsConst = false;
        if (auto *CInt = dyn_cast<ConstantInt>(CI->getOperand(0))) {
          RSrc = Regs.allocRaw();
          SrcIsConst = true;
          emitInsn(BC, VM_LI, RSrc, 0, static_cast<uint16_t>(CInt->getZExtValue()));
        } else if (auto *CFP = dyn_cast<ConstantFP>(CI->getOperand(0))) {
          RSrc = Regs.allocRaw();
          SrcIsConst = true;
          uint32_t Bits = CFP->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RSrc, Bits);
        } else {
          RSrc = Regs.consume(CI->getOperand(0));
        }
        switch (CI->getOpcode()) {
        case Instruction::SIToFP: {
          uint8_t CF = CI->getDestTy()->isDoubleTy() ? 1 : 0;
          emitInsn(BC, VM_SITOFP, RDst, RSrc, 0, CF);
          break;
        }
        case Instruction::FPToSI: {
          uint8_t CF = CI->getSrcTy()->isDoubleTy() ? 1 : 0;
          emitInsn(BC, VM_FPTOSI, RDst, RSrc, 0, CF);
          break;
        }
        case Instruction::FPTrunc:
          emitInsn(BC, VM_FPTRUNC, RDst, RSrc, 0);
          break;
        case Instruction::FPExt:
          emitInsn(BC, VM_FPEXT, RDst, RSrc, 0);
          break;
        case Instruction::SExt:
          emitInsn(BC, VM_SEXT, RDst, RSrc, 0);
          break;
        case Instruction::ZExt:
          emitInsn(BC, VM_ZEXT, RDst, RSrc, 0);
          break;
        case Instruction::Trunc:
          emitInsn(BC, VM_TRUNC, RDst, RSrc, 0);
          break;
        case Instruction::UIToFP: {
          uint8_t CF = CI->getDestTy()->isDoubleTy() ? 1 : 0;
          emitInsn(BC, VM_UITOFP, RDst, RSrc, 0, CF);
          break;
        }
        case Instruction::FPToUI: {
          uint8_t CF = CI->getSrcTy()->isDoubleTy() ? 1 : 0;
          emitInsn(BC, VM_FPTOUI, RDst, RSrc, 0, CF);
          break;
        }
        case Instruction::PtrToInt:
        case Instruction::IntToPtr:
        case Instruction::BitCast:
          // All are no-ops in the VM → pointers, ints, and bit patterns are all
          // stored as uintptr_t in registers. Just copy the register value.
          emitInsn(BC, VM_MOV, RDst, RSrc, 0);
          break;
        default:
          report_fatal_error(
              Twine("[VMCodeGen] unsupported cast: ") +
              CI->getOpcodeName() + "\n");
        }
        if (SrcIsConst) Regs.freeReg(RSrc);
      } else if (isa<ICmpInst>(&I)) {
        auto *IC = cast<ICmpInst>(&I);
        unsigned RDst = Regs.alloc(&I);
        // Pre-allocate constant temps BEFORE consuming, so they can't
        // conflict with freed registers (consume → allocRaw reuse bug).
        bool S1C = isa<ConstantInt>(IC->getOperand(0)) || isa<ConstantPointerNull>(IC->getOperand(0));
        bool S2C = isa<ConstantInt>(IC->getOperand(1)) || isa<ConstantPointerNull>(IC->getOperand(1));
        unsigned RSrc1 = S1C ? Regs.allocRaw() : 0;
        unsigned RSrc2 = S2C ? Regs.allocRaw() : 0;
        if (!S1C) RSrc1 = Regs.consume(IC->getOperand(0));
        if (!S2C) RSrc2 = Regs.consume(IC->getOperand(1));
        if (S1C)
          emitInsn(BC, VM_LI, RSrc1, 0,
                   static_cast<uint16_t>(isa<ConstantPointerNull>(IC->getOperand(0)) ? 0
                     : cast<ConstantInt>(IC->getOperand(0))->getZExtValue()));
        if (S2C)
          emitInsn(BC, VM_LI, RSrc2, 0,
                   static_cast<uint16_t>(isa<ConstantPointerNull>(IC->getOperand(1)) ? 0
                     : cast<ConstantInt>(IC->getOperand(1))->getZExtValue()));
        uint8_t PredEnc = mapICmpPred(IC->getPredicate());
        emitInsn(BC, VM_CMP, RDst, RSrc1, RSrc2, PredEnc);
        if (S1C) Regs.freeReg(RSrc1);
        if (S2C) Regs.freeReg(RSrc2);
      } else if (isa<FCmpInst>(&I)) {
        auto *FC = cast<FCmpInst>(&I);
        unsigned RDst = Regs.alloc(&I);
        bool S1C = isa<ConstantFP>(FC->getOperand(0));
        bool S2C = isa<ConstantFP>(FC->getOperand(1));
        unsigned RSrc1 = S1C ? Regs.allocRaw() : 0;
        unsigned RSrc2 = S2C ? Regs.allocRaw() : 0;
        if (!S1C) RSrc1 = Regs.consume(FC->getOperand(0));
        if (!S2C) RSrc2 = Regs.consume(FC->getOperand(1));
        if (S1C) {
          uint32_t Bits = cast<ConstantFP>(FC->getOperand(0))->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RSrc1, Bits);
        }
        if (S2C) {
          uint32_t Bits = cast<ConstantFP>(FC->getOperand(1))->getValueAPF().bitcastToAPInt().getZExtValue();
          emitInsn32(BC, VM_LI32, RSrc2, Bits);
        }
        uint8_t PredEnc = mapFCmpPred(FC->getPredicate());
        if (FC->getOperand(0)->getType()->isDoubleTy())
          PredEnc |= 0x10;  // bit 4 = double width
        emitInsn(BC, VM_FCMP, RDst, RSrc1, RSrc2, PredEnc);
        if (S1C) Regs.freeReg(RSrc1);
        if (S2C) Regs.freeReg(RSrc2);
      } else if (isa<UncondBrInst>(&I)) {
        auto *UBI = cast<UncondBrInst>(&I);
        // --- PHI lowering ---
        if (auto *Succ = UBI->getSuccessor())
          for (auto &SI : *Succ) {
            auto *PN = dyn_cast<PHINode>(&SI);
            if (!PN) break;
            Value *Incoming = PN->getIncomingValueForBlock(&BB);
            unsigned RPhi = Regs.lookupReg(PN);
            if (auto *CI = dyn_cast<ConstantInt>(Incoming)) {
              emitInsn(BC, VM_LI, RPhi, 0, static_cast<uint16_t>(CI->getZExtValue()));
            } else {
              unsigned RVal = Regs.consume(Incoming);
              emitInsn(BC, VM_MOV, RPhi, RVal, 0);
            }
          }
        // --- Emit JMP (skip if target is next block in layout) ---
        {
          BasicBlock *Target = UBI->getSuccessor();
          if (BBOrder[Target] != BBOrder[&BB] + 1) {
            unsigned InstrStart = BC.size();
            emitInsn(BC, VM_JMP, 0, 0, 0);
            Patches.push_back({InstrStart, Target, false});
          }
        }
      } else if (isa<CondBrInst>(&I)) {
        auto *CBI = cast<CondBrInst>(&I);
        // --- PHI lowering for both successors ---
        for (unsigned Si = 0; Si < 2; Si++) {
          BasicBlock *Succ = CBI->getSuccessor(Si);
          for (auto &SI : *Succ) {
            auto *PN = dyn_cast<PHINode>(&SI);
            if (!PN) break;
            Value *Incoming = PN->getIncomingValueForBlock(&BB);
            unsigned RPhi = Regs.lookupReg(PN);
            if (auto *CI = dyn_cast<ConstantInt>(Incoming)) {
              emitInsn(BC, VM_LI, RPhi, 0, static_cast<uint16_t>(CI->getZExtValue()));
            } else {
              unsigned RVal = Regs.consume(Incoming);
              emitInsn(BC, VM_MOV, RPhi, RVal, 0);
            }
          }
        }
        // --- Emit BR (taken) + JMP (not-taken) ---
        // Optimize: skip JMP when either target is the next block in layout.
        // When the true target is the next block, invert the BR condition
        // (VM_FLAG_BR_NT) so it jumps to the false target instead,
        // eliminating both the BR #+0 and the JMP.
        {
          Value *Cond = CBI->getCondition();
          unsigned RCond = Regs.consume(Cond);
          BasicBlock *TrueTarget = CBI->getSuccessor(0);
          BasicBlock *FalseTarget = CBI->getSuccessor(1);
          bool NextIsTrue  = (BBOrder[TrueTarget]  == BBOrder[&BB] + 1);
          bool NextIsFalse = (BBOrder[FalseTarget] == BBOrder[&BB] + 1);

          if (NextIsTrue) {
            // Bridge to false target — invert condition flag
            unsigned BrStart = BC.size();
            emitInsn(BC, VM_BR, 0, RCond, 0, VM_FLAG_BR_NT);
            Patches.push_back({BrStart, FalseTarget, true});
          } else {
            unsigned BrStart = BC.size();
            emitInsn(BC, VM_BR, 0, RCond, 0);
            Patches.push_back({BrStart, TrueTarget, true});
            if (!NextIsFalse) {
              unsigned JmpStart = BC.size();
              emitInsn(BC, VM_JMP, 0, 0, 0);
              Patches.push_back({JmpStart, FalseTarget, false});
            }
          }
        }
      } else if (isa<SwitchInst>(&I)) {
        auto *SW = cast<SwitchInst>(&I);
        Value *Cond = SW->getCondition();
        unsigned RCond = Regs.consume(Cond);
        BasicBlock *DefaultBB = SW->getDefaultDest();

        // PHI lowering for each successor (including default)
        auto lowerPhi = [&](BasicBlock *Succ) {
          for (auto &SI : *Succ) {
            auto *PN = dyn_cast<PHINode>(&SI);
            if (!PN) break;
            Value *Incoming = PN->getIncomingValueForBlock(&BB);
            unsigned RPhi = Regs.lookupReg(PN);
            if (auto *CI = dyn_cast<ConstantInt>(Incoming)) {
              emitInsn(BC, VM_LI, RPhi, 0, static_cast<uint16_t>(CI->getZExtValue()));
            } else {
              unsigned RVal = Regs.consume(Incoming);
              emitInsn(BC, VM_MOV, RPhi, RVal, 0);
            }
          }
        };

        // Emit CMP + BR for each case: compare Cond == case_val, jump on EQ
        for (auto &C : SW->cases()) {
          ConstantInt *CV = C.getCaseValue();
          unsigned RVal = Regs.allocRaw();
          emitInsn(BC, VM_LI, RVal, 0, static_cast<uint16_t>(CV->getZExtValue()));
          unsigned RCmp = Regs.allocRaw();
          emitInsn(BC, VM_CMP, RCmp, RCond, RVal, 0);  // pred=0 = EQ
          Regs.freeReg(RVal);
          Regs.freeReg(RCmp);
          // Allocate RCmp before consume so it doesn't collide

          BasicBlock *Target = C.getCaseSuccessor();
          lowerPhi(Target);

          unsigned BrStart = BC.size();
          emitInsn(BC, VM_BR, 0, RCmp, 0);
          Patches.push_back({BrStart, Target, true});
        }

        // Default: jump to default BB
        lowerPhi(DefaultBB);
        if (BBOrder[DefaultBB] != BBOrder[&BB] + 1) {
          unsigned JmpStart = BC.size();
          emitInsn(BC, VM_JMP, 0, 0, 0);
          Patches.push_back({JmpStart, DefaultBB, false});
        }
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        // CallBrInst (asm goto) is not supported
        if (isa<CallBrInst>(CB))
          report_fatal_error("[VMCodeGen] CallBrInst (asm goto) not supported\n");

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          report_fatal_error("[VMCodeGen] indirect calls not supported\n");

        // Skip lifetime intrinsics (llvm.lifetime.start/end) — these are no-ops
        // in the VM and should not be linked as external function calls.
        // They are generated by the optimizer at -O3 to mark variable lifetimes.
        if (Callee->isIntrinsic() &&
            Callee->getName().starts_with("llvm.lifetime.")) {
          // Consume operands to maintain register tracking correctness.
          for (Value *Op : I.operands()) {
            if (isa<Instruction>(Op) || isa<Argument>(Op))
              Regs.consume(Op);
          }
          continue;
        }

        // Map LLVM memory intrinsics to their libc equivalents.
        // Intrinsics like @llvm.memcpy cannot be called through a function pointer
        // at runtime and would leave dangling ConstantExpr references in the function
        // table, causing crashes in later passes (e.g. PreISelIntrinsicLowering).
        if (Callee->isIntrinsic()) {
          StringRef Replacement;
          switch (Callee->getIntrinsicID()) {
          case Intrinsic::memcpy:
          case Intrinsic::memcpy_inline:
            Replacement = "memcpy"; break;
          case Intrinsic::memmove:
            Replacement = "memmove"; break;
          case Intrinsic::memset:
          case Intrinsic::memset_inline:
            Replacement = "memset"; break;
          default:
            report_fatal_error(
                Twine("[VMCodeGen] unsupported intrinsic in VMP function: ") +
                Callee->getName() + "\n");
          }
          // Replace with the real libc function so it can be called at runtime.
          Callee = cast<Function>(
              F->getParent()
                  ->getOrInsertFunction(Replacement, CB->getFunctionType())
                  .getCallee());
        }

        unsigned NArgs = CB->arg_size();
        auto It = FuncNameMap.find(Callee->getName());
        if (It == FuncNameMap.end()) {
          It = FuncNameMap.insert({Callee->getName(), FuncNames.size()}).first;
          FuncNames.push_back(Callee->getName().str());
        }
        unsigned FuncIdx = It->second;
        // Determine arg types for CALL dispatch mode
        bool AllInt = true, AllFP = true;
        uint16_t ArgMask = 0;  // bit i = 1 if arg i is float/double
        for (unsigned i = 0; i < NArgs && i < 8; i++) {
          Type *T = CB->getArgOperand(i)->getType();
          if (T->isFloatTy() || T->isDoubleTy()) { AllInt = false; ArgMask |= (1 << i); }
          else AllFP = false;
        }
        bool RetFP = CB->getType()->isFloatTy() || CB->getType()->isDoubleTy();
        // Select CALL flags:
        //   bit 0: VM_CALL_RET_FP  — 1=fp return (XMM0)
        //   bit 1: VM_CALL_ARG_FP  — 1=all fp args (FPVMCallFn)
        //   bit 2: VM_CALL_ARG_MIX — 1=mixed int+fp args → libffi
        //   bit 3: VM_CALL_INVOKE   — 1=invoke (try/catch wrap in VM)
        uint8_t CallFlags = (RetFP ? VM_CALL_RET_FP : 0);
        if (!AllInt && !AllFP) {
          CallFlags |= VM_CALL_ARG_MIX;
        } else if (AllFP) {
          CallFlags |= VM_CALL_ARG_FP;    // pure fp → FPVMCallFn
        }
        // else pure int → VMCallFn or DblRetCallFn (based on ret type)

        // For invoke: emit VM_INVOKE_PREP to record unwind target offset.
        // The actual absolute offset is patched in Phase 3.
        if (auto *II = dyn_cast<InvokeInst>(CB)) {
          unsigned InvokePrepStart = BC.size();
          emitInsn(BC, VM_INVOKE_PREP, 0, 0, 0);
          InvokePatches.push_back({InvokePrepStart, II->getUnwindDest()});
          CallFlags |= VM_CALL_INVOKE;
        }

        bool IsVarArg = Callee->isVarArg();
        for (unsigned i = 0; i < NArgs && i < 8; i++) {
          unsigned RArg;
          Type *ArgTy = CB->getArgOperand(i)->getType();
          bool IsFP = ArgTy->isFloatTy() || ArgTy->isDoubleTy();
          uint8_t ArgFlags = IsFP ? 1 : 0;
          if (auto *CInt = dyn_cast<ConstantInt>(CB->getArgOperand(i))) {
            RArg = Regs.allocRaw();
            emitInsn(BC, VM_LI, RArg, 0, static_cast<uint16_t>(CInt->getZExtValue()));
          } else if (auto *CFP = dyn_cast<ConstantFP>(CB->getArgOperand(i))) {
            RArg = Regs.allocRaw();
            uint32_t Bits = CFP->getValueAPF().bitcastToAPInt().getZExtValue();
            emitInsn32(BC, VM_LI32, RArg, Bits);
          } else if (auto *GV = dyn_cast<GlobalVariable>(
                         CB->getArgOperand(i)->stripPointerCasts())) {
            // LLVM may optimise away a zero-offset GEP and use the global
            // variable directly.  Assign a global register for its address.
            RArg = getGlobalReg(GV);
          } else {
            RArg = Regs.consume(CB->getArgOperand(i));
          }
          emitInsn(BC, VM_SETARG, i, RArg, 0, ArgFlags);
          // Variadic functions need fp args in BOTH XMM and GP slots
          if (IsVarArg && IsFP)
            emitInsn(BC, VM_SETARG, i, RArg, 0, 0);  // also → call_args[i]
        }
        unsigned RetReg = CB->getType()->isVoidTy() ? 0 : Regs.alloc(&I);
        // Encode args info in bytes 6-7 for libffi (mixed mode):
        //   byte 6 low nibble  = arg_count (clamped to 8)
        //   byte 6 high nibble = fixed_arg_count (for variadic, else == arg_count)
        //   byte 7 = type_mask (bit i = float arg i)
        uint16_t ArgCount = (NArgs > 8) ? 8 : NArgs;
        unsigned NumFixed = Callee->getFunctionType()->getNumParams();
        uint16_t FixedCount = (NumFixed > ArgCount) ? ArgCount : (uint16_t)NumFixed;
        uint16_t ArgInfo = (ArgCount & 0x0F) | ((FixedCount & 0x0F) << 4)
                         | ((ArgMask & 0xFF) << 8);
        emitInsn(BC, VM_CALL, RetReg, FuncIdx, ArgInfo, CallFlags);

        // Invoke-specific post-call handling:
        //   - PHI lowering for the normal destination
        //   - JMP to normal destination if not fall-through
        if (auto *II = dyn_cast<InvokeInst>(CB)) {
          BasicBlock *NormalDest = II->getNormalDest();
          // --- PHI lowering (normal dest only; unwind path is dead) ---
          for (auto &SI : *NormalDest) {
            auto *PN = dyn_cast<PHINode>(&SI);
            if (!PN) break;
            Value *Incoming = PN->getIncomingValueForBlock(&BB);
            unsigned RPhi = Regs.lookupReg(PN);
            if (auto *CI = dyn_cast<ConstantInt>(Incoming)) {
              emitInsn(BC, VM_LI, RPhi, 0,
                       static_cast<uint16_t>(CI->getZExtValue()));
            } else {
              unsigned RVal = Regs.consume(Incoming);
              emitInsn(BC, VM_MOV, RPhi, RVal, 0);
            }
          }
          // --- JMP to normal dest if not next block in layout ---
          if (BBOrder[NormalDest] != BBOrder[&BB] + 1) {
            unsigned InstrStart = BC.size();
            emitInsn(BC, VM_JMP, 0, 0, 0);
            Patches.push_back({InstrStart, NormalDest, false});
          }
        }
      } else if (isa<LandingPadInst>(&I)) {
        // Landingpad produces a {ptr, i32} aggregate.  Allocate one register
        // for the exception handle (VM_LPAD fills ctx.r[dst] with the
        // serialized std::exception_ptr).  The selector (index 1) is always 0
        // for cleanup landingpads and is handled by extractvalue → VM_LI 0.
        // The unwind path is dead in normal execution; LPAD is only reached
        // when an exception was caught by VM_CALL INVOKE.
        unsigned RDst = Regs.alloc(&I);
        emitInsn(BC, VM_LPAD, RDst, 0, 0);
      } else if (auto *EVI = dyn_cast<ExtractValueInst>(&I)) {
        // extractvalue from a {ptr, i32} aggregate (landingpad result).
        // Index 0 = exception pointer, index 1 = selector (always 0 for cleanup).
        Value *Agg = EVI->getAggregateOperand();
        unsigned RDst = Regs.alloc(&I);
        unsigned RAgg = Regs.consume(Agg);
        auto Indices = EVI->getIndices();
        if (Indices.size() == 1 && Indices[0] == 1) {
          // Selector field: always 0 for cleanup-only landingpads.
          emitInsn(BC, VM_LI, RDst, 0, 0);
        } else {
          // Exception pointer field (index 0): RAgg holds the serialized
          // std::exception_ptr stored by VM_LPAD.
          emitInsn(BC, VM_MOV, RDst, RAgg, 0);
        }
      } else if (auto *IVI = dyn_cast<InsertValueInst>(&I)) {
        // insertvalue into a {ptr, i32} aggregate (used before resume).
        // Propagate the exception handle from the source aggregate.
        Value *Agg = IVI->getAggregateOperand();
        Value *Val = IVI->getInsertedValueOperand();
        unsigned RDst = Regs.alloc(&I);
        // For index 0 (exception pointer): copy the inserted value.
        // For index 1 (selector): copy the previous aggregate (which already
        // has the exception pointer in its first field).
        auto Indices = IVI->getIndices();
        if (Indices.size() == 1 && Indices[0] == 0) {
          // Insert exception pointer into a new aggregate (poison base).
          unsigned RVal = Regs.consume(Val);
          emitInsn(BC, VM_MOV, RDst, RVal, 0);
        } else {
          // Index 1: copy the aggregate from the previous insertvalue result.
          unsigned RAgg = Regs.consume(Agg);
          Regs.consume(Val);
          emitInsn(BC, VM_MOV, RDst, RAgg, 0);
        }
      } else if (isa<ResumeInst>(&I)) {
        // resume {ptr, i32}: rethrow the exception stored in the register.
        // The operand is the aggregate value containing the serialized
        // std::exception_ptr.  We just consume it and emit VM_RESUME.
        Value *ExcVal = cast<ResumeInst>(&I)->getValue();
        unsigned RExc = Regs.consume(ExcVal);
        emitInsn(BC, VM_RESUME, 0, RExc, 0);
      } else if (isa<UnreachableInst>(&I)) {
        // unreachable: this code path is dead (e.g. after __cxa_throw).
        // Emit nothing — control flow will never reach here at runtime.
      } else {
        report_fatal_error(
            Twine("[VMCodeGen] unsupported instruction: ") +
            I.getOpcodeName() + "\n");
      }
    }
  }

  // --- Phase 3: patch branch target offsets ---
  for (auto &P : Patches) {
    int32_t Diff = (int32_t)ActualBBOffset[P.Target] - (int32_t)(P.InstrStart + 8);
    if (Diff < -32768 || Diff > 32767)
      report_fatal_error("[VMCodeGen] branch target out of 16-bit range\n");
    uint16_t Enc = (uint16_t)(int16_t)Diff;
    unsigned Pos = P.IsBr ? P.InstrStart + 6 : P.InstrStart + 4;
    BC[Pos] = Enc & 0xFF;
    BC[Pos + 1] = (Enc >> 8) & 0xFF;
  }

  // --- Patch invoke unwind targets ---
  for (auto &P : InvokePatches) {
    uint32_t TargetOff = ActualBBOffset[P.Target];
    // VM_INVOKE_PREP uses dst (bytes 2-3) and src1 (bytes 4-5) for the
    // 32-bit absolute bytecode offset of the unwind destination.
    BC[P.InstrStart + 2] = TargetOff & 0xFF;
    BC[P.InstrStart + 3] = (TargetOff >> 8) & 0xFF;
    BC[P.InstrStart + 4] = (TargetOff >> 16) & 0xFF;
    BC[P.InstrStart + 5] = (TargetOff >> 24) & 0xFF;
  }

  // Global registers were already allocated via NextReg++ inside getGlobalReg,
  // so NextReg / MaxReg already account for them.

  return Regs.MaxReg;
}

//===----------------------------------------------------------------------===//
// Insert VMExecute() call in the function
//===----------------------------------------------------------------------===//
static void insertVmpcall(Function *F) {
  LLVMContext &Ctx = F->getContext();
  Module *M = F->getParent();

  // Generate bytecode
  std::vector<uint8_t> BC;
  std::vector<std::string> FuncNames;
  DenseMap<StringRef, unsigned> FuncNameMap;
  std::vector<VMGlobalRef> Globals;
  unsigned MaxRegs = genBytecode(F, BC, FuncNames, FuncNameMap, Globals);

  if (BC.empty())
    return;

  // Create global byte array in the module
  Type *Int8Ty = Type::getInt8Ty(Ctx);
  auto *BCArrTy = ArrayType::get(Int8Ty, BC.size());
  Constant *BCInit = ConstantDataArray::get(Ctx, BC);
  auto *BCGV = new GlobalVariable(*M, BCArrTy, true,
                                  GlobalValue::PrivateLinkage, BCInit,
                                  ".vmp_bc");
  BCGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  // Pointer to first byte: GEP bytecode, 0, 0
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Constant *Zero = ConstantInt::get(Int32Ty, 0);
  Constant *Indices[] = {Zero, Zero};
  Constant *BCPtr = ConstantExpr::getInBoundsGetElementPtr(
      BCArrTy, BCGV, Indices);

  // Size as i32
  Constant *Size = ConstantInt::get(Int32Ty, BC.size());

  Type *Int8PtrTy = PointerType::get(Ctx, 0);

  // Create function table global (array of function pointers)
  Constant *FnTableGV = nullptr;
  unsigned FuncCount = FuncNames.size();
  if (FuncCount > 0) {
    std::vector<Constant *> FnPtrs;
    for (auto &Name : FuncNames) {
      FunctionCallee Callee = M->getOrInsertFunction(Name, Int8PtrTy);
      auto *CalleeC = cast<Constant>(Callee.getCallee());
      FnPtrs.push_back(ConstantExpr::getPointerCast(CalleeC, Int8PtrTy));
    }
    auto *FnArrTy = ArrayType::get(Int8PtrTy, FuncCount);
    Constant *FnInit = ConstantArray::get(FnArrTy, FnPtrs);
    auto *GV = new GlobalVariable(*M, FnArrTy, true,
                                  GlobalValue::PrivateLinkage, FnInit,
                                  ".vmp_fn_table");
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    Constant *FnIndices[] = {Zero, Zero};
    FnTableGV = ConstantExpr::getInBoundsGetElementPtr(FnArrTy, GV, FnIndices);
  } else {
    FnTableGV = Constant::getNullValue(Int8PtrTy);
  }
  const DataLayout &DL = M->getDataLayout();
  Type *IntPtrTy = DL.getIntPtrType(Ctx);
  Type *RetTy = F->getReturnType();
  Constant *FuncCountC = ConstantInt::get(Int32Ty, FuncCount);

  // Declare: i64 VMExecute(ptr, i32, i32, ptr, i32, ptr, i32)
  // The last two parameters are (global_init[], num_globals).
  // Returns uintptr_t — caller casts/bitcasts to actual return type.
  FunctionType *ExecFnTy = FunctionType::get(
      IntPtrTy,
      {Int8PtrTy, Int32Ty, Int32Ty, Int8PtrTy, Int32Ty, Int8PtrTy, Int32Ty},
      false);
  FunctionCallee VmExec = M->getOrInsertFunction("VMExecute", ExecFnTy);


  SmallVector<Type *, 8> EightPtrs(8, IntPtrTy);
  FunctionType *SaveFnTy = FunctionType::get(
      Type::getVoidTy(Ctx), EightPtrs, false);
  FunctionCallee VmSave = M->getOrInsertFunction("VMSaveReg", SaveFnTy);

  // --- Replace function body with VMSaveReg + VMExecute + return ---


  // Remove all existing basic blocks
  for (auto &BB : make_early_inc_range(*F)) {
    BB.dropAllReferences();
    BB.eraseFromParent();
  }

  // Create new entry block
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> B(Entry);

  // 1) VMSaveReg(arg0..arg7) — capture actual argument values as VM registers
  unsigned ArgIdx = 0;
  Value *RegArgs[8] = {};
  for (auto &Arg : F->args()) {
    if (ArgIdx >= 8)
      break;
    Value *CastArg = &Arg;
    if (Arg.getType()->isIntegerTy())
      CastArg = B.CreateZExt(&Arg, IntPtrTy);
    else if (Arg.getType()->isPointerTy())
      CastArg = B.CreatePtrToInt(&Arg, IntPtrTy);
    else if (Arg.getType()->isFloatingPointTy()) {
      Type *ArgTy = Arg.getType();
      if (ArgTy->isDoubleTy()) {
        // double (64-bit): bitcast to i64
        Value *Bits = B.CreateBitCast(&Arg, IntPtrTy);
        CastArg = Bits;
      } else if (ArgTy->isFloatTy()) {
        // float (32-bit): bitcast to i32, zero-extend to i64
        Value *Bits32 = B.CreateBitCast(&Arg, Int32Ty);
        Value *Bits64 = B.CreateZExt(Bits32, IntPtrTy);
        CastArg = Bits64;
      } else {
        // Other FP types (half/bfloat/etc.): preserve bit pattern through same-width integer
        unsigned BitWidth = ArgTy->getPrimitiveSizeInBits();
        Type *IntTy = Type::getIntNTy(Ctx, BitWidth);
        Value *Bits = B.CreateBitCast(&Arg, IntTy);
        if (BitWidth < (unsigned)IntPtrTy->getIntegerBitWidth())
          Bits = B.CreateZExt(Bits, IntPtrTy);
        CastArg = Bits;
      }
    } else {
      // Unsupported type — fall back to 0
      CastArg = ConstantInt::get(IntPtrTy, 0);
    }
    RegArgs[ArgIdx++] = CastArg;
  }
  while (ArgIdx < 8)
    RegArgs[ArgIdx++] = ConstantInt::get(IntPtrTy, 0);

  B.CreateCall(VmSave, RegArgs);

  // 2a) Build global variable address table (if any globals referenced)
  Value *GlobalInit = Constant::getNullValue(Int8PtrTy);
  Constant *NumGlobals = ConstantInt::get(Int32Ty, 0);
  if (!Globals.empty()) {
    NumGlobals = ConstantInt::get(Int32Ty, Globals.size());
    // Format: flat array of {reg_index, value} pairs.
    auto *GITy = ArrayType::get(IntPtrTy, Globals.size() * 2);
    AllocaInst *GIArr = B.CreateAlloca(GITy);
    for (size_t i = 0; i < Globals.size(); i++) {
      Value *Addr = B.CreatePtrToInt(Globals[i].GV, IntPtrTy);
      // Store register index
      Value *RegGEP = B.CreateInBoundsGEP(GITy, GIArr,
          {ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, (uint32_t)(i * 2))});
      B.CreateStore(ConstantInt::get(IntPtrTy, Globals[i].Reg), RegGEP);
      // Store address value
      Value *ValGEP = B.CreateInBoundsGEP(GITy, GIArr,
          {ConstantInt::get(Int32Ty, 0), ConstantInt::get(Int32Ty, (uint32_t)(i * 2 + 1))});
      B.CreateStore(Addr, ValGEP);
    }
    GlobalInit = B.CreateBitCast(GIArr, Int8PtrTy);
  }

  // 2b) VMExecute(bytecode_ptr, size, nregs, func_table, func_count, global_init, num_globals)
  Constant *NRegs = ConstantInt::get(Int32Ty, MaxRegs);
  CallInst *Result = B.CreateCall(VmExec,
      {BCPtr, Size, NRegs, FnTableGV, FuncCountC, GlobalInit, NumGlobals});

  // 3) Cast and return — Result is IntPtrTy (uintptr_t)
  if (RetTy->isVoidTy()) {
    B.CreateRetVoid();
  } else if (RetTy->isPointerTy()) {
    B.CreateRet(B.CreateIntToPtr(Result, RetTy));
  } else if (RetTy->isIntegerTy()) {
    Value *V = Result;
    if (IntPtrTy != RetTy)
      V = B.CreateTrunc(V, RetTy);
    B.CreateRet(V);
  } else {
    // Float/double: trunc if needed → bitcast back to FP type
    Value *V = Result;
    unsigned RetBits = RetTy->getPrimitiveSizeInBits();
    if (RetBits < (unsigned)IntPtrTy->getIntegerBitWidth()) {
      Type *IntTy = Type::getIntNTy(Ctx, RetBits);
      V = B.CreateTrunc(V, IntTy);
    }
    B.CreateRet(B.CreateBitCast(V, RetTy));
  }
}

//===----------------------------------------------------------------------===//
// Pass entry point
//===----------------------------------------------------------------------===//
PreservedAnalyses VMCodeGenPass::run(Module &M,
                                     ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (!hasVMPAnnotation(&F, M))
      continue;

    LLVM_DEBUG(dbgs() << "[VMCodeGen] processing: " << F.getName() << "\n");
    insertVmpcall(&F);
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
