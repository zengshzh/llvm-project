//===-- VMCodeGen.h - Insert printf for VMP-annotated functions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_VMCODEGEN_H
#define LLVM_TRANSFORMS_IPO_VMCODEGEN_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

struct VMCodeGenPass : public OptionalPassInfoMixin<VMCodeGenPass> {
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_VMCODEGEN_H
