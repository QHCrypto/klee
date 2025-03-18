#include "BoncRound.h"
#include "llvm/IR/InstrTypes.h"

#include <optional>

namespace klee::bonc {

namespace {

const llvm::Function* currentRoundFn;

std::set<uint64_t> writtenObjectsInRound;
}

#define LOG klee_message

void beforeEnterRound(const Executor *executor, const ExecutionState &state,
  const KInstruction *ki, const llvm::Function *f,
  const std::vector<ref<Expr>> &args) {
  if (auto md = f->getMetadata(BONC_METADATA_KIND)) {
    if (currentRoundFn) {
      LOG("Already in Round function %s; Recursive calling [[bonc::round]] is not supported.", currentRoundFn->getName().str().c_str());
      return;
    }
    currentRoundFn = f;
    LOG("Entering Round function %s", f->getName().str().c_str());
    md->getOperand(0)->print(llvm::errs());
  }
}

void recordWrite(const Executor* executor, const ExecutionState& state, const MemoryObject* mo, const ref<Expr>& value) {
  if (!currentRoundFn) {
    return;
  }
  // LOG("Write at address %" PRIx64 " with expression", mo->address);
  // value->print(llvm::errs());
  writtenObjectsInRound.insert(mo->address);
}


void afterExitRound(const Executor *executor, const ExecutionState &state,
  const llvm::Instruction *inst) {
  if (!inst) {
    // initial stack frame
    return;
  }
  auto cb = cast<const llvm::CallBase>(inst);
  auto f = cb->getCalledFunction();
  if (currentRoundFn != f) {
    return;
  }

  for (const auto addr : writtenObjectsInRound) {
    LOG("Object at address %" PRIx64 " was written in round", addr);
    ObjectPair op;
    auto ret = state.addressSpace.resolveOne(ConstantExpr::create(addr, Expr::Int64), op);
    if (ret) {
      LOG("Size: %u", op.second->size);
      op.second->read8(0)->print(llvm::errs());
    } else {
      LOG("FAILED TO READ");
    }
  }

  LOG("Exiting Round function %s", f->getName().str().c_str());
  currentRoundFn = nullptr;
}

} // namespace klee::bonc