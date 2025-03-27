#pragma once

#include "klee/ADT/Ref.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/JSON.h"

namespace klee {

class Array;

namespace bonc {

class BitExpr {
public:
  enum Kind {
    Constant = 0,
    Read,
    Lookup,
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
  virtual llvm::json::Value toJSON() const = 0;

  friend llvm::json::Value toJSON(const BitExpr& e) {
    return e.toJSON();
  }

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
  llvm::json::Value toJSON() const override;

  bool getValue() const { return value; }

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const ConstantBitExpr *) { return true; }
};

class ReadTarget {
public:
  enum Kind {
    Invalid = -1,
    State = 0,
    Input,
  };

private:
  const Kind kind;
  std::string name;
  ReadTarget(Kind kind, std::string name)
      : kind{kind}, name{std::move(name)} {}

public:
  static ReadTarget create(Kind kind, std::string name) {
    return ReadTarget(kind, name);
  }

  Kind getKind() const { return kind; }
  const std::string &getName() const { return name; }
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
  llvm::json::Value toJSON() const override;

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const ReadBitExpr *) { return true; }
};

class LookupBitExpr : public BitExpr {
public:
  static const Kind kind = Lookup;

private:
  const klee::Array *table;
  std::vector<ref<BitExpr>> inputs;
  unsigned output_offset;

  LookupBitExpr(const klee::Array *table, std::vector<ref<BitExpr>> inputs,
                unsigned output_offset)
      : table{table}, inputs{inputs}, output_offset{output_offset} {}

public:
  static ref<LookupBitExpr> create(const klee::Array *table,
                                   std::vector<ref<BitExpr>> inputs,
                                   unsigned output_offset) {
    return ref<LookupBitExpr>(new LookupBitExpr(table, inputs, output_offset));
  }

  const klee::Array *getTable() const { return table; }
  const std::vector<ref<BitExpr>> &getInputs() const { return inputs; }
  unsigned getOutputOffset() const { return output_offset; }

  Kind getKind() const override { return kind; }
  void print(llvm::raw_ostream &os) const override;
  llvm::json::Value toJSON() const override;

  static bool classof(const BitExpr *e) { return e->getKind() == kind; }
  static bool classof(const LookupBitExpr *) { return true; }
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
  llvm::json::Value toJSON() const override;

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
  llvm::json::Value toJSON() const override;

  ref<BitExpr> getLeft() const { return left; }
  ref<BitExpr> getRight() const { return right; }

  static bool classof(const BitExpr *e) {
    return And <= e->getKind() && e->getKind() <= Xor;
  }
  static bool classof(const BinaryBitExpr *) { return true; }
};

} // namespace bonc

} // namespace klee