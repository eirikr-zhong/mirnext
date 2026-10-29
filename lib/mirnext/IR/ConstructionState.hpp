/* Licensed under the MIT License. Builder value storage; not a construction API. */
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::ir::detail {
class ExpressionState {
 public:
  Type type () const { return type_; }
  bool is_valid () const;
  const Function &function () const;
  Result<Operand> value () const;
  ExpressionState convert (Type) const;
  ExpressionState operator- () const;
  ExpressionState operator~() const;
  ExpressionState operator!() const;
#define MIRNEXT_EXPRESSION_OPERATOR(symbol, operation)                         \
  friend ExpressionState operator symbol (const ExpressionState &lhs,          \
                                          const ExpressionState &rhs) {        \
    return lhs.apply (Operation::operation, rhs);                              \
  }                                                                            \
  template <class T>                                                           \
    requires (std::is_arithmetic_v<T> && std::is_constructible_v<Operand, T>)  \
  friend ExpressionState operator symbol (const ExpressionState &lhs, T rhs) { \
    return lhs.apply (Operation::operation, lhs.literal_like (Operand (rhs))); \
  }                                                                            \
  template <class T>                                                           \
    requires (std::is_arithmetic_v<T> && std::is_constructible_v<Operand, T>)  \
  friend ExpressionState operator symbol (T lhs, const ExpressionState &rhs) { \
    return rhs.literal_like (Operand (lhs)).apply (Operation::operation, rhs); \
  }
  MIRNEXT_EXPRESSION_OPERATOR (+, Add)
  MIRNEXT_EXPRESSION_OPERATOR (-, Sub)
  MIRNEXT_EXPRESSION_OPERATOR (*, Mul)
  MIRNEXT_EXPRESSION_OPERATOR (/, Div)
  MIRNEXT_EXPRESSION_OPERATOR (%, Mod)
  MIRNEXT_EXPRESSION_OPERATOR (&, And)
  MIRNEXT_EXPRESSION_OPERATOR (|, Or)
  MIRNEXT_EXPRESSION_OPERATOR (^, Xor)
  MIRNEXT_EXPRESSION_OPERATOR (<<, LeftShift)
  MIRNEXT_EXPRESSION_OPERATOR (>>, RightShift)
  MIRNEXT_EXPRESSION_OPERATOR (==, Equal)
  MIRNEXT_EXPRESSION_OPERATOR (!=, NotEqual)
  MIRNEXT_EXPRESSION_OPERATOR (<, Less)
  MIRNEXT_EXPRESSION_OPERATOR (<=, LessEqual)
  MIRNEXT_EXPRESSION_OPERATOR (>, Greater)
  MIRNEXT_EXPRESSION_OPERATOR (>=, GreaterEqual)
#undef MIRNEXT_EXPRESSION_OPERATOR
 private:
  friend class ::mirnext::ir::InstructionBuilder;
  friend struct NativeAccess;
  enum class Operation {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Xor,
    LeftShift,
    RightShift,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual
  };
  ExpressionState (InstructionBuilder &, Type, std::optional<Operand>);
  ExpressionState apply (Operation, const ExpressionState &) const;
  ExpressionState literal_like (Operand) const;
  InstructionBuilder *builder_;
  Type type_;
  using Scalar = std::variant<std::int64_t, std::uint64_t, float, double, long double,
                              VectorConstant, const Register *>;
  std::optional<Scalar> value_;
};

class BlockArgumentState {
 public:
  BlockArgumentState (const Register &address, std::size_t size,
                      Type::BlockClass classification = Type::BlockClass::Default,
                      std::int64_t displacement = 0)
    : memory_{Type::block (size, classification), address, {}, displacement} {}
  BlockArgumentState (const Register &address, Type type, std::int64_t displacement = 0)
    : memory_{type, address, {}, displacement} {}
  static BlockArgumentState return_buffer (const Register &address, std::size_t size) {
    return BlockArgumentState (address, Type::return_block (size));
  }
  const Memory &memory () const { return memory_; }

 private:
  Memory memory_;
};

class CallArgumentState {
 public:
  CallArgumentState (BlockArgumentState value) : value_ (std::move (value)) {}
  CallArgumentState (ExpressionState value) : value_ (std::move (value)) {}
  template <class T>
    requires std::is_constructible_v<Operand, T>
  CallArgumentState (T &&value) : value_ (Operand (std::forward<T> (value))) {}
  const auto &storage () const { return value_; }

 private:
  std::variant<Operand, BlockArgumentState, ExpressionState> value_;
};

class CallResultState {
 public:
  const Instruction &instruction () const { return *instruction_; }
  ReferenceRange<const Register> results () const {
    return ReferenceRange<const Register> (results_);
  }
  Result<const Register &> value () const {
    if (results_.size () != 1)
      return Error{ErrorCode::InvalidOperand, "call does not have exactly one result"};
    return *(results_.front ());
  }

 private:
  friend class ::mirnext::ir::InstructionBuilder;
  CallResultState (const Instruction &instruction, std::vector<const Register *> results)
    : instruction_ (&instruction), results_ (std::move (results)) {}
  const Instruction *instruction_;
  std::vector<const Register *> results_;
};

}  // namespace mirnext::ir::detail
