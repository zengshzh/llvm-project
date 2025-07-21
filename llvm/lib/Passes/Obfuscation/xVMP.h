#ifndef _OBFUSCATION_VMPROTECT_H_
#define _OBFUSCATION_VMPROTECT_H_
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO.h"
#include "Utils.h"
// #include "llvm/Transforms/Obfuscation/CryptoUtils.h"
namespace llvm{ // 基本块分割
    class VMProtectPass : public PassInfoMixin<VMProtectPass>{
        public:
            bool flag;
            VMProtectPass(bool flag){
                this->flag = flag;
            } // 携带flag的构造函数
            PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM); // Pass实现函数
            static bool isRequired() { return true; } // 直接返回true即可
    };
    VMProtectPass *createVMProtect(bool flag); // 创建基本块分割
}
extern bool is_interpreter_function(llvm::Function *targetFunction);
extern std::string get_vm_function_name(llvm::Function *targetFunction);
#endif