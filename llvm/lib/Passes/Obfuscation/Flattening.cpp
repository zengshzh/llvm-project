#include "Utils.h"
#include "CryptoUtils.h"
#include "Flattening.h"
#include "SplitBasicBlock.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
//#include "llvm/Transforms/Utils/LowerSwitch.h"
// namespace
using namespace llvm;
using std::vector;

#define DEBUG_TYPE "flattening" // 调试标识
// Stats
STATISTIC(Flattened, "Functions flattened");

PreservedAnalyses FlatteningPass::run(Function& F, FunctionAnalysisManager& AM) {
    Function *tmp = &F; // 传入的Function
    // 判断是否需要开启控制流平坦化
    if (toObfuscate(flag, tmp, "fla")) {
      INIT_CONTEXT(F);
      // outs()<<"[Soule] debug. "<< F.getName()<<" \n";
      if (flatten(*tmp)) {
        outs()<<"[obf] flattening. "<< F.getName()<<" \n";
        ++Flattened;
      }
      return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}


bool FlatteningPass::flatten(Function &F) {
    Function *f = &F;
    std::vector<BasicBlock *> origBB;
    BasicBlock *loopEntry;
    BasicBlock *loopEnd;
    LoadInst *load;
    SwitchInst *switchI;
    AllocaInst *switchVar;

    // SCRAMBLER
    char scrambling_key[16];
    llvm::cryptoutils->get_bytes(scrambling_key, 16);
    // END OF SCRAMBLER

  #if LLVM_VERSION_MAJOR >= 9
      // >=9.0, LowerSwitchPass depends on LazyValueInfoWrapperPass, which cause AssertError.
      // So I move LowerSwitchPass into register function, just before FlatteningPass.
  #else
    // Lower switch
    FunctionPass *lower = createLowerSwitchPass();
    lower->runOnFunction(*f);
  #endif

    // Skip functions with exception handling, indirect branches, or switch
    // instructions. Flattening them (moving EH pads / switch blocks into a
    // switch dispatch loop) produces invalid IR and can crash the compiler.
    if (F.hasPersonalityFn()) {
      return false;
    }
    for (BasicBlock &BB : F) {
      if (BB.isEHPad() || isa<InvokeInst>(BB.getTerminator()) ||
          isa<CatchSwitchInst>(BB.getTerminator()) ||
          isa<IndirectBrInst>(BB.getTerminator()) ||
          isa<SwitchInst>(BB.getTerminator())) {
        return false;
      }
    }

    // Save all original BB
    for (Function::iterator i = f->begin(); i != f->end(); ++i) {
      BasicBlock *tmp = &*i;
      origBB.push_back(tmp);
    }

    // Nothing to flatten
    if (origBB.size() <= 1) {
      return false;
    }

    // Remove first BB
    origBB.erase(origBB.begin());

    // Get a pointer on the first BB
    Function::iterator tmp = f->begin(); //++tmp;
    BasicBlock *insert = &*tmp;

    // If main begin with an if
    BranchInst *br = NULL;
    if (isa<BranchInst>(insert->getTerminator())) {
      br = cast<BranchInst>(insert->getTerminator());
    }

    if ((br != NULL && br->isConditional()) ||
        insert->getTerminator()->getNumSuccessors() > 1) {
      BasicBlock::iterator i = insert->end();
      --i;

      if (insert->size() > 1) {
        --i;
      }

      BasicBlock *tmpBB = insert->splitBasicBlock(i, "first");
      origBB.insert(origBB.begin(), tmpBB);
    }

    // 初始状态(scramble32(0))固定映射到 origBB[0]。当入口块是无条件跳转时，
    // 不会走上面的 split，origBB[0] 只是"布局序第二块"，并不等于入口真正的后继。
    // 若二者不一致（例如入口跳向循环头、布局上却紧跟着循环回边），压平后的
    // 状态机会先进到错误的块（回边），读取尚未初始化的循环变量，运行期崩溃
    // （get_root_detect_raw 的 su 路径循环即此类）。把入口真正的后继挪到最前，
    // 保证初始状态从入口后继开始执行。
    if (br != NULL && br->isUnconditional()) {
      BasicBlock *entryTarget = br->getSuccessor(0);
      auto It = std::find(origBB.begin(), origBB.end(), entryTarget);
      if (It != origBB.end()) {
        std::rotate(origBB.begin(), It, std::next(It));
      }
    }

    // Remove jump
    insert->getTerminator()->eraseFromParent();

    // Create switch variable and set as it
    switchVar =
        new AllocaInst(Type::getInt32Ty(f->getContext()), 0, "switchVar", insert);
    new StoreInst(
        ConstantInt::get(Type::getInt32Ty(f->getContext()),
                         llvm::cryptoutils->scramble32(0, scrambling_key)),
        switchVar, insert);

    // Create main loop
    loopEntry = BasicBlock::Create(f->getContext(), "loopEntry", f, insert);
    loopEnd = BasicBlock::Create(f->getContext(), "loopEnd", f, insert);

    load = new LoadInst(Type::getInt32Ty(f->getContext()), switchVar, "switchVar", loopEntry);

    // Move first BB on top
    insert->moveBefore(loopEntry);
    BranchInst::Create(loopEntry, insert);

    // loopEnd jump to loopEntry
    BranchInst::Create(loopEntry, loopEnd);

    BasicBlock *swDefault =
        BasicBlock::Create(f->getContext(), "switchDefault", f, loopEnd);
    BranchInst::Create(loopEnd, swDefault);

    // Create switch instruction itself and set condition
    switchI = SwitchInst::Create(&*f->begin(), swDefault, 0, loopEntry);
    switchI->setCondition(load);

    // Remove branch jump from 1st BB and make a jump to the while
    f->begin()->getTerminator()->eraseFromParent();

    BranchInst::Create(loopEntry, &*f->begin());

    // Put all BB in the switch
    for (std::vector<BasicBlock *>::iterator b = origBB.begin();
         b != origBB.end(); ++b) {
      BasicBlock *i = *b;
      ConstantInt *numCase = NULL;

      // Move the BB inside the switch (only visual, no code logic)
      i->moveBefore(loopEnd);

      // Add case to switch
      numCase = cast<ConstantInt>(ConstantInt::get(
          switchI->getCondition()->getType(),
          llvm::cryptoutils->scramble32(switchI->getNumCases(), scrambling_key)));
        switchI->addCase(numCase, i);
    }

    // Recalculate switchVar
    for (std::vector<BasicBlock *>::iterator b = origBB.begin();
         b != origBB.end(); ++b) {
      BasicBlock *i = *b;
      ConstantInt *numCase = NULL;

      // Ret BB
      if (i->getTerminator()->getNumSuccessors() == 0) {
        continue;
      }

      // If it's a non-conditional jump
      if (i->getTerminator()->getNumSuccessors() == 1) {
        // Get successor and delete terminator
        BasicBlock *succ = i->getTerminator()->getSuccessor(0);
        i->getTerminator()->eraseFromParent();

        // Get next case
        numCase = switchI->findCaseDest(succ);

        // If next case == default case (switchDefault)
        if (numCase == NULL) {
          numCase = cast<ConstantInt>(
              ConstantInt::get(switchI->getCondition()->getType(),
                               llvm::cryptoutils->scramble32(
                                   switchI->getNumCases() - 1, scrambling_key)));
        }

        // Update switchVar and jump to the end of loop
        new StoreInst(numCase, load->getPointerOperand(), i);
        BranchInst::Create(loopEnd, i);
        continue;
      }

      // If it's a conditional jump
      if (i->getTerminator()->getNumSuccessors() == 2) {
        // Get next cases
        ConstantInt *numCaseTrue =
            switchI->findCaseDest(i->getTerminator()->getSuccessor(0));
        ConstantInt *numCaseFalse =
            switchI->findCaseDest(i->getTerminator()->getSuccessor(1));

        // Check if next case == default case (switchDefault)
        if (numCaseTrue == NULL) {
          numCaseTrue = cast<ConstantInt>(
              ConstantInt::get(switchI->getCondition()->getType(),
                               llvm::cryptoutils->scramble32(
                                   switchI->getNumCases() - 1, scrambling_key)));
        }

        if (numCaseFalse == NULL) {
          numCaseFalse = cast<ConstantInt>(
              ConstantInt::get(switchI->getCondition()->getType(),
                               llvm::cryptoutils->scramble32(
                                   switchI->getNumCases() - 1, scrambling_key)));
        }

        // Create a SelectInst
        BranchInst *br = cast<BranchInst>(i->getTerminator());
        SelectInst *sel =
            SelectInst::Create(br->getCondition(), numCaseTrue, numCaseFalse, "",
                               i->getTerminator());

        // Erase terminator
        i->getTerminator()->eraseFromParent();

        // Update switchVar and jump to the end of loop
        new StoreInst(sel, load->getPointerOperand(), i);
        BranchInst::Create(loopEnd, i);
        continue;
      }
    }

    fixStack(F);

    // Verify the IR after flattening. If it is invalid, print the specific
    // error instead of crashing later in codegen.
    if (verifyFunction(F, &errs())) {
      errs() << "[fla] BAD IR after flattening " << F.getName() << "\n";
      F.eraseFromParent();
      return false;
    }

    return true;
  }


FlatteningPass *llvm::createFlattening(bool flag) {
    return new FlatteningPass(flag);
}
