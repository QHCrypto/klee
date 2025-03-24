#include "BoncController.h"
#include "ExecutionState.h"
#include "Executor.h"

#include "llvm-13/llvm/IR/InstrTypes.h"
#include "llvm/IR/BasicBlock.h"
#define LOG klee_message

namespace klee::bonc {

class BoncControllerImpl {
public:
  const ExecutionState *currentState;
  const llvm::Function *currentRoundFn;
  const llvm::BasicBlock *currentRoundBB;
  std::set<uint64_t> writtenObjectsInRound;

  BoncControllerImpl() : currentState{}, currentRoundFn{}, currentRoundBB{} {}

  void beforeEnterRoundFn(const Executor *executor, const ExecutionState &state,
                          const llvm::Function *f) {
    if (currentRoundFn) {
      LOG("Already in Round function %s; Calling another [[bonc::round]] (or "
          "recursively) is not supported.",
          currentRoundFn->getName().str().c_str());
      return;
    }
    if (currentRoundBB) {
      LOG("Already in Round loop; Calling another [[bonc::round]] is not "
          "supported.");
      return;
    }
    LOG("Entering Round function %s", f->getName().str().c_str());
    currentRoundFn = f;
    currentState = &state;
  }

  void beforeEnterRoundLoop(Executor *executor, ExecutionState &state,
                            const llvm::BasicBlock *bb) {
    if (currentRoundFn) {
      LOG("Already in Round function %s; Entering a [[bonc::round]] loop is "
          "not supported.",
          currentRoundFn->getName().str().c_str());
      return;
    }
    if (currentRoundBB) {
      if (currentRoundBB == bb) {
        LOG("Re-entering Round loop");
        trackRoundUpdate(executor, state);
      } else {
        LOG("Already in [[bonc::round]] loop; Entering another round loop is "
            "not supported.");
        return;
      }
    } else {
      LOG("Entering Round loop");
    }
    currentRoundBB = bb;
    currentState = &state;
  }

  void recordWrite(const Executor *executor, const ExecutionState &state,
                   const MemoryObject *mo, const ref<Expr> &value) {
    if (!(currentRoundFn || currentRoundBB)) {
      return;
    }
    assert(currentState == &state);
    // LOG("Write at address %" PRIx64 " with expression", mo->address);
    // value->print(llvm::errs());
    writtenObjectsInRound.insert(mo->address);
  }

  void afterExitRoundFn(Executor *executor, ExecutionState &state,
                        const llvm::Function *fn) {
    if (currentRoundFn != fn) {
      return;
    }
    assert(currentState == &state);
    LOG("Exiting Round function %s", fn->getName().str().c_str());

    trackRoundUpdate(executor, state);

    currentRoundFn = nullptr;
  }

  void afterExitRoundLoop(Executor *executor, ExecutionState &state,
                          const llvm::BasicBlock *bb) {
    if (currentRoundBB != bb) {
      return;
    }
    assert(currentState == &state);
    LOG("Exiting Round loop");

    trackRoundUpdate(executor, state);

    currentRoundBB = nullptr;
  }

  void trackRoundUpdate(Executor *executor, ExecutionState &state) {
    for (const auto addr : writtenObjectsInRound) {
      ObjectPair op;
      auto ret = state.addressSpace.resolveOne(
          ConstantExpr::create(addr, Expr::Int64), op);
      if (!ret) {
        // The written object has been freed (local variable)
        continue;
      }

      LOG("Object at address %" PRIx64 " was written in round and kept", addr);
      LOG("Size: %u", op.second->size);

      bool is_constant = true;

      for (auto offset = 0u; offset < op.second->size; offset++) {
        LOG("Byte at offset %u:", offset);
        auto expr = ConstraintManager::simplifyExpr(state.constraints,
                                                    op.second->read8(offset));
        if (!isa<ConstantExpr>(expr)) {
          is_constant = false;
        }
        // ConstraintManager::simplifyExpr(state.constraints,
        //                                 op.second->read8(offset))
        //     ->print(llvm::errs());
      }

      // Make it symbolic anyway
      if (!is_constant) {
        executor->executeMakeSymbolic(state, op.first,
                                      "bonc_added_after_round");
      }
    }
  }
};

BoncController::BoncController()
    : pImpl{std::make_unique<BoncControllerImpl>()} {}

BoncController::~BoncController() = default;

void BoncController::beforeEnterRoundFn(const Executor *executor,
                                        const ExecutionState &state,
                                        const llvm::Instruction *callInst,
                                        const std::vector<ref<Expr>> &args) {
  auto cb = cast<const llvm::CallBase>(callInst);
  auto f = cb->getCalledFunction();
  if (!f) {
    // indirect call
    return;
  }
  if (auto md = f->getMetadata(BONC_METADATA_KIND)) {
    // TODO check md == ::round
    md->getOperand(0)->print(llvm::errs());
    pImpl->beforeEnterRoundFn(executor, state, f);
  }
}

void BoncController::beforeEnterRoundLoop(Executor *executor,
                                          ExecutionState &state,
                                          const llvm::BasicBlock *bb) {
  pImpl->beforeEnterRoundLoop(executor, state, bb);
}

void BoncController::recordWrite(const Executor *executor,
                                 const ExecutionState &state,
                                 const MemoryObject *mo,
                                 const ref<Expr> &value) {
  pImpl->recordWrite(executor, state, mo, value);
}

void BoncController::afterExitRoundFn(Executor *executor, ExecutionState &state,
                                      const llvm::Instruction *callInst,
                                      const llvm::Instruction *retInst,
                                      const ref<Expr> &retValue) {
  if (!callInst) {
    // initial stack frame
    return;
  }
  auto cb = cast<const llvm::CallBase>(callInst);
  auto f = cb->getCalledFunction();
  pImpl->afterExitRoundFn(executor, state, f);
}

void BoncController::afterExitRoundLoop(Executor *executor,
                                        ExecutionState &state,
                                        const llvm::BasicBlock *bb) {
  pImpl->afterExitRoundLoop(executor, state, bb);
}

} // namespace klee::bonc