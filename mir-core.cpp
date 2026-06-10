/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <utility>

namespace mirnext {

Type::Type(Kind kind) noexcept : kind_(kind) {}

Type Type::i8() noexcept { return Type(Kind::I8); }

Type Type::u8() noexcept { return Type(Kind::U8); }

Type Type::i16() noexcept { return Type(Kind::I16); }

Type Type::u16() noexcept { return Type(Kind::U16); }

Type Type::i32() noexcept { return Type(Kind::I32); }

Type Type::u32() noexcept { return Type(Kind::U32); }

Type Type::i64() noexcept { return Type(Kind::I64); }

Type Type::u64() noexcept { return Type(Kind::U64); }

Type Type::f() noexcept { return Type(Kind::F); }

Type Type::d() noexcept { return Type(Kind::D); }

Type Type::ld() noexcept { return Type(Kind::LD); }

Type Type::p() noexcept { return Type(Kind::P); }

Type::Kind Type::kind() const noexcept { return kind_; }

Register::Register(const Function *function, std::size_t id, Type type, std::string name)
    : function_(function), id_(id), type_(type), name_(std::move(name)) {}

std::size_t Register::id() const noexcept { return id_; }

Type Register::type() const noexcept { return type_; }

std::string_view Register::name() const noexcept { return name_; }

bool Register::is_valid() const noexcept { return function_ != nullptr && id_ != 0; }

Label::Label(const Function *function, std::size_t id) noexcept : function_(function), id_(id) {}

Label::Label(const Function *function, std::size_t id, IRBuilder *builder, bool entry) noexcept
    : function_(function), id_(id), builder_(builder), entry_(entry) {}

std::size_t Label::id() const noexcept { return id_; }

bool Label::is_valid() const noexcept { return function_ != nullptr && id_ != 0; }

Prototype::Prototype(std::string name, std::vector<Type> return_types,
                     std::vector<Parameter> parameters)
    : name_(std::move(name)), return_types_(std::move(return_types)),
      parameters_(std::move(parameters)) {}

std::string_view Prototype::name() const noexcept { return name_; }

const std::vector<Type> &Prototype::return_types() const noexcept { return return_types_; }

const std::vector<Prototype::Parameter> &Prototype::parameters() const noexcept {
  return parameters_;
}

Import::Import(std::string name) : name_(std::move(name)) {}

std::string_view Import::name() const noexcept { return name_; }

Operand::Operand(Register reg) : Operand(Kind::Register, 0, reg.id(), 0) {}

Operand::Operand(Label label) : Operand(Kind::LabelRef, 0, 0, label.id()) {}

Operand::Operand(std::int64_t value) : Operand(Kind::Int64, value, 0, 0) {}

Operand::Operand(std::uint64_t value)
    : Operand(Kind::UInt64, static_cast<std::int64_t>(value), 0, 0) {}

Operand::Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label)
    : kind_(kind), int_value_(int_value), register_id_(reg), label_id_(label) {}

Operand::Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
                 std::int64_t displacement, int scale)
    : kind_(Kind::Memory), int_value_(0), register_id_(0), label_id_(0),
      memory_type_(memory_type), memory_displacement_(displacement),
      memory_base_register_id_(base_register), memory_index_register_id_(index_register),
      memory_scale_(scale) {}

Operand::Operand(ReferenceKind reference_kind, const void *reference)
    : kind_(Kind::Reference), int_value_(0), register_id_(0), label_id_(0),
      reference_kind_(reference_kind), reference_(reference) {}

Operand Operand::reg(Register value) { return Operand(value); }

Operand Operand::int64(std::int64_t value) { return Operand(value); }

Operand Operand::uint64(std::uint64_t value) { return Operand(value); }

Operand Operand::float32(float value) {
  Operand operand(Kind::Float32, 0, 0, 0);
  operand.float32_value_ = value;
  return operand;
}

Operand Operand::float64(double value) {
  Operand operand(Kind::Float64, 0, 0, 0);
  operand.float64_value_ = value;
  return operand;
}

Operand Operand::long_double(long double value) {
  Operand operand(Kind::LongDouble, 0, 0, 0);
  operand.long_double_value_ = value;
  return operand;
}

Operand Operand::label_ref(Label label) { return Operand(label); }

Operand Operand::label_ref(std::size_t label_id) { return Operand(Kind::LabelRef, 0, 0, label_id); }

Operand Operand::mem(Type type, Register base, Register index, std::int64_t displacement,
                     int scale) {
  return Operand(type, base.id(), index.id(), displacement, scale);
}

Operand Operand::ref(const Prototype &prototype) {
  return Operand(ReferenceKind::Prototype, &prototype);
}

Operand Operand::ref(const Import &import) { return Operand(ReferenceKind::Import, &import); }

Operand Operand::ref(const Function &function) {
  return Operand(ReferenceKind::Function, &function);
}

Operand::Kind Operand::kind() const noexcept { return kind_; }

std::size_t Operand::register_id() const noexcept { return register_id_; }

std::int64_t Operand::int64_value() const noexcept { return int_value_; }

std::uint64_t Operand::uint64_value() const noexcept {
  return static_cast<std::uint64_t>(int_value_);
}

float Operand::float32_value() const noexcept { return float32_value_; }

double Operand::float64_value() const noexcept { return float64_value_; }

long double Operand::long_double_value() const noexcept { return long_double_value_; }

std::size_t Operand::label_id() const noexcept { return label_id_; }

Type Operand::memory_type() const noexcept { return memory_type_; }

std::int64_t Operand::memory_displacement() const noexcept { return memory_displacement_; }

std::size_t Operand::memory_base_register_id() const noexcept { return memory_base_register_id_; }

std::size_t Operand::memory_index_register_id() const noexcept { return memory_index_register_id_; }

int Operand::memory_scale() const noexcept { return memory_scale_; }

Operand::ReferenceKind Operand::reference_kind() const noexcept { return reference_kind_; }

const void *Operand::reference_pointer() const noexcept { return reference_; }

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

Prototype &Module::new_prototype(std::string_view name, std::vector<Type> return_types,
                                 std::vector<Prototype::Parameter> parameters) {
  prototypes_.push_back(std::make_unique<Prototype>(std::string(name), std::move(return_types),
                                                    std::move(parameters)));
  return *prototypes_.back();
}

Import &Module::new_import(std::string_view name) {
  imports_.push_back(std::make_unique<Import>(std::string(name)));
  return *imports_.back();
}

const std::vector<std::unique_ptr<Prototype>> &Module::prototypes() const noexcept {
  return prototypes_;
}

const std::vector<std::unique_ptr<Import>> &Module::imports() const noexcept { return imports_; }

const std::vector<std::unique_ptr<Function>> &Module::functions() const noexcept {
  return functions_;
}

Module &Context::new_module(std::string_view name) {
  modules_.push_back(std::make_unique<Module>(std::string(name)));
  return *modules_.back();
}

const std::vector<std::unique_ptr<Module>> &Context::modules() const noexcept { return modules_; }


} // namespace mirnext
