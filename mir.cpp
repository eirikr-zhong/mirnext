/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <ostream>
#include <utility>

namespace mirnext {

Type::Type(Kind kind) noexcept : kind_(kind) {}

Type Type::i64() noexcept { return Type(Kind::I64); }

Type Type::u8() noexcept { return Type(Kind::U8); }

Type::Kind Type::kind() const noexcept { return kind_; }

Register::Register(const Function *function, std::size_t id, Type type, std::string name)
    : function_(function), id_(id), type_(type), name_(std::move(name)) {}

std::size_t Register::id() const noexcept { return id_; }

Type Register::type() const noexcept { return type_; }

std::string_view Register::name() const noexcept { return name_; }

bool Register::is_valid() const noexcept { return function_ != nullptr && id_ != 0; }

Label::Label(const Function *function, std::size_t id) noexcept : function_(function), id_(id) {}

std::size_t Label::id() const noexcept { return id_; }

bool Label::is_valid() const noexcept { return function_ != nullptr && id_ != 0; }

Operand::Operand(Register reg) : Operand(Kind::Register, 0, reg.id(), 0) {}

Operand::Operand(Label label) : Operand(Kind::LabelRef, 0, 0, label.id()) {}

Operand::Operand(std::int64_t value) : Operand(Kind::Int64, value, 0, 0) {}

Operand::Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label)
    : kind_(kind), int_value_(int_value), register_id_(reg), label_id_(label) {}

Operand::Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
                 std::int64_t displacement, int scale)
    : kind_(Kind::Memory), int_value_(0), register_id_(0), label_id_(0),
      memory_type_(memory_type), memory_displacement_(displacement),
      memory_base_register_id_(base_register), memory_index_register_id_(index_register),
      memory_scale_(scale) {}

Operand Operand::reg(Register value) { return Operand(value); }

Operand Operand::int64(std::int64_t value) { return Operand(value); }

Operand Operand::label_ref(Label label) { return Operand(label); }

Operand Operand::label_ref(std::size_t label_id) { return Operand(Kind::LabelRef, 0, 0, label_id); }

Operand Operand::mem(Type type, Register base, Register index, std::int64_t displacement,
                     int scale) {
  return Operand(type, base.id(), index.id(), displacement, scale);
}

Operand::Kind Operand::kind() const noexcept { return kind_; }

std::size_t Operand::register_id() const noexcept { return register_id_; }

std::int64_t Operand::int64_value() const noexcept { return int_value_; }

std::size_t Operand::label_id() const noexcept { return label_id_; }

Type Operand::memory_type() const noexcept { return memory_type_; }

std::int64_t Operand::memory_displacement() const noexcept { return memory_displacement_; }

std::size_t Operand::memory_base_register_id() const noexcept { return memory_base_register_id_; }

std::size_t Operand::memory_index_register_id() const noexcept { return memory_index_register_id_; }

int Operand::memory_scale() const noexcept { return memory_scale_; }

Instruction::Instruction(Opcode opcode, std::vector<Operand> operands)
    : opcode_(opcode), operands_(std::move(operands)) {}

Instruction::Opcode Instruction::opcode() const noexcept { return opcode_; }

const std::vector<Operand> &Instruction::operands() const noexcept { return operands_; }

std::size_t Instruction::label_id() const noexcept { return label_id_; }

void Instruction::set_label_id(std::size_t label_id) noexcept { label_id_ = label_id; }

Function::Function(std::string name) : name_(std::move(name)) {}

Function::Function(std::string name, std::vector<Type> return_types, std::vector<Parameter> parameters)
    : name_(std::move(name)), return_types_(std::move(return_types)) {
  for (const Parameter &parameter : parameters) {
    create_register_internal(parameter.type, parameter.name, true);
  }
}

std::string_view Function::name() const noexcept { return name_; }

const std::vector<Type> &Function::return_types() const noexcept { return return_types_; }

const std::vector<Register> &Function::arguments() const noexcept { return arguments_; }

Result<Register> Function::argument(std::string_view name) const {
  if (const Register *reg = find_argument(name)) return *reg;
  return Error{ErrorCode::UnknownName, "unknown function argument"};
}

const Register *Function::find_argument(std::string_view name) const noexcept {
  for (const Register &reg : arguments_) {
    if (reg.name() == name) return &reg;
  }
  return nullptr;
}

Result<Register> Function::create_register(Type type, std::string_view name) {
  return create_register_internal(type, name, false);
}

const std::vector<Register> &Function::local_registers() const noexcept { return local_registers_; }

const Register *Function::find_register(std::string_view name) const noexcept {
  for (const Register &reg : arguments_) {
    if (reg.name() == name) return &reg;
  }
  for (const Register &reg : local_registers_) {
    if (reg.name() == name) return &reg;
  }
  return nullptr;
}

const Register *Function::find_register(std::size_t id) const noexcept {
  for (const Register &reg : arguments_) {
    if (reg.id() == id) return &reg;
  }
  for (const Register &reg : local_registers_) {
    if (reg.id() == id) return &reg;
  }
  return nullptr;
}

Label Function::create_label() { return Label(this, next_label_id_++); }

Instruction &Function::append(Instruction instruction) {
  instructions_.push_back(std::make_unique<Instruction>(std::move(instruction)));
  return *instructions_.back();
}

Instruction &Function::append_label() {
  return append_label(create_label());
}

Instruction &Function::append_label(Label label) {
  Instruction instruction(Opcode::Label);
  instruction.set_label_id(label.id());
  return append(std::move(instruction));
}

Instruction &Function::append_ret(std::vector<Operand> values) {
  return append(Instruction(Opcode::Ret, std::move(values)));
}

std::size_t Function::instruction_count() const noexcept { return instructions_.size(); }

const std::vector<std::unique_ptr<Instruction>> &Function::instructions() const noexcept {
  return instructions_;
}

Result<Register> Function::create_register_internal(Type type, std::string_view name, bool argument) {
  if (name.empty()) return Error{ErrorCode::InvalidArgument, "register name must not be empty"};
  if (has_register_name(name)) return Error{ErrorCode::DuplicateName, "duplicate register name"};

  std::vector<Register> &registers = argument ? arguments_ : local_registers_;
  registers.push_back(Register(this, next_register_id_++, type, std::string(name)));
  return registers.back();
}

bool Function::has_register_name(std::string_view name) const noexcept {
  return find_register(name) != nullptr;
}

Module::Module(std::string name) : name_(std::move(name)) {}

std::string_view Module::name() const noexcept { return name_; }

Function &Module::new_function(std::string_view name) {
  functions_.push_back(std::make_unique<Function>(std::string(name)));
  return *functions_.back();
}

Function &Module::new_function(std::string_view name, std::vector<Type> return_types,
                               std::vector<Function::Parameter> parameters) {
  functions_.push_back(std::make_unique<Function>(std::string(name), std::move(return_types),
                                                  std::move(parameters)));
  return *functions_.back();
}

const std::vector<std::unique_ptr<Function>> &Module::functions() const noexcept {
  return functions_;
}

Module &Context::new_module(std::string_view name) {
  modules_.push_back(std::make_unique<Module>(std::string(name)));
  return *modules_.back();
}

const std::vector<std::unique_ptr<Module>> &Context::modules() const noexcept { return modules_; }

static void dump_type_list(std::ostream &out, const std::vector<Type> &types) {
  if (types.empty()) {
    out << "void";
    return;
  }
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (i != 0) out << ", ";
    out << type_name(types[i]);
  }
}

static void dump_operand(std::ostream &out, const Function &function, const Operand &operand) {
  auto dump_register_id = [&out, &function](std::size_t register_id) {
    if (const Register *reg = function.find_register(register_id)) {
      out << '%' << reg->name();
    } else {
      out << "%r" << register_id;
    }
  };

  switch (operand.kind()) {
  case Operand::Kind::Int64:
    out << operand.int64_value();
    break;
  case Operand::Kind::Register:
    dump_register_id(operand.register_id());
    break;
  case Operand::Kind::LabelRef:
    out << "L" << operand.label_id();
    break;
  case Operand::Kind::Memory:
    out << type_name(operand.memory_type()) << ":(";
    dump_register_id(operand.memory_base_register_id());
    out << ", ";
    dump_register_id(operand.memory_index_register_id());
    out << ", " << operand.memory_scale();
    if (operand.memory_displacement() != 0) out << ", " << operand.memory_displacement();
    out << ')';
    break;
  }
}

void Context::dump(std::ostream &out) const {
  for (const auto &module : modules_) {
    out << "module " << module->name() << '\n';
    for (const auto &function : module->functions()) {
      out << "  func " << function->name() << '(';
      for (std::size_t i = 0; i < function->arguments().size(); ++i) {
        if (i != 0) out << ", ";
        const Register &arg = function->arguments()[i];
        out << type_name(arg.type()) << " %" << arg.name();
      }
      out << ") -> ";
      dump_type_list(out, function->return_types());
      out << '\n';
      for (const Register &reg : function->local_registers()) {
        out << "    reg " << type_name(reg.type()) << " %" << reg.name() << '\n';
      }
      for (const auto &instruction : function->instructions()) {
        out << "    " << opcode_name(instruction->opcode());
        if (instruction->opcode() == Opcode::Label) {
          out << " L" << instruction->label_id();
        } else {
          for (const Operand &operand : instruction->operands()) {
            out << ' ';
            dump_operand(out, *function, operand);
          }
        }
        out << '\n';
      }
    }
  }
}

IRBuilder::IRBuilder(Context &context) noexcept : context_(&context) {}

void IRBuilder::set_insert_point(Function &function) noexcept { insert_point_ = &function; }

Function *IRBuilder::insert_point() const noexcept { return insert_point_; }

Result<Label> IRBuilder::create_label() {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return function.value()->create_label();
}

Result<Instruction *> IRBuilder::bind(Label label) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append_label(label);
}

Result<Instruction *> IRBuilder::create_alloca(Register dst, Operand size) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Alloca, {Operand(dst), size}));
}

Result<Instruction *> IRBuilder::create_mov(Operand dst, Operand src) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Mov, {dst, src}));
}

Result<Instruction *> IRBuilder::create_mov(Register dst, Operand src) {
  return create_mov(Operand(dst), src);
}

Result<Instruction *> IRBuilder::create_add(Register dst, Operand lhs, Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Add, {Operand(dst), lhs, rhs}));
}

Result<Instruction *> IRBuilder::create_jmp(Label target) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Jmp, {Operand(target)}));
}

Result<Instruction *> IRBuilder::create_beq(Label target, Operand lhs, Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Beq, {Operand(target), lhs, rhs}));
}

Result<Instruction *> IRBuilder::create_bge(Label target, Operand lhs, Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Bge, {Operand(target), lhs, rhs}));
}

Result<Instruction *> IRBuilder::create_blt(Label target, Operand lhs, Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Blt, {Operand(target), lhs, rhs}));
}

Result<Instruction *> IRBuilder::create_ret(std::vector<Operand> values) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append_ret(std::move(values));
}

Result<Function *> IRBuilder::require_insert_point() const {
  if (insert_point_ == nullptr) {
    return Error{ErrorCode::NoInsertPoint, "builder has no insert point"};
  }
  return insert_point_;
}

const char *opcode_name(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::Label: return "label";
  case Opcode::Ret: return "ret";
  case Opcode::Nop: return "nop";
  case Opcode::Mov: return "mov";
  case Opcode::Add: return "add";
  case Opcode::Bge: return "bge";
  case Opcode::Blt: return "blt";
  case Opcode::Alloca: return "alloca";
  case Opcode::Jmp: return "jmp";
  case Opcode::Beq: return "beq";
  }
  return "unknown";
}

const char *type_name(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I64: return "i64";
  case Type::Kind::U8: return "u8";
  }
  return "unknown";
}

} // namespace mirnext
