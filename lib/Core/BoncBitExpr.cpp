#include "BoncBitExpr.h"

#include "klee/Expr/Expr.h"

namespace klee::bonc {

void ReadBitExpr::print(llvm::raw_ostream &os) const {
  switch (target.getKind()) {
  case ReadTarget::Invalid:
    os << "invalid";
    break;
  case ReadTarget::State:
    os << "state_" << target.getStateRoundIndex() << "_"
       << target.getStateBlockIndex() << "[" << offset << "]";
    break;
  case ReadTarget::Key:
    os << "key[" << offset << "]";
    break;
  case ReadTarget::IV:
    os << "iv[" << offset << "]";
    break;
  case ReadTarget::Nonce:
    os << "nonce[" << offset << "]";
    break;
  case ReadTarget::Plaintext:
    os << "plaintext[" << offset << "]";
    break;
  case ReadTarget::Ciphertext:
    os << "ciphertext[" << offset << "]";
    break;
  case ReadTarget::Keystream:
    os << "keystream[" << offset << "]";
    break;
  }
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

} // namespace klee::bonc