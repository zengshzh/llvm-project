#ifndef LLVM_STRING_ENCRYPTION_H
#define LLVM_STRING_ENCRYPTION_H
// LLVM include
#include "llvm/Pass.h"
#include "llvm/IR/PassManager.h" // New PassManager

namespace llvm {
  class StringObfuscationPass : public PassInfoMixin<StringObfuscationPass>{ // New PassManager
  public:
    bool flag;
    StringObfuscationPass(bool flag){
        this->flag = flag;
    } // 携带flag的构造函数
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    static bool isRequired() { return true; }
  };
  StringObfuscationPass* createStringObfuscation(bool flag);
}

#endif