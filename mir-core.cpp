/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <utility>

namespace mirnext {

Type::Type(Kind kind) noexcept : kind_(kind) {}

Type Type::b() noexcept { return Type(Kind::B); }
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
Type Type::from_kind(Kind kind) noexcept { return Type(kind); }
Type::Kind Type::kind() const noexcept { return kind_; }

Register::Register(const Function *function, std::size_t id, Type type, std::string name)
    : function_(function), id_(id), type_(type), name_(std::move(name)) {}

std::size_t Register::id() const noexcept { return id_; }
Type Register::type() const noexcept { return type_; }
std::string_view Register::name() const noexcept { return name_; }
bool Register::is_valid() const noexcept { return function_ != nullptr && id_ != 0; }

Data::Data(std::string name, std::size_t size)
    : name_(std::move(name)), kind_(Kind::Bss), size_(size) {}

Data::Data(std::string name, Type element_type, ValueKind value_kind,
           std::vector<Operand> values)
    : name_(std::move(name)), kind_(Kind::Typed), element_type_(element_type),
      value_kind_(value_kind), values_(std::move(values)) {}

Data::Data(std::string name, std::string value)
    : name_(std::move(name)), kind_(Kind::String), string_value_(std::move(value)) {}

Data::Data(std::string name, RefTarget target)
    : name_(std::move(name)), kind_(Kind::Ref), ref_target_(target) {}

std::string_view Data::name() const noexcept { return name_; }
bool Data::is_valid() const noexcept { return true; }
Data::Kind Data::kind() const noexcept { return kind_; }
Type Data::element_type() const noexcept { return element_type_; }
Data::ValueKind Data::value_kind() const noexcept { return value_kind_; }
std::size_t Data::size() const noexcept { return size_; }
const std::vector<Operand> &Data::values() const noexcept { return values_; }
std::string_view Data::string_value() const noexcept { return string_value_; }
const Data::RefTarget &Data::ref_target() const noexcept { return ref_target_; }

Operand::Operand(Register reg) : Operand(Kind::Register, 0, reg.id(), 0) {}
Operand::Operand(const Label &label) : Operand(Kind::LabelRef, 0, 0, label.id()) {}
Operand::Operand(std::int64_t value) : Operand(Kind::Int64, value, 0, 0) {}
Operand::Operand(std::uint64_t value)
    : Operand(Kind::UInt64, static_cast<std::int64_t>(value), 0, 0) {}

Operand Operand::poison() { return Operand(Kind::Poison, 0, 0, 0); }
Operand::Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label)
    : kind_(kind), int_value_(int_value), register_id_(reg), label_id_(label) {}

Operand::Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
                 std::int64_t displacement, int scale, std::string_view alias,
                 std::string_view nonalias)
    : kind_(Kind::Memory), int_value_(0), register_id_(0), label_id_(0),
      memory_type_(memory_type), memory_displacement_(displacement),
      memory_base_register_id_(base_register), memory_index_register_id_(index_register),
      memory_scale_(scale), memory_alias_(alias), memory_nonalias_(nonalias) {}

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

Operand Operand::label_ref(const Label &label) { return Operand(label); }
Operand Operand::label_ref(std::size_t label_id) { return Operand(Kind::LabelRef, 0, 0, label_id); }

Operand Operand::mem(Type type, Register base, Register index, std::int64_t displacement,
                     int scale) {
  return Operand::mem(type, base.id(), index.id(), displacement, scale, {}, {});
}

Operand Operand::mem(Type type, std::size_t base_register, std::size_t index_register,
                     std::int64_t displacement, int scale) {
  return Operand::mem(type, base_register, index_register, displacement, scale, {}, {});
}

Operand Operand::mem(Type type, std::size_t base_register, std::size_t index_register,
                     std::int64_t displacement, int scale, std::string_view alias,
                     std::string_view nonalias) {
  return Operand(type, base_register, index_register, displacement, scale, alias, nonalias);
}

Operand Operand::module_slot(std::size_t id) {
  Operand operand(Kind::ModuleSlot, 0, 0, 0);
  operand.module_slot_id_ = id;
  return operand;
}

Operand Operand::ref(const Function &function) {
  return Operand(ReferenceKind::Function, &function);
}

Operand Operand::ref(const Data &data) {
  return Operand(ReferenceKind::Data, &data);
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
std::size_t Operand::module_slot_id() const noexcept { return module_slot_id_; }
Type Operand::memory_type() const noexcept { return memory_type_; }
std::int64_t Operand::memory_displacement() const noexcept { return memory_displacement_; }
std::size_t Operand::memory_base_register_id() const noexcept { return memory_base_register_id_; }
std::size_t Operand::memory_index_register_id() const noexcept { return memory_index_register_id_; }
int Operand::memory_scale() const noexcept { return memory_scale_; }
std::string_view Operand::memory_alias() const noexcept { return memory_alias_; }
std::string_view Operand::memory_nonalias() const noexcept { return memory_nonalias_; }
bool Operand::has_memory_alias_metadata() const noexcept {
  return !memory_alias_.empty() || !memory_nonalias_.empty();
}
Operand::ReferenceKind Operand::reference_kind() const noexcept { return reference_kind_; }
const void *Operand::reference_pointer() const noexcept { return reference_; }

Instruction::Instruction(Opcode opcode, std::vector<Operand> operands)
    : opcode_(opcode), operands_(std::move(operands)) {}

Instruction::Opcode Instruction::opcode() const noexcept { return opcode_; }
const std::vector<Operand> &Instruction::operands() const noexcept { return operands_; }
std::size_t Instruction::label_id() const noexcept { return label_id_; }
void Instruction::set_label_id(std::size_t label_id) noexcept { label_id_ = label_id; }

Block::Block(Kind kind, Block *parent) noexcept : kind_(kind), parent_(parent) {}
Block::~Block() = default;

Block::ModuleBinding::ModuleBinding(Type type, std::string name, bool writable)
    : type(type), name(std::move(name)), writable(writable) {}

Block::Kind Block::kind() const noexcept { return kind_; }
Block *Block::parent() const noexcept { return parent_; }

Module &Block::module() noexcept { return *module_; }
const Module &Block::module() const noexcept { return *module_; }
Function *Block::function() noexcept { return function_; }
const Function *Block::function() const noexcept { return function_; }
const std::optional<Error> &Block::error() const noexcept { return module().error(); }

void Block::set_module(Module *module) noexcept { module_ = module; }
void Block::set_function(Function *function) noexcept { function_ = function; }

void Block::fail(Error error) {
  if (module_ != nullptr) module_->fail(std::move(error));
}

Instruction &Block::append_operation(Instruction instruction) {
  static Instruction dummy(Opcode::Nop);
  if (error()) return dummy;
  Function *owner = function();
  if (owner != nullptr && !owner->is_local()) {
    fail(Error{ErrorCode::InvalidOperand, "only local functions can contain instructions"});
    return dummy;
  }
  if (owner != nullptr) owner->invalidate_flattened();
  pending_overflow_sequence_ = 0;
  operations_.push_back(std::make_unique<Instruction>(std::move(instruction)));
  return *operations_.back();
}

Label &Block::create_child_label(std::string_view name, LabelOptions options) {
  static Module dummy_module(".invalid");
  static Function dummy_function(dummy_module, ".invalid");
  static Label dummy(dummy_function, dummy_function, 0, "", {});
  if (error()) return dummy;
  Function *owner = function();
  if (owner == nullptr) {
    fail(Error{ErrorCode::InvalidOperand, "label requires a function block"});
    return dummy;
  }
  if (owner != nullptr && !owner->is_local()) {
    fail(Error{ErrorCode::InvalidOperand, "only local functions can contain labels"});
    return dummy;
  }
  std::optional<Register> stack_scope_token;
  if (options.stack_scope) {
    std::size_t attempt = 0;
    while (true) {
      const std::string token_name = ".scope" + std::to_string(attempt++);
      Result<Register> token = owner->create_register_internal(Type::p(), token_name, false);
      if (token) {
        stack_scope_token = *token;
        break;
      }
      if (token.error().code != ErrorCode::DuplicateName) {
        fail(std::move(token).error());
        return dummy;
      }
    }
  }
  const std::size_t id = owner->next_label_id_++;
  child_labels_.push_back(std::unique_ptr<Label>(
      new Label(*owner, *this, id, std::string(name), options)));
  child_labels_.back()->stack_scope_token_ = stack_scope_token;
  owner->invalidate_flattened();
  return *child_labels_.back();
}

const std::vector<std::unique_ptr<Instruction>> &Block::operations() const noexcept {
  return operations_;
}

const std::vector<std::unique_ptr<Label>> &Block::child_labels() const noexcept {
  return child_labels_;
}

Label::Label(Function &function, Block &parent, std::size_t id, std::string name,
             LabelOptions options)
    : Block(Kind::Label, &parent), id_(id), name_(std::move(name)),
      options_(options) {
  set_module(&function.module());
  set_function(&function);
}

std::size_t Label::id() const noexcept { return id_; }
std::string_view Label::name() const noexcept { return name_; }
bool Label::is_valid() const noexcept { return id_ != 0 && function() != nullptr; }
bool Label::is_stack_scoped() const noexcept { return options_.stack_scope; }
Label &Label::label(std::string_view name, LabelOptions options) {
  return create_child_label(name, options);
}
void Label::end() {}

Function::Function(Module &module, std::string name)
    : Block(Kind::Function, &module), name_(std::move(name)) {
  set_module(&module);
  set_function(this);
}

Function::Function(Module &module, std::string name, std::vector<Type> return_types,
                   std::vector<Parameter> parameters, Linkage linkage, bool vararg)
    : Block(Kind::Function, &module), name_(std::move(name)),
      return_types_(std::move(return_types)), linkage_(linkage), vararg_(vararg) {
  set_module(&module);
  set_function(this);
  for (const Parameter &parameter : parameters) {
    if (!create_register_internal(parameter.type, parameter.name, true)) std::abort();
  }
}

std::string_view Function::name() const noexcept { return name_; }
const std::vector<Type> &Function::return_types() const noexcept { return return_types_; }
Function::Linkage Function::linkage() const noexcept { return linkage_; }
bool Function::is_local() const noexcept { return linkage_ == Linkage::Local; }
bool Function::is_import() const noexcept { return linkage_ == Linkage::Import; }
bool Function::is_signature() const noexcept { return linkage_ == Linkage::Signature; }
bool Function::is_vararg() const noexcept { return vararg_; }
bool Function::is_inline() const noexcept { return inline_hint_; }
Function &Function::set_inline(bool enabled) noexcept {
  inline_hint_ = enabled;
  return *this;
}
const std::vector<Register> &Function::arguments() const noexcept { return arguments_; }

Value Function::arg(std::string_view name) {
  if (error()) return poison_value(Type::i64());
  const Register *reg = find_argument(name);
  if (reg == nullptr) {
    fail(Error{ErrorCode::UnknownName, "unknown function argument"});
    return poison_value(Type::i64());
  }
  Register reg_value = *reg;
  return Value(*this, reg_value.type(), Operand(reg_value));
}

const Register *Function::find_argument(std::string_view name) const noexcept {
  for (const Register &reg : arguments_) {
    if (reg.name() == name) return &reg;
  }
  return nullptr;
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

Label &Function::label(std::string_view name, LabelOptions options) {
  return create_child_label(name, options);
}

void Function::end() {}

std::size_t Function::instruction_count() const noexcept { return instructions().size(); }

const std::vector<std::unique_ptr<Instruction>> &Function::instructions() const noexcept {
  if (flattened_dirty_) {
    flattened_instructions_.clear();
    flatten_into(flattened_instructions_, *this, false);
    flattened_dirty_ = false;
  }
  return flattened_instructions_;
}

Result<Register> Function::create_register_internal(Type type, std::string_view name, bool argument) {
  if (error()) return Error{ErrorCode::InvalidOperand, "module is already in error state"};
  if (!argument && !is_local()) {
    return Error{ErrorCode::InvalidOperand, "only local functions can create local registers"};
  }
  if (name.empty()) return Error{ErrorCode::InvalidArgument, "register name must not be empty"};
  if (has_register_name(name)) return Error{ErrorCode::DuplicateName, "duplicate register name"};

  std::vector<Register> &registers = argument ? arguments_ : local_registers_;
  registers.push_back(Register(this, next_register_id_++, type, std::string(name)));
  return registers.back();
}

bool Function::has_register_name(std::string_view name) const noexcept {
  return find_register(name) != nullptr;
}

void Function::invalidate_flattened() const noexcept { flattened_dirty_ = true; }

void Function::flatten_into(std::vector<std::unique_ptr<Instruction>> &out, const Block &block,
                            bool emit_label) const {
  auto is_terminator = [](Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Jmp:
    case Opcode::JmpIndirect:
    case Opcode::Bt:
    case Opcode::Bts:
    case Opcode::Bf:
    case Opcode::Bfs:
    case Opcode::Beq:
    case Opcode::Beqs:
    case Opcode::FBeq:
    case Opcode::DBeq:
    case Opcode::LDBeq:
    case Opcode::Bne:
    case Opcode::Bnes:
    case Opcode::FBne:
    case Opcode::DBne:
    case Opcode::LDBne:
    case Opcode::Blt:
    case Opcode::Blts:
    case Opcode::UBlt:
    case Opcode::UBlts:
    case Opcode::FBlt:
    case Opcode::DBlt:
    case Opcode::LDBlt:
    case Opcode::Ble:
    case Opcode::Bles:
    case Opcode::UBle:
    case Opcode::UBles:
    case Opcode::FBle:
    case Opcode::DBle:
    case Opcode::LDBle:
    case Opcode::Bgt:
    case Opcode::Bgts:
    case Opcode::UBgt:
    case Opcode::UBgts:
    case Opcode::FBgt:
    case Opcode::DBgt:
    case Opcode::LDBgt:
    case Opcode::Bge:
    case Opcode::Bges:
    case Opcode::UBge:
    case Opcode::UBges:
    case Opcode::FBge:
    case Opcode::DBge:
    case Opcode::LDBge:
    case Opcode::Bo:
    case Opcode::UBo:
    case Opcode::Bno:
    case Opcode::UBno:
    case Opcode::Switch:
    case Opcode::Ret:
    case Opcode::JRet:
      return true;
    default:
      return false;
    }
  };
  const Label *scoped_label = nullptr;
  if (emit_label) {
    const Label &label = static_cast<const Label &>(block);
    auto instruction = std::make_unique<Instruction>(Opcode::Label);
    instruction->set_label_id(label.id());
    out.push_back(std::move(instruction));
    if (label.is_stack_scoped() && label.stack_scope_token_) {
      scoped_label = &label;
      out.push_back(std::make_unique<Instruction>(
          Opcode::BStart, std::vector<Operand>{Operand(*label.stack_scope_token_)}));
    }
  }
  bool emitted_any_bend = false;
  auto emit_bend = [&out, &emitted_any_bend, scoped_label]() {
    if (scoped_label == nullptr) return;
    out.push_back(std::make_unique<Instruction>(
        Opcode::BEnd, std::vector<Operand>{Operand(*scoped_label->stack_scope_token_)}));
    emitted_any_bend = true;
  };
  for (const std::unique_ptr<Instruction> &instruction : block.operations_) {
    if (is_terminator(instruction->opcode())) emit_bend();
    out.push_back(std::make_unique<Instruction>(*instruction));
    if (instruction->opcode() == Opcode::Label) {
      const std::size_t label_id = instruction->label_id();
      for (const std::unique_ptr<Label> &label : block.child_labels_) {
        if (label->id() == label_id) {
          flatten_into(out, *label, false);
          break;
        }
      }
    }
  }
  if (!emitted_any_bend) emit_bend();
  for (const std::unique_ptr<Label> &label : block.child_labels_) {
    if (label->appended_as_instruction_) continue;
    flatten_into(out, *label, true);
  }
}

Module::Module(std::string name) : Block(Kind::Module, nullptr), name_(std::move(name)) {
  set_module(this);
}

std::string_view Module::name() const noexcept { return name_; }
const std::optional<Error> &Module::error() const noexcept { return error_; }

void Module::fail(Error error) {
  if (!error_) error_ = std::move(error);
}

Function &Module::new_function(std::string_view name) {
  functions_.push_back(std::make_unique<Function>(*this, std::string(name)));
  return *functions_.back();
}

Function &Module::new_function(std::string_view name, std::vector<Type> return_types,
                               std::vector<Function::Parameter> parameters,
                               FunctionOptions options) {
  Function::Linkage linkage = Function::Linkage::Local;
  switch (options.linkage) {
  case FunctionLinkage::Local:
    linkage = Function::Linkage::Local;
    break;
  case FunctionLinkage::Import:
    linkage = Function::Linkage::Import;
    break;
  case FunctionLinkage::Signature:
    linkage = Function::Linkage::Signature;
    break;
  }
  functions_.push_back(std::make_unique<Function>(*this, std::string(name),
                                                  std::move(return_types),
                                                  std::move(parameters), linkage,
                                                  options.vararg));
  functions_.back()->set_inline(options.inline_hint);
  return *functions_.back();
}

Function &Module::new_vararg_function(std::string_view name, std::vector<Type> return_types,
                                      std::vector<Function::Parameter> parameters) {
  return new_function(name, std::move(return_types), std::move(parameters),
                      FunctionOptions{FunctionLinkage::Local, true});
}

Result<Value> Module::ref(const Function &function) {
  return Value(*this, Type::p(), Operand::ref(function));
}

Result<Value> Module::ref(const Data &data) {
  return Value(*this, Type::p(), Operand::ref(data));
}

const std::vector<std::unique_ptr<Data>> &Module::data_items() const noexcept {
  return data_items_;
}

const std::vector<std::unique_ptr<Function>> &Module::functions() const noexcept {
  return functions_;
}

Module &Context::new_module(std::string_view name) {
  modules_.push_back(std::make_unique<Module>(std::string(name)));
  return *modules_.back();
}

const std::vector<std::unique_ptr<Module>> &Context::modules() const noexcept { return modules_; }

} // namespace mirnext
