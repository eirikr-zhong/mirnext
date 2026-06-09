/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_HPP
#define MIRNEXT_HPP

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mirnext {

class Operand {
public:
  enum class Kind { Int64, LabelRef };

  static Operand int64(std::int64_t value);
  static Operand label_ref(std::size_t label_id);

  Kind kind() const noexcept;
  std::int64_t int64_value() const noexcept;
  std::size_t label_id() const noexcept;

private:
  Operand(Kind kind, std::int64_t int_value, std::size_t label);

  Kind kind_;
  std::int64_t int_value_;
  std::size_t label_id_;
};

class Instruction {
public:
  enum class Opcode { Label, Ret, Nop };

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
  explicit Function(std::string name);

  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Function(Function &&) = delete;
  Function &operator=(Function &&) = delete;

  std::string_view name() const noexcept;
  Instruction &append(Instruction instruction);
  Instruction &append_label();
  Instruction &append_ret();
  std::size_t instruction_count() const noexcept;
  const std::vector<std::unique_ptr<Instruction>> &instructions() const noexcept;

private:
  std::string name_;
  std::vector<std::unique_ptr<Instruction>> instructions_;
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

const char *opcode_name(Instruction::Opcode opcode) noexcept;

} // namespace mirnext

#endif
