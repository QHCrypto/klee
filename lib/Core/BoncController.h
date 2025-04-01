#pragma once

#include "ExecutionState.h"
#include "llvm-13/llvm/IR/Instruction.h"
#include "llvm/IR/BasicBlock.h"
#include <memory>

namespace klee::bonc {

constexpr const unsigned BONC_METADATA_KIND = 31;

class BoncControllerImpl;

class BoncController {

private:
  std::unique_ptr<BoncControllerImpl> pImpl; // 使用 pImpl 指针

public:
  BoncController();
  ~BoncController();

  void beforeEnterRoundFn(const Executor *executor, const ExecutionState &state,
                          const llvm::Instruction *callInst,
                          const std::vector<ref<Expr>> &args);

  void beforeEnterRoundLoop(Executor *executor,
                            ExecutionState &state,
                            const llvm::BasicBlock *bb);

  void recordWrite(const Executor *executor, const ExecutionState &state,
                   const MemoryObject *mo, const ref<Expr> &value);

  void afterExitRoundFn(Executor *executor, ExecutionState &state,
                        const llvm::Instruction *callInst,
                        const llvm::Instruction *retInst,
                        const ref<Expr> &retValue);

  void afterExitRoundLoop(Executor *executor, ExecutionState &state,
                          const llvm::BasicBlock *bb);

  void setInput(const std::string& name, std::size_t size);
  void setOutput(const std::string& name, std::size_t size);

  void printResult(llvm::raw_ostream &os) const;
};

} // namespace klee::bonc