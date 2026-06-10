/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_HPP
#define MIRNEXT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mirnext {

class Function;
class Import;
class Prototype;
class IRBuilder;
class Value;
class Var;
class Memory;
class Cond;
class CallResult;

enum class ErrorCode { InvalidArgument, DuplicateName, UnknownName, NoInsertPoint, InvalidOperand };

struct Error {
  ErrorCode code;
  std::string message;
};

// Result<T> is the no-exceptions error carrier used by the C++ API.
// Check it with `if (!result)` before accessing the value. Calling value() on an error, or
// error() on a value, aborts intentionally to catch unchecked misuse during development.
template <class T> class Result {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  bool has_error() const noexcept { return std::holds_alternative<Error>(storage_); }
  explicit operator bool() const noexcept { return has_value(); }

  // Pointer accessors are useful when a caller wants to inspect without aborting.
  T *value_ptr() noexcept { return std::get_if<T>(&storage_); }
  const T *value_ptr() const noexcept { return std::get_if<T>(&storage_); }
  Error *error_ptr() noexcept { return std::get_if<Error>(&storage_); }
  const Error *error_ptr() const noexcept { return std::get_if<Error>(&storage_); }

  T &value() & noexcept {
    if (T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  const T &value() const & noexcept {
    if (const T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  T value() && noexcept {
    if (T *value = std::get_if<T>(&storage_)) return std::move(*value);
    std::abort();
  }

  Error &error() & noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  const Error &error() const & noexcept {
    if (const Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  Error error() && noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return std::move(*error);
    std::abort();
  }

  // Convenience access for already-checked results:
  //   auto r = f();
  //   if (!r) return r.error();
  //   use(*r);
  T &operator*() & noexcept { return value(); }
  const T &operator*() const & noexcept { return value(); }
  T *operator->() noexcept { return &value(); }
  const T *operator->() const noexcept { return &value(); }

private:
  std::variant<T, Error> storage_;
};

#define MIRNEXT_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define MIRNEXT_CONCAT(lhs, rhs) MIRNEXT_CONCAT_IMPL(lhs, rhs)

// Propagate an Error from a Result-returning expression.
// Use only inside functions that return Result<...>.
//
// This is a statement macro, not an expression. A syntax like
// `value = MIRNEXT_TRY(expr)` would need non-portable compiler extensions to return
// from the current function on failure. Write the destination as the first macro argument instead:
//
//   MIRNEXT_TRY(auto reg, function.create_register(Type::i64(), "tmp"));
//   MIRNEXT_TRY(existing_reg, function.argument("arg1"));
//
// The first argument is the assignment left-hand side. It can declare a new variable
// (`auto reg`, `Register reg`) or name an existing one (`reg`).
#define MIRNEXT_TRY(lhs, expr) MIRNEXT_TRY_IMPL(lhs, MIRNEXT_CONCAT(_mirnext_try_result_, __LINE__), expr)

#define MIRNEXT_TRY_IMPL(lhs, result_name, expr)          \
  auto result_name = (expr);                              \
  if (!result_name) return std::move(result_name).error(); \
  lhs = std::move(result_name).value()

class Type {
public:
  enum class Kind { I8, U8, I16, U16, I32, U32, I64, U64, F, D, LD, P };

  static Type i8() noexcept;
  static Type u8() noexcept;
  static Type i16() noexcept;
  static Type u16() noexcept;
  static Type i32() noexcept;
  static Type u32() noexcept;
  static Type i64() noexcept;
  static Type u64() noexcept;
  static Type f() noexcept;
  static Type d() noexcept;
  static Type ld() noexcept;
  static Type p() noexcept;

  Kind kind() const noexcept;

  friend bool operator==(Type lhs, Type rhs) noexcept { return lhs.kind_ == rhs.kind_; }
  friend bool operator!=(Type lhs, Type rhs) noexcept { return !(lhs == rhs); }

private:
  explicit Type(Kind kind) noexcept;

  Kind kind_;
};

class Register {
public:
  Register() = default;

  std::size_t id() const noexcept;
  Type type() const noexcept;
  std::string_view name() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class Function;

  Register(const Function *function, std::size_t id, Type type, std::string name);

  const Function *function_ = nullptr;
  std::size_t id_ = 0;
  Type type_ = Type::i64();
  std::string name_;
};

class Label {
public:
  Label() = default;

  std::size_t id() const noexcept;
  bool is_valid() const noexcept;
  void begin();
  void end();

  Value arg(std::string_view name);
  Var local(Type type, std::string_view name);
  Value i8(std::int8_t value);
  Value u8(std::uint8_t value);
  Value i16(std::int16_t value);
  Value u16(std::uint16_t value);
  Value i32(std::int32_t value);
  Value u32(std::uint32_t value);
  Value i64(std::int64_t value);
  Value u64(std::uint64_t value);
  Value f32(float value);
  Value f64(double value);
  Value ld(long double value);
  Value alloca(std::int64_t size, std::string_view name);
  Value alloca(Type element_type, std::string_view name);
  Value alloca(Type element_type, std::int64_t count, std::string_view name);
  Memory mem(Type type, Value base);
  Memory mem(Type type, Value base, Value index, int scale = 1,
             std::int64_t displacement = 0);
  Value load(Memory memory);
  void store(Memory memory, Value value);
  void assign(Value dst, Value src);
  Value call(const Prototype &prototype, const Import &callee, std::vector<Value> args);
  Value call(const Prototype &prototype, const Function &callee, std::vector<Value> args);
  CallResult call_multi(const Prototype &prototype, const Import &callee, std::vector<Value> args);
  CallResult call_multi(const Prototype &prototype, const Function &callee, std::vector<Value> args);
  void call_void(const Prototype &prototype, const Import &callee, std::vector<Value> args);
  void call_void(const Prototype &prototype, const Function &callee, std::vector<Value> args);
  void jmp(Label target);
  void if_(Cond cond, Label target);
  void ret(Value value);
  void ret(std::vector<Value> values);
  void ret();

private:
  friend class Function;
  friend class IRBuilder;

  Label(const Function *function, std::size_t id) noexcept;
  Label(const Function *function, std::size_t id, IRBuilder *builder, bool entry) noexcept;

  const Function *function_ = nullptr;
  std::size_t id_ = 0;
  IRBuilder *builder_ = nullptr;
  bool entry_ = false;
};

class Prototype {
public:
  struct Parameter {
    Type type;
    std::string name;
  };

  Prototype(std::string name, std::vector<Type> return_types, std::vector<Parameter> parameters);

  std::string_view name() const noexcept;
  const std::vector<Type> &return_types() const noexcept;
  const std::vector<Parameter> &parameters() const noexcept;

private:
  std::string name_;
  std::vector<Type> return_types_;
  std::vector<Parameter> parameters_;
};

class Import {
public:
  explicit Import(std::string name);

  std::string_view name() const noexcept;

private:
  std::string name_;
};

class Operand {
public:
  enum class Kind { Int64, UInt64, Float32, Float64, LongDouble, Register, LabelRef, Memory, Reference };
  enum class ReferenceKind { None, Prototype, Import, Function };

  Operand(Register reg);
  Operand(Label label);
  Operand(std::int64_t value);
  Operand(std::uint64_t value);

  static Operand reg(Register value);
  static Operand int64(std::int64_t value);
  static Operand uint64(std::uint64_t value);
  static Operand float32(float value);
  static Operand float64(double value);
  static Operand long_double(long double value);
  static Operand label_ref(Label label);
  static Operand label_ref(std::size_t label_id);
  static Operand mem(Type type, Register base, Register index, std::int64_t displacement = 0,
                     int scale = 1);
  static Operand ref(const Prototype &prototype);
  static Operand ref(const Import &import);
  static Operand ref(const Function &function);

  Kind kind() const noexcept;
  std::size_t register_id() const noexcept;
  std::int64_t int64_value() const noexcept;
  std::uint64_t uint64_value() const noexcept;
  float float32_value() const noexcept;
  double float64_value() const noexcept;
  long double long_double_value() const noexcept;
  std::size_t label_id() const noexcept;
  Type memory_type() const noexcept;
  std::int64_t memory_displacement() const noexcept;
  std::size_t memory_base_register_id() const noexcept;
  std::size_t memory_index_register_id() const noexcept;
  int memory_scale() const noexcept;
  ReferenceKind reference_kind() const noexcept;
  const void *reference_pointer() const noexcept;

private:
  friend class IRBuilder;

  Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label);
  Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
          std::int64_t displacement, int scale);
  Operand(ReferenceKind reference_kind, const void *reference);

  Kind kind_;
  std::int64_t int_value_;
  float float32_value_ = 0.0f;
  double float64_value_ = 0.0;
  long double long_double_value_ = 0.0;
  std::size_t register_id_;
  std::size_t label_id_;
  Type memory_type_ = Type::i64();
  std::int64_t memory_displacement_ = 0;
  std::size_t memory_base_register_id_ = 0;
  std::size_t memory_index_register_id_ = 0;
  int memory_scale_ = 1;
  ReferenceKind reference_kind_ = ReferenceKind::None;
  const void *reference_ = nullptr;
};

enum class Opcode {
  Label,
  Ret,
  Nop,
  Mov,
  FMov,
  DMov,
  LDMov,
  Ext8,
  Ext16,
  Ext32,
  UExt8,
  UExt16,
  UExt32,
  I2F,
  I2D,
  I2LD,
  UI2F,
  UI2D,
  UI2LD,
  F2I,
  D2I,
  LD2I,
  F2D,
  F2LD,
  D2F,
  D2LD,
  LD2F,
  LD2D,
  Alloca,
  Neg,
  Negs,
  FNeg,
  DNeg,
  LDNeg,
  Add,
  Adds,
  FAdd,
  DAdd,
  LDAdd,
  Sub,
  Subs,
  FSub,
  DSub,
  LDSub,
  Mul,
  Muls,
  FMul,
  DMul,
  LDMul,
  Div,
  Divs,
  UDiv,
  UDivs,
  FDiv,
  DDiv,
  LDDiv,
  Mod,
  Mods,
  UMod,
  UMods,
  And,
  Ands,
  Or,
  Ors,
  Xor,
  Xors,
  Lsh,
  Lshs,
  Rsh,
  Rshs,
  URsh,
  URshs,
  Eq,
  Eqs,
  FEq,
  DEq,
  LDEq,
  Ne,
  Nes,
  FNe,
  DNe,
  LDNe,
  Lt,
  Lts,
  ULt,
  ULts,
  FLt,
  DLt,
  LDLt,
  Le,
  Les,
  ULe,
  ULes,
  FLe,
  DLe,
  LDLe,
  Gt,
  Gts,
  UGt,
  UGts,
  FGt,
  DGt,
  LDGt,
  Ge,
  Ges,
  UGe,
  UGes,
  FGe,
  DGe,
  LDGe,
  Addo,
  Addos,
  Subo,
  Subos,
  Mulo,
  Mulos,
  UMulo,
  UMulos,
  Jmp,
  Bt,
  Bts,
  Bf,
  Bfs,
  Beq,
  Beqs,
  FBeq,
  DBeq,
  LDBeq,
  Bne,
  Bnes,
  FBne,
  DBne,
  LDBne,
  Blt,
  Blts,
  UBlt,
  UBlts,
  FBlt,
  DBlt,
  LDBlt,
  Ble,
  Bles,
  UBle,
  UBles,
  FBle,
  DBle,
  LDBle,
  Bgt,
  Bgts,
  UBgt,
  UBgts,
  FBgt,
  DBgt,
  LDBgt,
  Bge,
  Bges,
  UBge,
  UBges,
  FBge,
  DBge,
  LDBge,
  Bo,
  UBo,
  Bno,
  UBno,
  Call,
};

class Instruction {
public:
  using Opcode = mirnext::Opcode;

  Instruction(Opcode opcode, std::vector<Operand> operands = {});

  Opcode opcode() const noexcept;
  const std::vector<Operand> &operands() const noexcept;
  std::size_t label_id() const noexcept;

private:
  friend class Function;

  void set_label_id(std::size_t label_id) noexcept;

  Opcode opcode_;
  std::vector<Operand> operands_;
  std::size_t label_id_ = 0;
};

class Function {
public:
  struct Parameter {
    Type type;
    std::string name;
  };

  explicit Function(std::string name);
  Function(std::string name, std::vector<Type> return_types, std::vector<Parameter> parameters);

  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Function(Function &&) = delete;
  Function &operator=(Function &&) = delete;

  std::string_view name() const noexcept;
  const std::vector<Type> &return_types() const noexcept;
  const std::vector<Register> &arguments() const noexcept;
  Result<Register> argument(std::string_view name) const;
  const Register *find_argument(std::string_view name) const noexcept;
  Result<Register> create_register(Type type, std::string_view name);
  const std::vector<Register> &local_registers() const noexcept;
  const Register *find_register(std::string_view name) const noexcept;
  const Register *find_register(std::size_t id) const noexcept;
  Label create_label();
  Instruction &append(Instruction instruction);
  Instruction &append_label();
  Instruction &append_label(Label label);
  Instruction &append_ret(std::vector<Operand> values = {});
  std::size_t instruction_count() const noexcept;
  const std::vector<std::unique_ptr<Instruction>> &instructions() const noexcept;

private:
  Result<Register> create_register_internal(Type type, std::string_view name, bool argument);
  bool has_register_name(std::string_view name) const noexcept;

  std::string name_;
  std::vector<Type> return_types_;
  std::vector<Register> arguments_;
  std::vector<Register> local_registers_;
  std::vector<std::unique_ptr<Instruction>> instructions_;
  std::size_t next_register_id_ = 1;
  std::size_t next_label_id_ = 1;
};

class Module {
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
  Result<std::vector<std::byte>> encode_binary() const;
  const std::vector<std::unique_ptr<Prototype>> &prototypes() const noexcept;
  const std::vector<std::unique_ptr<Import>> &imports() const noexcept;
  const std::vector<std::unique_ptr<Function>> &functions() const noexcept;

private:
  std::string name_;
  std::vector<std::unique_ptr<Prototype>> prototypes_;
  std::vector<std::unique_ptr<Import>> imports_;
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
// Context, Module, Function, and IRBuilder are not internally synchronized.
// It is safe for different threads to build distinct Module instances after
// those modules have been created, as long as no thread concurrently mutates or
// traverses the owning Context as a whole. Likewise, a Module or Function must
// not be concurrently modified from multiple threads without external
// synchronization. Linking, dumping, and lowering a Context must happen after
// all builder threads that mutate it have joined.
class Value {
public:
  Value() noexcept;

  Type type() const noexcept;
  Operand operand() const noexcept;
  IRBuilder *builder() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class IRBuilder;
  friend class Cond;
  friend Value operator+(Value lhs, Value rhs);
  friend Value operator-(Value lhs, Value rhs);
  friend Value operator*(Value lhs, Value rhs);
  friend Value operator/(Value lhs, Value rhs);
  friend Value operator%(Value lhs, Value rhs);
  friend Cond operator==(Value lhs, Value rhs);
  friend Cond operator!=(Value lhs, Value rhs);
  friend Cond operator<(Value lhs, Value rhs);
  friend Cond operator<=(Value lhs, Value rhs);
  friend Cond operator>(Value lhs, Value rhs);
  friend Cond operator>=(Value lhs, Value rhs);

  Value(IRBuilder *builder, std::size_t label_id, Type type, Operand operand) noexcept;

  IRBuilder *builder_;
  std::size_t label_id_;
  Type type_;
  Operand operand_;
};

class Var {
public:
  Var() noexcept;

  operator Value() const noexcept;

  Type type() const noexcept;
  Value value() const noexcept;
  IRBuilder *builder() const noexcept;
  bool is_valid() const noexcept;

  Var &operator=(const Var &rhs);
  Var &operator=(Value rhs);
  Var &operator=(std::int64_t rhs);

private:
  friend class IRBuilder;

  Var(Label label, Value value) noexcept;

  Label label_;
  Value value_;
};

class Memory {
public:
  Memory() noexcept;

  Type type() const noexcept;
  Operand operand() const noexcept;
  IRBuilder *builder() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class IRBuilder;

  Memory(IRBuilder *builder, std::size_t label_id, Type type, Operand operand) noexcept;

  IRBuilder *builder_;
  std::size_t label_id_;
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
  friend class IRBuilder;

  explicit CallResult(std::vector<Value> values) : values_(std::move(values)) {}

  std::vector<Value> values_;
};

class Cond {
public:
  Cond() noexcept;

  IRBuilder *builder() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class IRBuilder;
  friend Cond operator==(Value lhs, Value rhs);
  friend Cond operator!=(Value lhs, Value rhs);
  friend Cond operator<(Value lhs, Value rhs);
  friend Cond operator<=(Value lhs, Value rhs);
  friend Cond operator>(Value lhs, Value rhs);
  friend Cond operator>=(Value lhs, Value rhs);

  Cond(IRBuilder *builder, std::size_t label_id, Opcode opcode, Value lhs, Value rhs) noexcept;

  IRBuilder *builder_;
  std::size_t label_id_;
  Opcode opcode_;
  Value lhs_;
  Value rhs_;
};

class IRBuilder {
public:
  explicit IRBuilder(Function &function) noexcept;

  Label entry();
  Label label();
  bool ok() const noexcept;
  const Error &error() const noexcept;

private:
  struct BlockState {
    std::size_t label_id;
    bool entry;
    bool materialized = false;
    bool closed = false;
  };

  struct InsertPoint {
    std::size_t label_id;
    bool scoped;
  };

  friend class Label;
  friend Value operator+(Value lhs, Value rhs);
  friend Value operator-(Value lhs, Value rhs);
  friend Value operator*(Value lhs, Value rhs);
  friend Value operator/(Value lhs, Value rhs);
  friend Value operator%(Value lhs, Value rhs);
  friend Cond operator==(Value lhs, Value rhs);
  friend Cond operator!=(Value lhs, Value rhs);
  friend Cond operator<(Value lhs, Value rhs);
  friend Cond operator<=(Value lhs, Value rhs);
  friend Cond operator>(Value lhs, Value rhs);
  friend Cond operator>=(Value lhs, Value rhs);
  friend class Var;

  Value arg(Label label, std::string_view name);
  Var local(Label label, Type type, std::string_view name);
  Value i8(Label label, std::int8_t value);
  Value u8(Label label, std::uint8_t value);
  Value i16(Label label, std::int16_t value);
  Value u16(Label label, std::uint16_t value);
  Value i32(Label label, std::int32_t value);
  Value u32(Label label, std::uint32_t value);
  Value i64(Label label, std::int64_t value);
  Value u64(Label label, std::uint64_t value);
  Value f32(Label label, float value);
  Value f64(Label label, double value);
  Value ld(Label label, long double value);
  Value alloca(Label label, std::int64_t size, std::string_view name);
  Value alloca(Label label, Type element_type, std::string_view name);
  Value alloca(Label label, Type element_type, std::int64_t count, std::string_view name);
  Memory mem(Label label, Type type, Value base);
  Memory mem(Label label, Type type, Value base, Value index, int scale,
             std::int64_t displacement);
  Value load(Label label, Memory memory);
  void store(Label label, Memory memory, Value value);
  void assign(Label label, Value dst, Value src);
  Value call(Label label, const Prototype &prototype, const Import &callee,
             std::vector<Value> args);
  Value call(Label label, const Prototype &prototype, const Function &callee,
             std::vector<Value> args);
  CallResult call_multi(Label label, const Prototype &prototype, const Import &callee,
                        std::vector<Value> args);
  CallResult call_multi(Label label, const Prototype &prototype, const Function &callee,
                        std::vector<Value> args);
  void call_void(Label label, const Prototype &prototype, const Import &callee,
                 std::vector<Value> args);
  void call_void(Label label, const Prototype &prototype, const Function &callee,
                 std::vector<Value> args);
  void jmp(Label label, Label target);
  void if_(Label label, Cond cond, Label target);
  void ret(Label label, Value value);
  void ret(Label label, std::vector<Value> values);
  void ret(Label label);

  void begin(Label label);
  void end(Label label);
  void fail(Error error);
  Label current_or(Label fallback) noexcept;
  Value integer_literal(Label label, Type type, std::int64_t value);
  Value invalid_value() noexcept;
  Memory invalid_memory() noexcept;
  Cond invalid_cond() noexcept;
  Register temp(Type type);
  std::optional<Opcode> typed_move_opcode(Type type) noexcept;
  Operand memory_operand(Type type, const Value &base, std::size_t index_register,
                         std::int64_t displacement, int scale);
  bool validate_value(Label label, const Value &value, std::string_view description);
  bool validate_register_value(Label label, const Value &value, std::string_view description);
  bool validate_memory(Label label, const Memory &memory);
  std::optional<std::size_t> resolve_binary_label(Value lhs, Value rhs);
  CallResult append_call(Label label, const Prototype &prototype, Operand callee,
                         std::vector<Value> args, std::optional<std::size_t> return_count);
  Value append_binary(Opcode opcode, Value lhs, Value rhs);
  Cond compare(Opcode opcode, Value lhs, Value rhs);
  bool validate_call_args(Label label, const Prototype &prototype, const std::vector<Value> &args);
  bool validate_binary(Value lhs, Value rhs, bool require_integer = false);
  bool validate_label(Label label);
  bool validate_target(Label target);
  BlockState *find_block(std::size_t label_id) noexcept;
  bool materialize_block(Label label);
  bool ensure_block(Label label);
  Label create_attached_label(bool entry);
  void append_branch(Label label, Opcode opcode, Label target);
  void append_branch(Label label, Opcode opcode, Label target, Operand lhs, Operand rhs);

  Function *function_;
  std::optional<Error> error_;
  std::vector<BlockState> blocks_;
  std::optional<std::size_t> entry_label_id_;
  std::vector<InsertPoint> insert_stack_;
  bool first_block_started_ = false;
  std::size_t next_temp_id_ = 0;
};

Value operator+(Value lhs, Value rhs);
Value operator-(Value lhs, Value rhs);
Value operator*(Value lhs, Value rhs);
Value operator/(Value lhs, Value rhs);
Value operator%(Value lhs, Value rhs);
Cond operator==(Value lhs, Value rhs);
Cond operator!=(Value lhs, Value rhs);
Cond operator<(Value lhs, Value rhs);
Cond operator<=(Value lhs, Value rhs);
Cond operator>(Value lhs, Value rhs);
Cond operator>=(Value lhs, Value rhs);

const char *opcode_name(Opcode opcode) noexcept;
const char *type_name(Type type) noexcept;

Result<std::vector<std::byte>> encode_binary(const Module &module);

} // namespace mirnext

#endif
