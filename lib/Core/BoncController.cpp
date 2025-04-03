#include "BoncController.h"

#include "BoncBitExpr.h"
#include "ExecutionState.h"
#include "Executor.h"

#include "klee/Expr/Expr.h"
#include "llvm-13/llvm/IR/InstrTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/JSON.h"
#include <algorithm>
#include <climits>
#include <string>

#define LOG klee_message

#define BIT_WIDTH(x)                                                           \
  (CHAR_BIT * sizeof(unsigned long long) -                                     \
   __builtin_clzll(static_cast<unsigned long long>(x)))

namespace klee::bonc {

struct SBoxTableInfo {
  std::string name;
  std::size_t input_width;
  std::size_t output_width;
  std::vector<uint64_t> values;

  friend llvm::json::Value toJSON(const SBoxTableInfo &info) {
    return llvm::json::Object{{"name", info.name},
                              {"input_width", info.input_width},
                              {"output_width", info.output_width},
                              {"values", std::move(info.values)}};
  }
};

struct IterationInfo {
  std::string name;
  std::size_t size;
  std::vector<ref<BitExpr>> update_expressions;

  friend llvm::json::Value toJSON(const IterationInfo &info) {
    llvm::json::Array update_json;
    for (auto &update : info.update_expressions) {
      update_json.push_back(update->toJSON());
    }
    return llvm::json::Object{{"name", info.name},
                              {"size", info.size},
                              {"update_expressions", std::move(update_json)}};
  }
};

struct IoInfo {
  std::string name;
  std::size_t size;
  std::vector<ref<BitExpr>> expressions;

  friend llvm::json::Value toJSON(const IoInfo &info) {
    llvm::json::Object result{{"name", info.name}, {"size", info.size}};
    if (info.expressions.size() > 0) {
      llvm::json::Array expr_json;
      for (auto &expr : info.expressions) {
        expr_json.push_back(expr->toJSON());
      }
      result["expressions"] = std::move(expr_json);
    }
    return result;
  }
};

class BoncControllerImpl {
public:
  const ExecutionState *current_state{};
  const llvm::Function *current_round_fn{};
  const llvm::BasicBlock *current_round_bb{};
  std::set<uint64_t> written_objects_in_round{};
  unsigned round_index{0};

  std::set<const Array *> sbox_tables;
  std::vector<IterationInfo> iterations;
  std::map<std::string, IoInfo> inputs;
  std::map<std::string, IoInfo> outputs;

  BoncControllerImpl() = default;

  std::vector<ref<BitExpr>>
  getBitExpr(ref<Expr> expr, const std::vector<unsigned> &bit_offsets) {
    if (bit_offsets.size() == 0) {
      return {};
    }
    switch (expr->getKind()) {
    case Expr::InvalidKind:
    case Expr::NotOptimized:
    case Expr::Select:
      LOG("Unsupported expression kind %d", expr->getKind());
      abort();
      break;
    case Expr::Add:
    case Expr::Sub:
    case Expr::Mul:
    case Expr::UDiv:
    case Expr::SDiv:
    case Expr::URem:
    case Expr::SRem:
      LOG("Unsupported non-F2 arithmetic expression kind %d", expr->getKind());
      abort();
      break;
    case Expr::Ult:
    case Expr::Ule:
    case Expr::Slt:
    case Expr::Sle:
      LOG("Unsupported non-F2 comparision expression kind %d", expr->getKind());
      abort();
      break;
    case Expr::Ne:
    case Expr::Ugt:
    case Expr::Uge:
    case Expr::Sgt:
    case Expr::Sge:
      LOG("Unsupported uncanonicalized comparison %d", expr->getKind());
      abort();
      break;
    case Expr::Constant: {
      auto value = cast<ConstantExpr>(expr)->getAPValue();
      std::vector<ref<BitExpr>> result;
      for (auto o : bit_offsets) {
        result.push_back(
            ConstantBitExpr::create(value.extractBitsAsZExtValue(1, o)));
      }
      return result;
    }
    case Expr::Read: {
      auto read_expr = cast<ReadExpr>(expr);
      auto index_expr = read_expr->index;
      auto read_root = read_expr->updates.root;
      if (read_root->isSymbolicArray()) {
        auto index_constant = dyn_cast<ConstantExpr>(index_expr);
        if (!index_constant) {
          LOG("Unsupported: read expression with symbolic array root (%s) at a "
              "non-constant index",
              read_root->name.c_str());
          abort();
        }
        auto target_kind = ReadTarget::Invalid;
        std::string target_name;
        if (read_root->name.find("bonc:state") == 0) {
          target_kind = ReadTarget::State;
          target_name = read_root->name.substr(5);
        } else if (read_root->name.find("bonc:input") == 0) {
          target_kind = ReadTarget::Input;
          target_name = read_root->name.substr(5);
        } else {
          LOG("Read expression with unknown array root (%s)",
              read_root->name.c_str());
          abort();
        }

        auto target = ReadTarget::create(target_kind, target_name);

        std::vector<ref<BitExpr>> result;
        for (auto o : bit_offsets) {
          result.push_back(ReadBitExpr::create(
              target, index_constant->getZExtValue() * CHAR_BIT + o));
        }
        return result;
      }
      // S-box lookup
      if (!read_root->isConstantArray()) {
        LOG("Read expression with non-constant array root (%s)",
            read_root->name.c_str());
        abort();
      }
      sbox_tables.insert(read_root);
      auto input_width = BIT_WIDTH(read_root->constantValues.size() - 1);
      std::vector<unsigned> input_bit_offsets(input_width);
      std::iota(input_bit_offsets.begin(), input_bit_offsets.end(), 0u);
      auto input_bits = getBitExpr(index_expr, input_bit_offsets);
      std::vector<ref<BitExpr>> result;
      for (auto o : bit_offsets) {
        result.push_back(LookupBitExpr::create(read_root, input_bits, o));
      }
      return result;
    }
    case Expr::Concat: {
      auto concat_expr = cast<ConcatExpr>(expr);
      auto left = concat_expr->getLeft();
      auto right = concat_expr->getRight();
      std::vector<unsigned> left_bit_offsets;
      std::vector<unsigned> right_bit_offsets;
      std::vector<unsigned> is_right;
      auto right_width = right->getWidth();
      for (auto o : bit_offsets) {
        if (o < right_width) {
          right_bit_offsets.push_back(o);
          is_right.push_back(1);
        } else {
          left_bit_offsets.push_back(o - right_width);
          is_right.push_back(0);
        }
      }
      auto left_result = getBitExpr(left, left_bit_offsets);
      auto right_result = getBitExpr(right, right_bit_offsets);
      std::vector<ref<BitExpr>> result;
      for (auto i = 0u, left_i = 0u, right_i = 0u; i < bit_offsets.size();
           i++) {
        if (is_right[i]) {
          result.push_back(std::move(right_result.at(right_i++)));
        } else {
          result.push_back(std::move(left_result.at(left_i++)));
        }
      }
      return result;
    }
    case Expr::Extract: {
      auto extract_expr = cast<ExtractExpr>(expr);
      auto offset = extract_expr->offset;
      std::vector<unsigned> src_bit_offsets;
      std::transform(bit_offsets.begin(), bit_offsets.end(),
                     std::back_inserter(src_bit_offsets),
                     [offset](unsigned o) { return o + offset; });
      return getBitExpr(extract_expr->expr, src_bit_offsets);
    }
    case Expr::ZExt:
    case Expr::SExt: {
      auto extend_expr = cast<CastExpr>(expr);
      auto is_signed_ext = extend_expr->getKind() == Expr::SExt;
      auto src = extend_expr->src;
      auto src_width = src->getWidth();
      std::vector<unsigned> src_bit_offsets;
      bool has_msb = false;
      for (auto o : bit_offsets) {
        if (o >= src_width - 1) {
          if (o == src_width - 1 || is_signed_ext) {
            has_msb = true;
          }
        } else {
          src_bit_offsets.push_back(o);
        }
      }
      if (has_msb) {
        src_bit_offsets.push_back(src_width - 1);
      }
      auto src_result = getBitExpr(src, src_bit_offsets);
      std::vector<ref<BitExpr>> result;
      auto src_result_index = 0u;
      for (auto o : bit_offsets) {
        if (o >= src_width - 1) {
          if (o == src_width - 1 || is_signed_ext) {
            result.push_back(src_result.back());
          } else {
            result.push_back(ConstantBitExpr::create(false));
          }
        } else {
          result.push_back(std::move(src_result.at(src_result_index++)));
        }
      }
      return result;
    }
    case Expr::Not: {
      auto src_result = getBitExpr(cast<NotExpr>(expr)->expr, bit_offsets);
      std::vector<ref<BitExpr>> result;
      for (auto src_bit : src_result) {
        if (auto constant_expr = dyn_cast<ConstantBitExpr>(src_bit)) {
          result.push_back(ConstantBitExpr::create(!constant_expr->getValue()));
        } else {
          result.push_back(NotBitExpr::create(src_bit));
        }
      }
      return result;
    }
    case Expr::And:
    case Expr::Or:
    case Expr::Xor: {
      auto bin_expr = cast<BinaryExpr>(expr);
      auto kind = bin_expr->getKind();
      auto left_result = getBitExpr(bin_expr->left, bit_offsets);
      auto right_result = getBitExpr(bin_expr->right, bit_offsets);
      std::vector<ref<BitExpr>> result;
      for (auto i = 0u; i < bit_offsets.size(); i++) {
        auto left_bit = left_result.at(i);
        auto right_bit = right_result.at(i);
        switch (kind) {
        default: {
          assert(0);
          __builtin_unreachable();
          break;
        }
        case Expr::And: {
          if (auto constant_left = dyn_cast<ConstantBitExpr>(left_bit)) {
            if (constant_left->getValue()) {
              result.push_back(right_bit);
            } else {
              result.push_back(ConstantBitExpr::create(false));
            }
          } else if (auto constant_right =
                         dyn_cast<ConstantBitExpr>(right_bit)) {
            if (constant_right->getValue()) {
              result.push_back(left_bit);
            } else {
              result.push_back(ConstantBitExpr::create(false));
            }
          } else {
            result.push_back(
                BinaryBitExpr::create(BitExpr::And, left_bit, right_bit));
          }
          break;
        }
        case Expr::Or: {
          if (auto constant_left = dyn_cast<ConstantBitExpr>(left_bit)) {
            if (constant_left->getValue()) {
              result.push_back(ConstantBitExpr::create(true));
            } else {
              result.push_back(right_bit);
            }
          } else if (auto constant_right =
                         dyn_cast<ConstantBitExpr>(right_bit)) {
            if (constant_right->getValue()) {
              result.push_back(ConstantBitExpr::create(true));
            } else {
              result.push_back(left_bit);
            }
          } else {
            result.push_back(
                BinaryBitExpr::create(BitExpr::Or, left_bit, right_bit));
          }
          break;
        }
        case Expr::Xor: {
          auto constant_left = dyn_cast<ConstantBitExpr>(left_bit);
          auto constant_right = dyn_cast<ConstantBitExpr>(right_bit);
          if (constant_left && constant_right) {
            result.push_back(ConstantBitExpr::create(
                constant_left->getValue() ^ constant_right->getValue()));
          } else if (constant_left) {
            if (constant_left->getValue()) {
              result.push_back(NotBitExpr::create(right_bit));
            } else {
              result.push_back(right_bit);
            }
          } else if (constant_right) {
            if (constant_right->getValue()) {
              result.push_back(NotBitExpr::create(left_bit));
            } else {
              result.push_back(left_bit);
            }
          } else {
            result.push_back(
                BinaryBitExpr::create(BitExpr::Xor, left_bit, right_bit));
          }
          break;
        }
        }
      }
      return result;
    }
    case Expr::Shl: {
      auto shl_expr = cast<ShlExpr>(expr);
      auto src_expr = shl_expr->left;
      auto shift_expr = dyn_cast<ConstantExpr>(shl_expr->right);
      if (!shift_expr) {
        LOG("Unsupported: non-constant shift amount in shift expression");
        abort();
      }
      auto shift_value = shift_expr->getZExtValue();
      std::vector<unsigned> src_bit_offsets;
      for (auto o : bit_offsets) {
        if (o >= shift_value) {
          src_bit_offsets.push_back(o - shift_value);
        }
      }
      auto src_result = getBitExpr(src_expr, src_bit_offsets);
      std::vector<ref<BitExpr>> result;
      auto src_result_index = 0u;
      for (auto i = 0u; i < bit_offsets.size(); i++) {
        if (bit_offsets.at(i) >= shift_value) {
          result.push_back(src_result.at(src_result_index++));
        } else {
          result.push_back(ConstantBitExpr::create(false));
        }
      }
      return result;
    }
    case Expr::LShr:
    case Expr::AShr: {
      auto shr_expr = cast<BinaryExpr>(expr);
      bool is_arithmetic_shift = shr_expr->getKind() == Expr::AShr;
      auto src_expr = shr_expr->left;
      auto shift_expr = dyn_cast<ConstantExpr>(shr_expr->right);
      if (!shift_expr) {
        LOG("Unsupported: non-constant shift amount in shift expression");
        abort();
      }
      auto shift_value = shift_expr->getZExtValue();
      auto src_width = src_expr->getWidth();
      std::vector<unsigned> src_bit_offsets;
      bool has_msb = false;
      for (auto o : bit_offsets) {
        if (o + shift_value >= src_width - 1) {
          if (o + shift_value == src_width - 1 || is_arithmetic_shift) {
            has_msb = true;
          }
        } else {
          src_bit_offsets.push_back(o + shift_value);
        }
      }
      if (has_msb) {
        src_bit_offsets.push_back(src_width - 1);
      }
      auto src_result = getBitExpr(src_expr, src_bit_offsets);
      std::vector<ref<BitExpr>> result;
      auto src_result_index = 0u;
      for (auto o : bit_offsets) {
        if (o + shift_value >= src_width - 1) {
          if (o + shift_value == src_width - 1 || is_arithmetic_shift) {
            result.push_back(src_result.back());
          } else {
            result.push_back(ConstantBitExpr::create(false));
          }
        } else {
          result.push_back(src_result.at(src_result_index++));
        }
      }
      return result;
    }
    case Expr::Eq: {
      auto eq_expr = cast<EqExpr>(expr);
      auto not_xor_expr =
          NotExpr::create(XorExpr::create(eq_expr->left, eq_expr->right));
      auto not_result = getBitExpr(not_xor_expr, bit_offsets);
      ref<BitExpr> result = ConstantBitExpr::create(true);
      for (auto i = 0u; i < not_result.size(); i++) {
        if (auto constant_result =
                dyn_cast<ConstantBitExpr>(not_result.at(i))) {
          if (!constant_result->getValue()) {
            return {ConstantBitExpr::create(false)};
          } else {
            result = not_result.at(i);
          }
        } else {
          result =
              BinaryBitExpr::create(BitExpr::And, result, not_result.at(i));
        }
      }
      result->print(llvm::errs());
      return std::vector<ref<BitExpr>>(bit_offsets.size(), result);
    }
    }
    assert(0 && "unreachable");
    __builtin_unreachable();
  }

  void beforeEnterRoundFn(const Executor *executor, const ExecutionState &state,
                          const llvm::Function *f) {
    if (current_round_fn) {
      LOG("Already in Round function %s; Calling another [[bonc::round]] (or "
          "recursively) is not supported.",
          current_round_fn->getName().str().c_str());
      return;
    }
    if (current_round_bb) {
      LOG("Already in Round loop; Calling another [[bonc::round]] is not "
          "supported.");
      return;
    }
    LOG("Entering Round function %s", f->getName().str().c_str());
    current_round_fn = f;
    current_state = &state;
  }

  void beforeEnterRoundLoop(Executor *executor, ExecutionState &state,
                            const llvm::BasicBlock *bb) {
    if (current_round_fn) {
      LOG("Already in Round function %s; Entering a [[bonc::round]] loop is "
          "not supported.",
          current_round_fn->getName().str().c_str());
      return;
    }
    if (current_round_bb) {
      if (current_round_bb == bb) {
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
    current_round_bb = bb;
    current_state = &state;
  }

  void recordWrite(const Executor *executor, const ExecutionState &state,
                   const MemoryObject *mo, const ref<Expr> &value) {
    if (!(current_round_fn || current_round_bb)) {
      return;
    }
    assert(current_state == &state);
    // LOG("Write at address %" PRIx64 " with expression", mo->address);
    // value->print(llvm::errs());
    written_objects_in_round.insert(mo->address);
  }

  void afterExitRoundFn(Executor *executor, ExecutionState &state,
                        const llvm::Function *fn) {
    if (current_round_fn != fn) {
      return;
    }
    assert(current_state == &state);
    LOG("Exiting Round function %s", fn->getName().str().c_str());

    trackRoundUpdate(executor, state);

    current_round_fn = nullptr;
  }

  void afterExitRoundLoop(Executor *executor, ExecutionState &state,
                          const llvm::BasicBlock *bb) {
    if (current_round_bb != bb) {
      return;
    }
    assert(current_state == &state);
    LOG("Exiting Round loop");

    trackRoundUpdate(executor, state);

    current_round_bb = nullptr;
  }

  void trackRoundUpdate(Executor *executor, ExecutionState &state) {
    std::size_t object_index = 0u;
    for (const auto addr : written_objects_in_round) {
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
      auto symbol_name = "bonc:state:" + std::to_string(round_index) + "/" +
                         std::to_string(object_index);
      std::vector<ref<BitExpr>> update_expressions;

      for (auto offset = 0u; offset < op.second->size; offset++) {
        auto expr = ConstraintManager::simplifyExpr(state.constraints,
                                                    op.second->read8(offset));
        if (!isa<ConstantExpr>(expr)) {
          is_constant = false;
        }
        std::vector<unsigned> bit_offsets(CHAR_BIT);
        std::iota(bit_offsets.begin(), bit_offsets.end(), 0);
        auto bit_exprs = getBitExpr(expr, bit_offsets);
        std::move(bit_exprs.begin(), bit_exprs.end(),
                  std::back_inserter(update_expressions));
      }

      // Make it symbolic if not constant
      if (!is_constant) {
        executor->executeMakeSymbolic(state, op.first, symbol_name);
        object_index++;
        iterations.push_back(
            {symbol_name, op.second->size, std::move(update_expressions)});
      }
    }
    round_index++;
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

void BoncController::setInput(const std::string &name, std::size_t size) {
  auto it = pImpl->inputs.find(name);
  assert(it == pImpl->inputs.end() && "Input name already exists in the map");
  pImpl->inputs.insert({name, {name, size, {}}});
}

void BoncController::setOutput(
    const std::string &name, std::size_t size,
    const std::vector<ref<Expr>> &expressions_by_byte) {
  auto it = pImpl->outputs.find(name);
  assert(it == pImpl->outputs.end() && "Output name already exists in the map");
  std::vector<ref<BitExpr>> bit_exprs;
  for (auto &expr : expressions_by_byte) {
    std::vector<unsigned> bit_offsets(CHAR_BIT);
    std::iota(bit_offsets.begin(), bit_offsets.end(), 0);
    auto exprs = pImpl->getBitExpr(expr, bit_offsets);
    std::move(exprs.begin(), exprs.end(), std::back_inserter(bit_exprs));
  }
  pImpl->outputs.insert({name, {name, size, std::move(bit_exprs)}});
}

void BoncController::printResult(llvm::raw_ostream &os) const {
  llvm::json::Object result;
  result.insert({"version", 0});
  result.insert({"info", llvm::json::Object()});
  result.insert({"meta_parameters", llvm::json::Array{}});
  llvm::json::Array inputs, outputs, iterations, sboxes;
  for (const auto &[_, info] : pImpl->inputs) {
    inputs.push_back(info);
  }
  for (const auto &[_, info] : pImpl->outputs) {
    outputs.push_back(info);
  }
  for (const auto &item : pImpl->iterations) {
    iterations.push_back(toJSON(item));
  }
  for (const auto &item : pImpl->sbox_tables) {
    auto &values = item->constantValues;
    SBoxTableInfo info{};
    info.name = item->name;
    info.input_width = BIT_WIDTH(item->getSize() - 1);
    info.values.reserve(values.size());
    for (auto i = 0u; i < values.size(); i++) {
      info.values.push_back(values.at(i)->getZExtValue());
    }
    info.output_width =
        BIT_WIDTH(*std::max_element(info.values.begin(), info.values.end()));
    sboxes.push_back(toJSON(info));
  }
  result.insert({"inputs", std::move(inputs)});
  result.insert({"outputs", std::move(outputs)});
  result.insert(
      {"components", llvm::json::Object{{"sboxes", std::move(sboxes)}}});
  result.insert({"iterations", std::move(iterations)});

  os << llvm::json::Value(std::move(result));
}

} // namespace klee::bonc