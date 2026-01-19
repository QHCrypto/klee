#pragma once

#include "ExecutionState.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/LLVMContext.h"
#include <memory>

namespace klee::bonc {

constexpr const unsigned BONC_ROUND_METADATA_KIND = llvm::LLVMContext::MD_bonc_round;
constexpr const unsigned BONC_METAPARAM_METADATA_KIND = llvm::LLVMContext::MD_bonc_metaparam;

class BoncControllerImpl;

class BoncController {

private:
  std::unique_ptr<BoncControllerImpl> pImpl; // 使用 pImpl 指针

public:
  BoncController();
  ~BoncController();

  BoncController(BoncController &&);
  BoncController &operator=(BoncController &&);

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
  void setOutput(const std::string& name, std::size_t size, 
                 const std::vector<ref<Expr>>& expressions_by_byte);

  void printResult(llvm::raw_ostream &os) const;
};

} // namespace klee::bonc