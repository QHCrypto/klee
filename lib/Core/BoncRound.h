#pragma once

#include "Executor.h"

namespace klee::bonc {

constexpr const unsigned BONC_METADATA_KIND = 31;

void beforeEnterRound(const Executor* executor, const ExecutionState& state, const KInstruction* callInstruction, const llvm::Function* f, const std::vector<ref<Expr>>& args);

void recordWrite(const Executor* executor, const ExecutionState& state, const MemoryObject* mo, const ref<Expr>& value);

void afterExitRound(const Executor* executor, const ExecutionState& state, const llvm::Instruction* callInstruction);

}