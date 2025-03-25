#pragma once

#include "klee/ADT/Ref.h"
#include "llvm/Support/raw_ostream.h"

namespace klee::bonc {

class BitExpr {
public:
  enum Kind {
    Constant = 0,
    Read,
    Not,
    And,
    Or,
    Xor,
  };
  BitExpr() = default;
  virtual ~BitExpr() = default;

  mutable class ReferenceCounter _refCount;

  virtual Kind getKind() const = 0;
  virtual void print(llvm::raw_ostream &os) const = 0;

  static bool classof(const BitExpr *) { return true; }
};

class ConstantBitExpr : public BitExpr {
public:
  static const Kind kind = Constant;

private:
  const bool value;
  ConstantBitExpr(bool value) : value{value} {}

public:
  static ref<ConstantBitExpr> create(bool value) {
    return ref<ConstantBitExpr>(new ConstantBitExpr(value));
  }

  Kind getKind() const override { return kind; }
  void print(llvm::raw_ostream &os) const override {
    os << (value ? "1" : "0");
  }

  bool getValue() const { return value; }

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const ConstantBitExpr *) { return true; }
};

class ReadTarget {
public:
  enum Kind {
    Invalid = -1,
    State = 0,
    Key,
    IV,
    Nonce,
    Plaintext,
    Ciphertext,
    Keystream,
  };

private:
  const Kind kind;
  const unsigned stateRoundIndex;
  ReadTarget(Kind kind, unsigned stateRoundIndex = 0)
      : kind{kind}, stateRoundIndex{stateRoundIndex} {}

public:
  static ReadTarget createState(unsigned stateRoundIndex) {
    return ReadTarget(State, stateRoundIndex);
  }
  static ReadTarget create(Kind kind) {
    assert(kind != State && "Use createState for state reads");
    return ReadTarget(kind);
  }

  Kind getKind() const { return kind; }
  unsigned getStateRoundIndex() const {
    // assert(kind == State && "Only state reads have a round index");
    return stateRoundIndex;
  }
};

class ReadBitExpr : public BitExpr {
public:
  static const Kind kind = Read;

private:
  const ReadTarget target;
  const unsigned offset;
  ReadBitExpr(ReadTarget target, unsigned offset)
      : target{target}, offset{offset} {}

public:
  static ref<ReadBitExpr> create(ReadTarget target, unsigned offset) {
    return ref<ReadBitExpr>(new ReadBitExpr(target, offset));
  }

  const ReadTarget &getTarget() const { return target; }
  unsigned getOffset() const { return offset; }

  Kind getKind() const override { return kind; }
  void print(llvm::raw_ostream &os) const override;

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const ReadBitExpr *) { return true; }
};

class NotBitExpr : public BitExpr {
public:
  static const Kind kind = Not;

private:
  ref<BitExpr> expr;
  NotBitExpr(ref<BitExpr> expr) : expr{expr} {}

public:
  static ref<NotBitExpr> create(ref<BitExpr> expr) {
    return ref<NotBitExpr>(new NotBitExpr(expr));
  }

  Kind getKind() const override { return kind; }
  void print(llvm::raw_ostream &os) const override;

  ref<BitExpr> getExpr() const { return expr; }

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const NotBitExpr *) { return true; }
};

class BinaryBitExpr : public BitExpr {
private:
  Kind kind;
  ref<BitExpr> left, right;

  BinaryBitExpr(Kind kind, ref<BitExpr> left, ref<BitExpr> right)
      : kind{kind}, left{left}, right{right} {
    assert(And <= kind && kind <= Xor && "invalid kind");
  }

public:
  static ref<BinaryBitExpr> create(Kind kind, ref<BitExpr> left,
                                   ref<BitExpr> right) {
    return ref<BinaryBitExpr>(new BinaryBitExpr(kind, left, right));
  }

  Kind getKind() const override { return kind; }
  void print(llvm::raw_ostream &os) const override;

  ref<BitExpr> getLeft() const { return left; }
  ref<BitExpr> getRight() const { return right; }

  static bool classof(const BitExpr *e) {
    return And <= e->getKind() && e->getKind() <= Xor;
  }
  static bool classof(const BinaryBitExpr *) { return true; }
};

} // namespace klee::bonc