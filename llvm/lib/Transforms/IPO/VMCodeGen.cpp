//===-- VMCodeGen.cpp - Insert printf for VMP-annotated functions ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Detects functions annotated with __attribute__((annotate("VMP"))) via
// @llvm.global.annotations and inserts a call to vmprint() at their entry.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/VMCodeGen.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "vm-codegen"

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
    // Annotations have at least 4 operands: {fn, str, file, line, ...}
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

static void insertVmprint(Function *F) {
  LLVMContext &Ctx = F->getContext();
  Module *M = F->getParent();

  // Declare: void vmprint(void)
  FunctionType *FnTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  FunctionCallee Vmprint = M->getOrInsertFunction("vmprint", FnTy);

  // Insert vmprint() call at the function entry
  IRBuilder<> Builder(&F->getEntryBlock(), F->getEntryBlock().getFirstInsertionPt());
  Builder.CreateCall(Vmprint);
}

PreservedAnalyses VMCodeGenPass::run(Module &M,
                                     ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (!hasVMPAnnotation(&F, M))
      continue;

    insertVmprint(&F);
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
