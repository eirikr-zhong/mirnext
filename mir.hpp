/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_HPP
#define MIRNEXT_HPP

#include "mir-ir.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mirnext {

namespace detail {

inline constexpr int kUnaryNeg = 0;
inline constexpr int kBinaryAdd = 0;
inline constexpr int kBinarySub = 1;
inline constexpr int kBinaryMul = 2;
inline constexpr int kBinaryDiv = 3;
inline constexpr int kBinaryMod = 4;
inline constexpr int kBinaryAnd = 5;
inline constexpr int kBinaryOr = 6;
inline constexpr int kBinaryXor = 7;
inline constexpr int kBinaryLsh = 8;
inline constexpr int kBinaryRsh = 9;
inline constexpr int kCompareEq = 0;
inline constexpr int kCompareNe = 1;
inline constexpr int kCompareLt = 2;
inline constexpr int kCompareLe = 3;
inline constexpr int kCompareGt = 4;
inline constexpr int kCompareGe = 5;

template <class T>
using RemoveCVRef = std::remove_cvref_t<T>;

template <class T>
concept ExprConvertibleLike =
    std::same_as<RemoveCVRef<T>, Expr> ||
    (!std::same_as<RemoveCVRef<T>, Value> && std::convertible_to<T, Expr>);

template <class T>
concept ExprLike = ExprConvertibleLike<T> || std::same_as<RemoveCVRef<T>, Value>;

template <class T>
concept ExprLiteralLike =
    std::integral<RemoveCVRef<T>> && !std::same_as<RemoveCVRef<T>, bool> &&
    std::convertible_to<T, std::int64_t>;

template <class T>
concept ExprOperand = ExprLike<T> || ExprLiteralLike<T>;

template <class L, class R>
concept HasExprOperand =
    ExprOperand<L> && ExprOperand<R> && (ExprConvertibleLike<L> || ExprConvertibleLike<R>);

Value value_unary(int op, Value value);
Value value_binary(int op, Value lhs, Value rhs);
Value value_compare(int op, Value lhs, Value rhs);
Expr expr_literal_like(const Expr &expr, std::int64_t value);
Expr expr_unary(int op, Expr value);
Expr expr_binary(int op, Expr lhs, Expr rhs);
Value expr_compare(int op, Expr lhs, Expr rhs);

} // namespace detail

struct SwitchCase {
  std::int64_t value;
  Label *target;
};

class Block {
public:
  enum class Kind { Module, Function, Label };

  virtual ~Block();

  Block(const Block &) = delete;
  Block &operator=(const Block &) = delete;
  Block(Block &&) = delete;
  Block &operator=(Block &&) = delete;

  Kind kind() const noexcept;
  Block *parent() const noexcept;
  Module &module() noexcept;
  const Module &module() const noexcept;
  Function *function() noexcept;
  const Function *function() const noexcept;

  const std::optional<Error> &error() const noexcept;

  Value i8(std::int8_t value);
  Value u8(std::uint8_t value);
  Value i16(std::int16_t value);
  Value u16(std::uint16_t value);
  Value i32(std::int32_t value);
  Value u32(std::uint32_t value);
  Value i64(std::int64_t value);
  Value u64(std::uint64_t value);
  Value b(bool value);
  Value f32(float value);
  Value f64(double value);
  Value ld(long double value);

  Var var(Type type, std::string_view name);
  Value value(Type type, std::string_view name);
  Value addr(Value ref, std::string_view name = {});
  Value alloca(std::int64_t size, std::string_view name);
  Value alloca(Type element_type, std::string_view name);
  Value alloca(Type element_type, std::int64_t count, std::string_view name);
  Memory mem(Type type, Value base);
  Memory mem(Type type, Value base, Value index, int scale = 1,
             std::int64_t displacement = 0);
  Value load(Memory memory);
  Value load(Var var);
  void store(Memory memory, Value value);
  void store(Var var, Value value);
  void assign(Value dst, Value src);
  Value convert(Value value, Type::Kind to);
  Value convert(Value value, Type to);
  template <Type::Kind To> Value convert(Value value) {
    return convert(std::move(value), To);
  }
  Value call(const Prototype &prototype, const Import &callee, std::vector<Value> args);
  Value call(const Prototype &prototype, const Function &callee, std::vector<Value> args);
  CallResult call_multi(const Prototype &prototype, const Import &callee,
                        std::vector<Value> args);
  CallResult call_multi(const Prototype &prototype, const Function &callee,
                        std::vector<Value> args);
  void call_void(const Prototype &prototype, const Import &callee, std::vector<Value> args);
  void call_void(const Prototype &prototype, const Function &callee, std::vector<Value> args);
  VarRef operator[](Var var);
  Expr expr(Value value);
  Expr expr(std::int64_t value);
  void jmp(Label &target);
  void if_(Value cond, Label &target);
  void if_overflow(Value value, Label &target);
  void if_no_overflow(Value value, Label &target);
  void switch_(Value index, std::vector<Label *> targets);
  void switch_(Value index, std::initializer_list<Label *> targets);
  void switch_(Value index, std::vector<SwitchCase> cases, Label &default_target);
  void switch_(Value index, std::initializer_list<SwitchCase> cases, Label &default_target);
  void ret(Value value);
  void ret(Expr value);
  void ret(std::vector<Value> values);
  void ret();

protected:
  Block(Kind kind, Block *parent) noexcept;

  void set_module(Module *module) noexcept;
  void set_function(Function *function) noexcept;
  void fail(Error error);
  Instruction &append_operation(Instruction instruction);
  Label &create_child_label(std::string_view name);
  const std::vector<std::unique_ptr<Instruction>> &operations() const noexcept;
  const std::vector<std::unique_ptr<Label>> &child_labels() const noexcept;

private:
  friend class Function;
  friend class Label;
  friend class Module;
  friend class Expr;
  friend class VarRef;
  friend class Var;
  friend Value detail::value_unary(int op, Value value);
  friend Value detail::value_binary(int op, Value lhs, Value rhs);
  friend Value detail::value_compare(int op, Value lhs, Value rhs);

  struct ModuleBinding {
    ModuleBinding(Type type, std::string name, bool writable);

    Type type;
    std::string name;
    bool writable;
    std::optional<Operand> initializer;
  };

  Value integer_literal(Type type, std::int64_t value);
  Var module_var(Type type, std::string_view name, bool writable);
  void init_module_var(Value storage, Value rhs);
  Value append_unary(Opcode opcode, Value value);
  Value append_unary_in_current(Opcode opcode, Value value);
  Value append_binary(Opcode opcode, Value lhs, Value rhs);
  Value append_binary_in_current(Opcode opcode, Value lhs, Value rhs);
  Value append_integer_binary(Opcode opcode, Value lhs, Value rhs);
  Value append_integer_binary_in_current(Opcode opcode, Value lhs, Value rhs);
  Value compare(Opcode opcode, Value lhs, Value rhs);
  Value compare_in_current(Opcode opcode, Value lhs, Value rhs);
  CallResult append_call(const Prototype &prototype, Operand callee,
                         std::vector<Value> args,
                         std::optional<std::size_t> return_count);
  Result<Register> temp(Type type);
  bool validate_value(const Value &value, std::string_view description);
  bool validate_register_value(const Value &value, std::string_view description);
  bool validate_memory(const Memory &memory);
  bool validate_reference_value(const Value &value, std::string_view description);
  bool validate_target(const Label &target);
  bool validate_overflow_branch_value(const Value &value);
  bool validate_switch_index(const Value &index);
  bool validate_call_args(const Prototype &prototype, const std::vector<Value> &args);
  Block *binary_emit_block(Value lhs, Value rhs);
  std::size_t mark_overflow_pending() noexcept;
  Value poison_value(Type type) noexcept;
  Var poison_var(Type type) noexcept;
  Memory poison_memory(Type type) noexcept;
  CallResult poison_call_result(const Prototype &prototype) noexcept;

  Kind kind_;
  Block *parent_;
  Module *module_ = nullptr;
  Function *function_ = nullptr;
  std::vector<std::unique_ptr<Instruction>> operations_;
  std::vector<std::unique_ptr<Label>> child_labels_;
  std::vector<ModuleBinding> module_bindings_;
  std::size_t overflow_sequence_ = 0;
  std::size_t pending_overflow_sequence_ = 0;
};

class Label final : public Block {
public:
  std::size_t id() const noexcept;
  std::string_view name() const noexcept;
  bool is_valid() const noexcept;
  Label &label(std::string_view name = {});
  void end();

private:
  friend class Block;
  friend class Function;
  friend class Operand;

  Label(Function &function, Block &parent, std::size_t id, std::string name);

  std::size_t id_;
  std::string name_;
  bool appended_as_instruction_ = false;
};

class Function final : public Block {
public:
  struct Parameter {
    Type type;
    std::string name;
  };

  explicit Function(Module &module, std::string name);
  Function(Module &module, std::string name, std::vector<Type> return_types,
           std::vector<Parameter> parameters);

  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Function(Function &&) = delete;
  Function &operator=(Function &&) = delete;

  std::string_view name() const noexcept;
  const std::vector<Type> &return_types() const noexcept;
  const std::vector<Register> &arguments() const noexcept;
  Value arg(std::string_view name);
  const std::vector<Register> &local_registers() const noexcept;
  const Register *find_register(std::string_view name) const noexcept;
  const Register *find_register(std::size_t id) const noexcept;
  Label &label(std::string_view name = {});
  void end();

  std::size_t instruction_count() const noexcept;
  const std::vector<std::unique_ptr<Instruction>> &instructions() const noexcept;

private:
  friend class Block;
  friend class Module;

  const Register *find_argument(std::string_view name) const noexcept;
  Result<Register> create_register_internal(Type type, std::string_view name, bool argument);
  bool has_register_name(std::string_view name) const noexcept;
  void invalidate_flattened() const noexcept;
  void flatten_into(std::vector<std::unique_ptr<Instruction>> &out, const Block &block,
                    bool emit_label) const;

  std::string name_;
  std::vector<Type> return_types_;
  std::vector<Register> arguments_;
  std::vector<Register> local_registers_;
  mutable std::vector<std::unique_ptr<Instruction>> flattened_instructions_;
  mutable bool flattened_dirty_ = true;
  std::size_t next_register_id_ = 1;
  std::size_t next_label_id_ = 1;
};

class Module final : public Block {
public:
  explicit Module(std::string name);

  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Module(Module &&) = delete;
  Module &operator=(Module &&) = delete;

  std::string_view name() const noexcept;
  Function &new_function(std::string_view name);
  Function &new_function(std::string_view name, std::vector<Type> return_types,
                         std::vector<Function::Parameter> parameters);
  Prototype &new_prototype(std::string_view name, std::vector<Type> return_types,
                           std::vector<Prototype::Parameter> parameters);
  Import &new_import(std::string_view name);
  Data &bss(std::string_view name, std::size_t size);
  Data &data(std::string_view name, Type element_type, std::vector<std::int64_t> values);
  Data &data(std::string_view name, Type element_type, std::vector<std::uint64_t> values);
  Data &data(std::string_view name, Type element_type, std::vector<float> values);
  Data &data(std::string_view name, Type element_type, std::vector<double> values);
  Data &data(std::string_view name, Type element_type, std::vector<long double> values);
  Data &string_data(std::string_view name, std::string_view value);
  Data &ref_data(std::string_view name, const Import &target,
                 std::int64_t displacement = 0);
  Data &ref_data(std::string_view name, const Function &target,
                 std::int64_t displacement = 0);
  Data &ref_data(std::string_view name, const Prototype &target,
                 std::int64_t displacement = 0);
  Result<Value> ref(const Import &import);
  Result<Value> ref(const Function &function);
  Result<Value> ref(const Prototype &prototype);
  Result<Value> ref(const Data &data);
  Result<std::vector<std::byte>> encode_binary() const;
  const std::optional<Error> &error() const noexcept;
  const std::vector<std::unique_ptr<Prototype>> &prototypes() const noexcept;
  const std::vector<std::unique_ptr<Import>> &imports() const noexcept;
  const std::vector<std::unique_ptr<Data>> &data_items() const noexcept;
  const std::vector<std::unique_ptr<Function>> &functions() const noexcept;

private:
  friend class Block;

  void fail(Error error);

  std::string name_;
  std::optional<Error> error_;
  std::vector<std::unique_ptr<Prototype>> prototypes_;
  std::vector<std::unique_ptr<Import>> imports_;
  std::vector<std::unique_ptr<Data>> data_items_;
  std::vector<std::unique_ptr<Function>> functions_;
};

class Context {
public:
  Context() = default;
  ~Context() = default;

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(Context &&) = delete;

  Module &new_module(std::string_view name);
  const std::vector<std::unique_ptr<Module>> &modules() const noexcept;
  void dump(std::ostream &out) const;

private:
  std::vector<std::unique_ptr<Module>> modules_;
};

// Threading contract:
// Context, Module, Function, and Block are not internally synchronized.
// Mutating builders must be externally synchronized when shared across threads.
class Value {
public:
  Value() noexcept;

  Block &block() const noexcept;
  Type type() const noexcept;
  Operand operand() const noexcept;
  bool is_valid() const noexcept;
  bool is_poison() const noexcept;
  Value &overflow(bool enabled = true) & noexcept;
  Value &&overflow(bool enabled = true) && noexcept;
  bool checks_overflow() const noexcept;
  bool has_overflow_result() const noexcept;

  Value convert(Type::Kind to) const;
  Value convert(Type to) const;
  template <Type::Kind To> Value convert() const {
    return convert(To);
  }

private:
  friend class Block;
  friend class Function;
  friend class Module;
  friend class Var;
  friend Value detail::value_unary(int op, Value value);
  friend Value detail::value_binary(int op, Value lhs, Value rhs);
  friend Value detail::value_compare(int op, Value lhs, Value rhs);

  Value(Block &block, Type type, Operand operand) noexcept;
  static Value poison(Block &block, Type type) noexcept;

  Block *block_;
  Type type_;
  Operand operand_;
  bool overflow_enabled_ = false;
  bool overflow_pending_ = false;
  std::size_t overflow_sequence_ = 0;
};

class Var {
public:
  Var() noexcept;

  operator Value() const noexcept;

  Block &block() const noexcept;
  Type type() const noexcept;
  Value value() const noexcept;
  bool is_valid() const noexcept;

  void assign(Value rhs);
  void init(Value rhs);

private:
  friend class Block;
  friend class Module;
  friend class Expr;

  Var(Block &block, Value storage) noexcept;
  static Var poison(Block &block, Type type) noexcept;

  Block *block_;
  Value storage_;
};

class Memory {
public:
  Memory() noexcept;

  Block &block() const noexcept;
  Type type() const noexcept;
  Operand operand() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class Block;

  Memory(Block &block, Type type, Operand operand) noexcept;
  static Memory poison(Block &block, Type type) noexcept;

  Block *block_;
  Type type_;
  Operand operand_;
};

class CallResult {
public:
  CallResult() = default;

  bool empty() const noexcept { return values_.empty(); }
  std::size_t size() const noexcept { return values_.size(); }
  const Value &operator[](std::size_t index) const noexcept { return values_[index]; }
  Value &operator[](std::size_t index) noexcept { return values_[index]; }
  const std::vector<Value> &values() const noexcept { return values_; }
  std::vector<Value> &&take_values() noexcept { return std::move(values_); }

  std::vector<Value>::const_iterator begin() const noexcept { return values_.begin(); }
  std::vector<Value>::const_iterator end() const noexcept { return values_.end(); }

private:
  friend class Block;

  explicit CallResult(std::vector<Value> values) : values_(std::move(values)) {}

  std::vector<Value> values_;
};

class Expr {
public:
  Expr() noexcept;
  explicit Expr(Value value) noexcept;

  Block &block() const noexcept;
  Type type() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class Block;
  friend class VarRef;
  friend Expr detail::expr_literal_like(const Expr &expr, std::int64_t value);
  friend Expr detail::expr_unary(int op, Expr value);
  friend Expr detail::expr_binary(int op, Expr lhs, Expr rhs);
  friend Value detail::expr_compare(int op, Expr lhs, Expr rhs);

  Expr(Block &block, Type type) noexcept;
  Expr(Block &block, Value value) noexcept;
  Value value() const noexcept;
  Block *owner() const noexcept;
  static Expr invalid(Block *block, Type type, Error error);
  static Expr literal_like(const Expr &expr, std::int64_t value);
  static Expr unary(int op, Expr value);
  static Expr binary(int op, Expr lhs, Expr rhs);
  static Value invalid_compare(Block *block, Error error);
  static Value compare(int op, Expr lhs, Expr rhs);

  Block *block_;
  Type type_;
  Value value_;
};

class VarRef {
public:
  VarRef() noexcept;

  operator Expr() const;
  void operator=(Expr rhs) const;
  void operator=(Value rhs) const;
  void operator=(std::int64_t rhs) const;

private:
  friend class Block;

  VarRef(Block &block, Var var) noexcept;

  Block *block_;
  Var var_;
};

Value operator-(Value value);
Value operator+(Value lhs, Value rhs);
Value operator-(Value lhs, Value rhs);
Value operator*(Value lhs, Value rhs);
Value operator/(Value lhs, Value rhs);
Value operator%(Value lhs, Value rhs);
Value operator&(Value lhs, Value rhs);
Value operator|(Value lhs, Value rhs);
Value operator^(Value lhs, Value rhs);
Value operator<<(Value lhs, Value rhs);
Value operator>>(Value lhs, Value rhs);
Value operator==(Value lhs, Value rhs);
Value operator!=(Value lhs, Value rhs);
Value operator<(Value lhs, Value rhs);
Value operator<=(Value lhs, Value rhs);
Value operator>(Value lhs, Value rhs);
Value operator>=(Value lhs, Value rhs);

namespace detail {

template <class T>
Expr as_expr(T &&value) {
  if constexpr (std::same_as<RemoveCVRef<T>, Expr>) {
    return std::forward<T>(value);
  } else if constexpr (std::same_as<RemoveCVRef<T>, Value>) {
    return Expr(std::forward<T>(value));
  } else {
    return static_cast<Expr>(std::forward<T>(value));
  }
}

template <class L, class R>
Expr expr_binary_from_operands(int op, L &&lhs, R &&rhs) {
  if constexpr (ExprLiteralLike<L>) {
    Expr rhs_expr = as_expr(std::forward<R>(rhs));
    Expr lhs_expr = expr_literal_like(rhs_expr, static_cast<std::int64_t>(lhs));
    return expr_binary(op, std::move(lhs_expr), std::move(rhs_expr));
  } else if constexpr (ExprLiteralLike<R>) {
    Expr lhs_expr = as_expr(std::forward<L>(lhs));
    Expr rhs_expr = expr_literal_like(lhs_expr, static_cast<std::int64_t>(rhs));
    return expr_binary(op, std::move(lhs_expr), std::move(rhs_expr));
  } else {
    return expr_binary(op, as_expr(std::forward<L>(lhs)), as_expr(std::forward<R>(rhs)));
  }
}

template <class L, class R>
Value expr_compare_from_operands(int op, L &&lhs, R &&rhs) {
  if constexpr (ExprLiteralLike<L>) {
    Expr rhs_expr = as_expr(std::forward<R>(rhs));
    Expr lhs_expr = expr_literal_like(rhs_expr, static_cast<std::int64_t>(lhs));
    return expr_compare(op, std::move(lhs_expr), std::move(rhs_expr));
  } else if constexpr (ExprLiteralLike<R>) {
    Expr lhs_expr = as_expr(std::forward<L>(lhs));
    Expr rhs_expr = expr_literal_like(lhs_expr, static_cast<std::int64_t>(rhs));
    return expr_compare(op, std::move(lhs_expr), std::move(rhs_expr));
  } else {
    return expr_compare(op, as_expr(std::forward<L>(lhs)), as_expr(std::forward<R>(rhs)));
  }
}

} // namespace detail

template <detail::ExprConvertibleLike T>
Expr operator-(T &&value) {
  return detail::expr_unary(detail::kUnaryNeg, detail::as_expr(std::forward<T>(value)));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator+(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryAdd, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator-(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinarySub, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator*(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryMul, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator/(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryDiv, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator%(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryMod, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator&(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryAnd, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator|(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryOr, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator^(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryXor, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator<<(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryLsh, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Expr operator>>(L &&lhs, R &&rhs) {
  return detail::expr_binary_from_operands(detail::kBinaryRsh, std::forward<L>(lhs),
                                           std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator==(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareEq, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator!=(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareNe, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator<(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareLt, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator<=(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareLe, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator>(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareGt, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

template <detail::ExprOperand L, detail::ExprOperand R>
  requires detail::HasExprOperand<L, R>
Value operator>=(L &&lhs, R &&rhs) {
  return detail::expr_compare_from_operands(detail::kCompareGe, std::forward<L>(lhs),
                                            std::forward<R>(rhs));
}

const char *opcode_name(Opcode opcode) noexcept;
const char *type_name(Type type) noexcept;

Result<std::vector<std::byte>> encode_binary(const Module &module);

} // namespace mirnext

#endif
