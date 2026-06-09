/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_HPP
#define MIRNEXT_HPP

#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mirnext {

class Function;

enum class ErrorCode { InvalidArgument, DuplicateName, UnknownName, NoInsertPoint, InvalidOperand };

struct Error {
  ErrorCode code;
  std::string message;
};

template <class T> class Result {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return has_value(); }

  T &value() noexcept {
    if (T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  const T &value() const noexcept {
    if (const T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  Error &error() noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  const Error &error() const noexcept {
    if (const Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

private:
  std::variant<T, Error> storage_;
};

class Type {
public:
  enum class Kind { I64, U8 };

  static Type i64() noexcept;
  static Type u8() noexcept;

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

private:
  friend class Function;

  Label(const Function *function, std::size_t id) noexcept;

  const Function *function_ = nullptr;
  std::size_t id_ = 0;
};

class Operand {
public:
  enum class Kind { Int64, Register, LabelRef, Memory };

  Operand(Register reg);
  Operand(Label label);
  Operand(std::int64_t value);

  static Operand reg(Register value);
  static Operand int64(std::int64_t value);
  static Operand label_ref(Label label);
  static Operand label_ref(std::size_t label_id);
  static Operand mem(Type type, Register base, Register index, std::int64_t displacement = 0,
                     int scale = 1);

  Kind kind() const noexcept;
  std::size_t register_id() const noexcept;
  std::int64_t int64_value() const noexcept;
  std::size_t label_id() const noexcept;
  Type memory_type() const noexcept;
  std::int64_t memory_displacement() const noexcept;
  std::size_t memory_base_register_id() const noexcept;
  std::size_t memory_index_register_id() const noexcept;
  int memory_scale() const noexcept;

private:
  Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label);
  Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
          std::int64_t displacement, int scale);

  Kind kind_;
  std::int64_t int_value_;
  std::size_t register_id_;
  std::size_t label_id_;
  Type memory_type_ = Type::i64();
  std::int64_t memory_displacement_ = 0;
  std::size_t memory_base_register_id_ = 0;
  std::size_t memory_index_register_id_ = 0;
  int memory_scale_ = 1;
};

enum class Opcode { Label, Ret, Nop, Mov, Add, Bge, Blt, Alloca, Jmp, Beq };

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
  const std::vector<std::unique_ptr<Function>> &functions() const noexcept;

private:
  std::string name_;
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

class IRBuilder {
public:
  explicit IRBuilder(Context &context) noexcept;

  void set_insert_point(Function &function) noexcept;
  Function *insert_point() const noexcept;

  Result<Label> create_label();
  Result<Instruction *> bind(Label label);
  Result<Instruction *> create_alloca(Register dst, Operand size);
  Result<Instruction *> create_mov(Operand dst, Operand src);
  Result<Instruction *> create_mov(Register dst, Operand src);
  Result<Instruction *> create_add(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_jmp(Label target);
  Result<Instruction *> create_beq(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_blt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ret(std::vector<Operand> values = {});

private:
  Result<Function *> require_insert_point() const;

  Context *context_;
  Function *insert_point_ = nullptr;
};

const char *opcode_name(Opcode opcode) noexcept;
const char *type_name(Type type) noexcept;

} // namespace mirnext

#endif
