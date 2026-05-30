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
#include "llvm/IR/DataLayout.h"
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

//===----------------------------------------------------------------------===//
// Translate IR function to VM bytecode
//===----------------------------------------------------------------------===//

// RegisterAllocator: dynamic register management with use-count recycling.
// r0-r7 reserved for function args.  All other registers allocated via
// free-list and automatically recycled when use count drops to zero.
struct RegisterAllocator {
  unsigned NextReg = 8; // r0-r7 reserved for function args
  unsigned MaxReg = 8;  // at least 8 for r0-r7
  DenseMap<Value *, unsigned> Map;
  SmallVector<unsigned> FreeList;
  DenseMap<Value *, unsigned> *UseCount;   // not owned
  DenseMap<Value *, Value *> AliasParent;  // GEP alias → base

  // Allocate a register and map it to value V (needed for lookups)
  unsigned alloc(Value *V) {
    unsigned r = allocRaw();
    Map[V] = r;
    return r;
  }

  // Allocate a temporary register (no Map entry, freed manually)
  unsigned allocRaw() {
    unsigned r;
    if (!FreeList.empty()) {
      r = FreeList.pop_back_val();
    } else {
      r = NextReg++;
    }
    if (r + 1 > MaxReg)
      MaxReg = r + 1;
    return r;
  }

  void freeReg(unsigned r) {
    FreeList.push_back(r);
  }

  // Consume an operand: decrement use count, auto-free when done
  unsigned consume(Value *V) {
    unsigned r = Map.lookup(V);
    Value *realV = V;
    while (AliasParent.count(realV))
      realV = AliasParent[realV];
    if (UseCount) {
      auto It = UseCount->find(realV);
      if (It != UseCount->end() && --It->second == 0)
        freeReg(r);
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

static unsigned genBytecode(Function *F, std::vector<uint8_t> &BC) {
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

  // --- Phase 2: translate to bytecode ---
  const DataLayout &DL = F->getParent()->getDataLayout();
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        unsigned RDst = Regs.alloc(AI);
        uint64_t AllocSize = DL.getTypeAllocSize(AI->getAllocatedType());
        emitInsn(BC, VM_ALLOCA, RDst, 0, static_cast<uint16_t>(AllocSize));
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *ValOp = SI->getValueOperand();
        Value *PtrOp = SI->getPointerOperand();
        // Resolve value: if ConstantInt, emit LI first
        unsigned RVal;
        if (auto *CI = dyn_cast<ConstantInt>(ValOp)) {
          RVal = Regs.allocRaw();
          emitInsn(BC, VM_LI, RVal, 0, static_cast<uint16_t>(CI->getZExtValue()));
        } else {
          RVal = Regs.consume(ValOp);
        }
        uint8_t SFlags = (DL.getTypeStoreSize(ValOp->getType()) > 4) ? 1 : 0;

        if (auto *AllocaPtr = dyn_cast<AllocaInst>(PtrOp->stripPointerCasts())) {
          unsigned RAddr = Regs.lookupReg(AllocaPtr);
          emitInsn(BC, VM_STORE, RAddr, RVal, 0, SFlags);
        } else {
          unsigned RAddr = Regs.consume(PtrOp);
          emitInsn(BC, VM_STORE, RAddr, RVal, 0, SFlags | 2);
        }
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *PtrOp = LI->getPointerOperand();
        unsigned RDst = Regs.alloc(&I);
        uint8_t LFlags = (DL.getTypeStoreSize(LI->getType()) > 4) ? 1 : 0;

        if (auto *AllocaPtr = dyn_cast<AllocaInst>(PtrOp->stripPointerCasts())) {
          unsigned RAddr = Regs.lookupReg(AllocaPtr);
          emitInsn(BC, VM_LOAD, RDst, RAddr, 0, LFlags);
        } else {
          unsigned RAddr = Regs.consume(PtrOp);
          emitInsn(BC, VM_LOAD, RDst, RAddr, 0, LFlags | 2);
        }
      } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        // GEP: compute pointer = base + byte_offset
        Value *PtrOp = GEP->getPointerOperand();
        APInt Offset(DL.getPointerSizeInBits(), 0);
        // Pre-allocate new regs BEFORE consume() so they can't collide
        unsigned RDst = Regs.alloc(&I);
        unsigned RTmp = Regs.allocRaw();
        unsigned RBase = Regs.consume(PtrOp);
        if (GEP->accumulateConstantOffset(DL, Offset)) {
          if (Offset == 0) {
            // Zero offset → alias to base register
            Regs.aliasValue(&I, PtrOp);
            Regs.freeReg(RDst);  // RDst not needed for alias
            Regs.freeReg(RTmp);
          } else {
            // Non-zero offset: rdst = rbase + offset
            emitInsn(BC, VM_LI, RTmp, 0, static_cast<uint16_t>(Offset.getZExtValue()));
            emitInsn(BC, VM_ADD, RDst, RBase, RTmp);
            Regs.freeReg(RTmp);
          }
        } else {
          LLVM_DEBUG(dbgs() << "[VMCodeGen] unsupported variable-offset GEP\n");
        }
      } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        // Resolve src1: if ConstantInt, emit LI then free after use
        unsigned RSrc1;
        bool Src1IsConst = false;
        if (auto *CI = dyn_cast<ConstantInt>(BO->getOperand(0))) {
          RSrc1 = Regs.allocRaw();
          Src1IsConst = true;
          emitInsn(BC, VM_LI, RSrc1, 0, static_cast<uint16_t>(CI->getZExtValue()));
        } else {
          RSrc1 = Regs.consume(BO->getOperand(0));
        }
        // Resolve src2: if ConstantInt, encode as immediate (flag bit 2)
        uint8_t ArithFlags = 0;
        unsigned RSrc2;
        if (auto *CI = dyn_cast<ConstantInt>(BO->getOperand(1))) {
          RSrc2 = static_cast<unsigned>(CI->getZExtValue());
          ArithFlags |= VM_FLAG_IMM;
        } else {
          RSrc2 = Regs.consume(BO->getOperand(1));
        }

        unsigned RDst = Regs.alloc(&I);

        switch (BO->getOpcode()) {
        case Instruction::Add:
        case Instruction::FAdd:
          emitInsn(BC, VM_ADD, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Sub:
        case Instruction::FSub:
          emitInsn(BC, VM_SUB, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::Mul:
        case Instruction::FMul:
          emitInsn(BC, VM_MUL, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::UDiv:
          emitInsn(BC, VM_UDIV, RDst, RSrc1, RSrc2, ArithFlags);
          break;
        case Instruction::SDiv:
          emitInsn(BC, VM_SDIV, RDst, RSrc1, RSrc2, ArithFlags);
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
      } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        unsigned RVal = 0;
        if (RI->getReturnValue()) {
          Value *RetVal = RI->getReturnValue();
          if (auto *CI = dyn_cast<ConstantInt>(RetVal)) {
            RVal = Regs.allocRaw();
            emitInsn(BC, VM_LI, RVal, 0, static_cast<uint16_t>(CI->getZExtValue()));
          } else {
            RVal = Regs.consume(RetVal);
          }
        }
        emitInsn(BC, VM_RET, RVal, 0, 0);
      } else {
        report_fatal_error(
            Twine("[VMCodeGen] unsupported instruction: ") +
            I.getOpcodeName() + "\n");
      }
    }
  }
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
  unsigned MaxRegs = genBytecode(F, BC);

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

  // Declare: void *VMExecute(ptr, i32, i32)
  FunctionType *ExecFnTy = FunctionType::get(
      Int8PtrTy, {Int8PtrTy, Int32Ty, Int32Ty}, false);
  FunctionCallee VmExec = M->getOrInsertFunction("VMExecute", ExecFnTy);

  // Declare: void VMSaveReg(ptr, ptr, ..., ptr)  — 8 register values
  SmallVector<Type *, 8> EightPtrs(8, Int8PtrTy);
  FunctionType *SaveFnTy = FunctionType::get(
      Type::getVoidTy(Ctx), EightPtrs, false);
  FunctionCallee VmSave = M->getOrInsertFunction("VMSaveReg", SaveFnTy);

  // --- Replace function body with VMSaveReg + VMExecute + return ---

  // Get return type and pointer-sized integer type for casting
  Type *RetTy = F->getReturnType();
  const DataLayout &DL = M->getDataLayout();
  Type *IntPtrTy = DL.getIntPtrType(Ctx);

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
      CastArg = B.CreateIntToPtr(&Arg, Int8PtrTy);
    else if (Arg.getType()->isPointerTy())
      CastArg = B.CreateBitCast(&Arg, Int8PtrTy);
    else
      CastArg = Constant::getNullValue(Int8PtrTy);
    RegArgs[ArgIdx++] = CastArg;
  }
  while (ArgIdx < 8)
    RegArgs[ArgIdx++] = Constant::getNullValue(Int8PtrTy);

  B.CreateCall(VmSave, RegArgs);

  // 2) VMExecute(bytecode_ptr, size, nregs)
  Constant *NRegs = ConstantInt::get(Int32Ty, MaxRegs);
  CallInst *Result = B.CreateCall(VmExec, {BCPtr, Size, NRegs});

  // 3) Cast and return
  if (RetTy->isVoidTy()) {
    B.CreateRetVoid();
  } else if (RetTy->isPointerTy()) {
    B.CreateRet(B.CreateBitCast(Result, RetTy));
  } else if (RetTy->isIntegerTy()) {
    Value *V = B.CreatePtrToInt(Result, IntPtrTy);
    if (IntPtrTy != RetTy)
      V = B.CreateTrunc(V, RetTy);
    B.CreateRet(V);
  } else {
    // For other types (float, etc.), go through intptr_t then bitcast
    Value *V = B.CreatePtrToInt(Result, IntPtrTy);
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
