#include "BoncBitExpr.h"

#include "klee/Expr/Expr.h"

namespace klee::bonc {

llvm::json::Value ConstantBitExpr::toJSON() const {
  return llvm::json::Object{{"type", "constant"}, {"value", +value}};
}

void ReadBitExpr::print(llvm::raw_ostream &os) const {
  os << target.getName() << "[" << offset << "]";
}

llvm::json::Value ReadBitExpr::toJSON() const {
  std::string target_kind =
      target.getKind() == ReadTarget::State ? "state" : "input";
  return llvm::json::Object{{"type", "read"},
                            {"target_kind", target_kind},
                            {"target_name", target.getName()},
                            {"offset", offset}};
}

void LookupBitExpr::print(llvm::raw_ostream &os) const {
  os << table->getName() << "(";
  for (auto i = 0u; i < inputs.size(); i++) {
    if (i > 0) {
      os << ", ";
    }
    inputs[i]->print(os);
  }
  os << ")[" << output_offset << "]";
}

llvm::json::Value LookupBitExpr::toJSON() const {
  llvm::json::Array input_json;
  for (auto &input : inputs) {
    input_json.push_back(input->toJSON());
  }
  return llvm::json::Object{{"type", "lookup"},
                            {"table_name", table->getName()},
                            {"inputs", std::move(input_json)},
                            {"output_offset", output_offset}};
}

void NotBitExpr::print(llvm::raw_ostream &os) const {
  os << "!";
  if (isa<BinaryBitExpr>(expr)) {
    os << "(";
    expr->print(os);
    os << ")";
  } else {
    expr->print(os);
  }
}

llvm::json::Value NotBitExpr::toJSON() const {
  return llvm::json::Object{
      {"type", "unary"}, {"operator", "not"}, {"operand", expr->toJSON()}};
}

void BinaryBitExpr::print(llvm::raw_ostream &os) const {
  if (isa<BinaryBitExpr>(left) && left->getKind() != kind) {
    os << "(";
    left->print(os);
    os << ")";
  } else {
    left->print(os);
  }
  switch (kind) {
  case And:
    os << " & ";
    break;
  case Or:
    os << " | ";
    break;
  case Xor:
    os << " ^ ";
    break;
  default:
    assert(0 && "invalid kind");
  }
  if (isa<BinaryBitExpr>(right) && right->getKind() != kind) {
    os << "(";
    right->print(os);
    os << ")";
  } else {
    right->print(os);
  }
}

llvm::json::Value BinaryBitExpr::toJSON() const {
  std::string op;
  switch (kind) {
  case And:
    op = "and";
    break;
  case Or:
    op = "or";
    break;
  case Xor:
    op = "xor";
    break;
  default:
    assert(0 && "invalid kind");
  }
  return llvm::json::Object{{"type", "binary"},
                            {"operator", op},
                            {"left", left->toJSON()},
                            {"right", right->toJSON()}};
}

} // namespace klee::bonc