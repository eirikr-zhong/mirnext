#include "mirnext/IR/Vector.hpp"
/* Licensed under the MIT License. */
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/IR.hpp"
#include "mirnext/IR/SSA.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace mirnext::ir {
Function::Function (Module &module, Symbol &symbol) : module_ (&module), symbol_ (&symbol) {}
Function::~Function () = default;
Result<void> Function::require_register_ir () const {
  if (ssa_)
    return Error{ErrorCode::InvalidArgument,
                 std::string (symbol ().name ()) + ": operation requires completed register IR"};
  return {};
}
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
bool name_valid (std::string_view name) { return !name.empty () && name.find ('\0') == name.npos; }
constexpr InstructionDescriptor descriptors[]{
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) \
  {Opcode::name, spelling, roles, OperandCount::count, ControlFlow::flow},
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
};
char type_role (Type type) {
  switch (type.kind ()) {
  case Type::Kind::F32: return 'f';
  case Type::Kind::F64: return 'd';
  case Type::Kind::LongDouble: return 'e';
  case Type::Kind::Vector: return 'v';
  default: return 'i';
  }
}
std::optional<Type> operand_type (const Operand &operand) {
  if (auto reg = detail::operand_access (operand).get<const Register *> ())
    return *reg ? std::optional ((*reg)->type ()) : std::nullopt;
  if (auto memory = detail::operand_access (operand).get<Memory> ()) return memory->type;
  if (auto constant = detail::operand_access (operand).get<VectorConstant> ())
    return constant->type;
  if (detail::operand_access (operand).get<float> ()) return Type (Type::Kind::F32);
  if (detail::operand_access (operand).get<double> ()) return Type (Type::Kind::F64);
  if (detail::operand_access (operand).get<long double> ()) return Type (Type::Kind::LongDouble);
  if (detail::operand_access (operand).get<std::int64_t> ()) return Type::i64 ();
  if (detail::operand_access (operand).get<std::uint64_t> ()) return Type (Type::Kind::U64);
  return {};
}
const Symbol *symbol_operand (const Operand &operand) {
  auto ref = detail::operand_access (operand).get<SymbolReference> ();
  return ref && ref->displacement == 0 ? detail::NodeAccess::symbol (*(ref)) : nullptr;
}
Result<void> check_role (const Function &function, const Operand &operand, char role) {
  const bool output = role >= 'A' && role <= 'Z';
  role = static_cast<char> (std::tolower (static_cast<unsigned char> (role)));
  if (auto label = detail::operand_access (operand).get<const Label *> ()) {
    if (!*label || &(*label)->function () != &function)
      return invalid ("label belongs to another function");
    return role == 'l' ? Result<void>{} : invalid ("unexpected label operand");
  }
  if (role == 'l') return invalid ("expected a label");
  char actual = '?';
  bool writable = false;
  if (auto reg = detail::operand_access (operand).get<const Register *> ()) {
    if (!*reg || &(*reg)->function () != &function)
      return invalid ("register belongs to another function");
    actual = type_role ((*reg)->type ());
    writable = true;
  } else if (auto memory = detail::operand_access (operand).get<Memory> ()) {
    if (memory->location ().get () && &memory->location ().get ()->function () != &function)
      return invalid ("memory location belongs to another function");
    if (!memory->type.valid () || memory->scale > 255
        || memory->alias.find ('\0') != std::string::npos
        || memory->nonalias.find ('\0') != std::string::npos)
      return invalid ("invalid memory operand");
    for (auto reg : {detail::NodeAccess::base (*(memory)), detail::NodeAccess::index (*(memory))})
      if (reg
          && (&reg->function () != &function || reg->is_memory_backed ()
              || (!reg->type ().is_integer () && !reg->type ().is_block ())))
        return invalid ("invalid address register or function owner");
    if (auto source = memory->literal_source ().get ()) {
      auto data = std::get_if<InitializedData> (&source->initializer ());
      if (&source->symbol ().module () != &function.module () || !source->is_literal_pool ()
          || source->writable () || !data || !data->type.is_float () || data->type != memory->type
          || data->values.size () != 1 || !detail::NodeAccess::base (*(memory))
          || detail::NodeAccess::index (*(memory)) || memory->displacement || output)
        return invalid ("invalid literal pool memory reference");
    }
    if (output && memory->type.is_block ())
      return invalid ("block memory cannot be a scalar result");
    actual = type_role (memory->type);
    writable = true;
  } else if (auto ref = detail::operand_access (operand).get<SymbolReference> ()) {
    if (!detail::NodeAccess::symbol (*(ref))
        || &detail::NodeAccess::symbol (*(ref))->module () != &function.module ())
      return invalid ("symbol belongs to another module; use an import");
    if (detail::NodeAccess::symbol (*(ref))->kind () == Symbol::Kind::Prototype
        || detail::NodeAccess::symbol (*(ref))->name ().empty ())
      return invalid ("symbol has no addressable name");
    actual = 'i';
  } else if (detail::operand_access (operand).get<std::int64_t> ()
             || detail::operand_access (operand).get<std::uint64_t> ()
             || detail::operand_access (operand).get<std::string> ())
    actual = 'i';
  else if (detail::operand_access (operand).get<float> ())
    actual = 'f';
  else if (detail::operand_access (operand).get<double> ())
    actual = 'd';
  else if (detail::operand_access (operand).get<long double> ())
    actual = 'e';
  else if (auto vector = detail::operand_access (operand).get<VectorConstant> ()) {
    if (!vector->type.valid () || !vector->type.is_vector ())
      return invalid ("invalid vector constant type");
    actual = 'v';
  }
  if (output && !writable) return invalid ("result must be a register or memory");
  if (role == 'r' && !detail::operand_access (operand).get<const Register *> ())
    return invalid ("expected a register");
  const bool same_float_representation
    = sizeof (long double) == sizeof (double)
      && ((role == 'd' && actual == 'e') || (role == 'e' && actual == 'd'));
  if (role != 'r' && role != 'a' && role != actual && !same_float_representation)
    return invalid ("operand type does not match instruction");
  return {};
}
bool signature_equal (const Signature &lhs, const Signature &rhs) { return lhs == rhs; }
bool compatible_binding (Type lhs, Type rhs) {
  // The MIR register representation merges all integer widths into one scalar class.
  return lhs == rhs || (lhs.is_integer () && rhs.is_integer ());
}
bool uses_jump_return (const Function &function) {
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ())
      if (instruction.opcode () == Opcode::JRet) return true;
  return false;
}
Result<void> check_call_target (const Instruction &call, const Symbol &target, bool jump_return) {
  if (target.kind () != Symbol::Kind::Function || !target.function ().get ())
    return invalid ("call target is not a function definition");
  auto signature = symbol_operand (call.operands ()[0])->signature ().get ();
  if (*signature != target.function ().get ()->signature ())
    return invalid ("call signature does not match function definition");
  if ((call.opcode () == Opcode::JCall) != jump_return)
    return invalid ("call convention does not match function definition");
  return {};
}
Result<void> check_data_definition (const Module &module, const DataInitializer &initializer) {
  auto valid = validate_initializer (module, initializer);
  if (!(valid).has_value ()) return valid;
  // Adding an external entry changes PHI predecessors. Like public Builder
  // edits, it requires register IR; existing label data remains valid in SSA.
  if (auto ref = std::get_if<LabelReference> (&initializer))
    return detail::NodeAccess::label (*(ref))->function ().require_register_ir ();
  return {};
}
}  // namespace

bool Type::valid () const noexcept {
  if (kind_ == Kind::Vector) {
    const auto element_bits = [this] {
      switch (vector_element_) {
      case VectorElement::I8:
      case VectorElement::U8: return 8u;
      case VectorElement::I16:
      case VectorElement::U16: return 16u;
      case VectorElement::I32:
      case VectorElement::U32:
      case VectorElement::F32: return 32u;
      case VectorElement::I64:
      case VectorElement::U64:
      case VectorElement::F64: return 64u;
      }
      return 0u;
    }();
    return element_bits != 0 && vector_bits_ == 128 && vector_lanes_ != 0
           && vector_lanes_ == vector_bits_ / element_bits;
  }
  return static_cast<unsigned> (kind_) <= static_cast<unsigned> (Kind::Undef)
         && (!is_block ()
             || (size_ <= static_cast<std::size_t> (INT64_MAX)
                 && static_cast<unsigned> (class_) <= static_cast<unsigned> (BlockClass::Class4)));
}
Result<void> Signature::verify () const {
  for (auto type : results)
    if (!type.valid () || (!type.is_scalar () && !type.is_vector ()))
      return invalid ("function result must be scalar or vector");
  for (std::size_t n = 0; n < parameters.size (); ++n) {
    auto type = parameters[n];
    if (!type.valid () || type.kind () == Type::Kind::Undef)
      return invalid ("invalid parameter type");
    if (type.is_block () && type.block_size () == 0)
      return invalid ("block parameter must have a nonzero size");
    if (type.kind () == Type::Kind::ReturnBlock && n != 0)
      return invalid ("return block must be the first parameter");
  }
  if (!results.empty () && !parameters.empty () && parameters[0].kind () == Type::Kind::ReturnBlock)
    return invalid ("return block cannot accompany scalar results");
  return {};
}
std::span<const InstructionDescriptor> instruction_descriptors () noexcept { return descriptors; }
OptionalRef<const InstructionDescriptor> instruction_descriptor (Opcode opcode) noexcept {
  auto index = static_cast<std::size_t> (opcode);
  return index < std::size (descriptors)
           ? OptionalRef<const InstructionDescriptor> (descriptors[index])
           : OptionalRef<const InstructionDescriptor>{};
}
Register::Register (Function &function, Type type, std::string_view name, std::string_view hardware,
                    bool argument)
  : function_ (&function),
    type_ (type),
    name_ (function.module ().program ().intern_string (name)),
    hardware_ (function.module ().program ().intern_string (hardware)),
    argument_ (argument) {}
Label::Label (Function &function, BasicBlock &block, std::string_view name)
  : function_ (&function),
    block_ (&block),
    name_ (function.module ().program ().intern_string (name)) {}
BasicBlock::BasicBlock (Function &function, std::string_view name)
  : function_ (&function), name_ (function.module ().program ().intern_string (name)) {}
Module::Module (Program &context, std::string_view name)
  : storage_ (context.storage_.get ()), name_ (context.intern_string (name)) {}
Program &Module::program () { return *storage_->owner; }
const Program &Module::program () const { return *storage_->owner; }
Symbol::Symbol (Module &module, std::string_view name, Kind kind)
  : module_ (&module), name_ (module.program ().intern_string (name)), kind_ (kind) {}
const Function &Instruction::function () const { return block_->function (); }
OptionalRef<const Instruction> BasicBlock::terminator () const {
  if (instructions_.empty ()) return {};
  auto &last = instructions_.back ();
  auto descriptor = instruction_descriptor (last.opcode ()).get ();
  return ::mirnext::detail::borrow_optional (
    descriptor && descriptor->flow != ControlFlow::Next ? &last : nullptr);
}
const Signature &Function::signature () const { return *symbol_->signature ().get (); }
ReferenceRange<const GlobalData> Function::label_data_uses () const {
  auto found = module_->function_data_uses_.find (this);
  return found == module_->function_data_uses_.end ()
           ? ReferenceRange<const GlobalData>{}
           : ReferenceRange<const GlobalData> (found->second.labels);
}
ReferenceRange<const GlobalData> Function::expression_data_uses () const {
  auto found = module_->function_data_uses_.find (this);
  return found == module_->function_data_uses_.end ()
           ? ReferenceRange<const GlobalData>{}
           : ReferenceRange<const GlobalData> (found->second.expressions);
}
ReferenceRange<const GlobalData> Symbol::reference_data_uses () const {
  auto found = module_->symbol_data_uses_.find (this);
  return found == module_->symbol_data_uses_.end ()
           ? ReferenceRange<const GlobalData>{}
           : ReferenceRange<const GlobalData> (found->second);
}
Revisions Function::revisions () const {
  auto result = revisions_;
  result.module_data = module_->data_revision ();
  return result;
}
void Function::changed (bool cfg) {
  ++revisions_.instructions;
  ++module_->revisions_.instructions;
  if (cfg) {
    ++revisions_.control_flow;
    ++module_->revisions_.control_flow;
  }
}
void Function::registers_changed () {
  ++revisions_.registers;
  ++module_->revisions_.registers;
}
Result<BasicBlock &> Function::create_block (std::string_view name, const StackScope *scope) {
  auto phase = require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (name.find ('\0') != name.npos) return invalid ("block name contains NUL");
  if (scope && (&scope->function () != this || scope->materialized ()))
    return invalid ("invalid stack scope owner or materialized scope");
  if (!name.empty () && block_names_.contains (name))
    return Error{ErrorCode::DuplicateName, "duplicate block: " + std::string (name)};
  auto block = std::unique_ptr<BasicBlock> (new BasicBlock (*this, name));
  auto result = block.get ();
  result->scope_ = scope;
  blocks_.push_back (std::move (block));
  result->position_ = std::prev (blocks_.end ());
  if (!name.empty ()) block_names_.emplace (result->name (), result);
  changed (true);
  return *(result);
}
Result<BasicBlock &> Function::create_block_after (BasicBlock &after, std::string_view name,
                                                   const StackScope *scope) {
  if (&after.function () != this) return invalid ("block insertion crosses function ownership");
  auto created = create_block (name, scope);
  if (!(created).has_value ()) return created;
  blocks_.splice (std::next (after.position_), blocks_, created.value ().position_);
  return created;
}
Result<StackScope &> Function::create_stack_scope (const StackScope *parent,
                                                   std::string_view name) {
  auto phase = require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (parent && (&parent->function () != this || parent->materialized ()))
    return invalid ("invalid parent stack scope");
  if (name.find ('\0') != name.npos) return invalid ("stack scope name contains NUL");
  auto scope = std::unique_ptr<StackScope> (
    new StackScope (*this, parent, module ().program ().intern_string (name)));
  auto result = scope.get ();
  scopes_.push_back (std::move (scope));
  changed (true);
  return *(result);
}
Result<BasicBlock &> Function::create_block_before (BasicBlock &before, std::string_view name,
                                                    const StackScope *scope) {
  if (&before.function () != this) return invalid ("block insertion crosses function ownership");
  auto created = create_block (name, scope);
  if (!(created).has_value ()) return created;
  blocks_.splice (before.position_, blocks_, created.value ().position_);
  return created;
}
bool Function::needs_materialization () const {
  return std::any_of (scopes_.begin (), scopes_.end (),
                      [] (auto &scope) { return !scope->materialized (); });
}
Result<Label &> Function::create_label (BasicBlock &block, std::string_view name) {
  auto phase = require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (&block.function () != this) return invalid ("label block belongs to another function");
  if (name.find ('\0') != name.npos) return invalid ("label name contains NUL");
  if (!name.empty () && label_names_.contains (name))
    return Error{ErrorCode::DuplicateName, "duplicate label: " + std::string (name)};
  auto label = std::unique_ptr<Label> (new Label (*this, block, name));
  auto result = label.get ();
  labels_.push_back (std::move (label));
  block.labels_.push_back (result);
  if (!name.empty ()) label_names_.emplace (result->name (), result);
  changed (true);
  return *(result);
}
Result<Register &> Function::add_register (Type type, std::string_view name,
                                           std::string_view hardware, bool argument) {
  if (!type.valid ()
      || (!type.is_scalar () && !type.is_vector () && !(argument && type.is_block ())))
    return invalid ("local register must be scalar or vector");
  if (name.find ('\0') != name.npos || hardware.find ('\0') != hardware.npos)
    return invalid ("register name or binding contains NUL");
  if (!name.empty () && register_names_.contains (name))
    return Error{ErrorCode::DuplicateName, "duplicate register: " + std::string (name)};
  if (!hardware.empty ()) {
    if (argument || (!type.is_scalar () && !type.is_vector ()))
      return invalid ("invalid hardware binding");
    if (auto found = hardware_registers_.find (hardware); found != hardware_registers_.end ()) {
      if (!compatible_binding (found->second->type (), type))
        return invalid ("incompatible hardware binding types");
      if (!name.empty ()) {
        register_names_.emplace (module_->program ().intern_string (name), found->second);
        registers_changed ();
      }
      return *(found->second);
    }
  }
  auto reg = std::unique_ptr<Register> (new Register (*this, type, name, hardware, argument));
  auto result = reg.get ();
  registers_.push_back (std::move (reg));
  if (!name.empty ()) register_names_.emplace (result->name (), result);
  if (!hardware.empty ()) hardware_registers_.emplace (result->hardware_binding (), result);
  if (argument) arguments_.push_back (result);
  registers_changed ();
  return *(result);
}
Result<Register &> Function::create_register (Type type, std::string_view name,
                                              std::string_view hardware) {
  auto phase = require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  return add_register (type, name, hardware, false);
}
OptionalRef<const Register> Function::find_register (std::string_view name) const {
  auto found = register_names_.find (name);
  return ::mirnext::detail::borrow_optional (found == register_names_.end () ? nullptr
                                                                             : found->second);
}
OptionalRef<const BasicBlock> Function::find_block (std::string_view name) const {
  auto found = block_names_.find (name);
  return ::mirnext::detail::borrow_optional (found == block_names_.end () ? nullptr
                                                                          : found->second);
}
OptionalRef<const Label> Function::find_label (std::string_view name) const {
  auto found = label_names_.find (name);
  return ::mirnext::detail::borrow_optional (found == label_names_.end () ? nullptr
                                                                          : found->second);
}
std::size_t Function::instruction_count () const {
  std::size_t count = 0;
  for (auto &block : blocks_) count += block->size ();
  return count;
}
Result<void> Function::verify () const {
  if (ssa_) {
    auto checked = ssa_->verify ();
    if (!(checked).has_value ()) return checked;
  }
  auto valid = signature ().verify ();
  if (!(valid).has_value ()) return valid;
  if (blocks_.empty ()) return invalid ("function definition has no blocks");
  std::unordered_set<std::string_view> bindings;
  for (auto &reg : registers_)
    if (!reg->hardware_binding ().empty () && !bindings.insert (reg->hardware_binding ()).second)
      return invalid ("duplicate hardware binding");
  const Instruction *flag_source = nullptr;
  bool ordinary_return = false, jump_return = false;
  const StackScope *previous_scope = nullptr;
  for (auto &block : blocks_) {
    if (block->stack_scope ().get ()
        && (&block->stack_scope ().get ()->function () != this
            || block->stack_scope ().get ()->materialized ()))
      return invalid ("invalid block stack scope");
    if (!detail::ReferenceAccess::pointers (block->labels ()).empty ()) flag_source = nullptr;
    if (block->stack_scope ().get () != previous_scope) flag_source = nullptr;
    previous_scope = block->stack_scope ().get ();
    bool terminated = false;
    for (auto &instruction : block->instructions_) {
      if (terminated) return invalid ("instruction follows a block terminator");
      auto checked = validate_instruction (*this, instruction.opcode (), instruction.operands ());
      if (!(checked).has_value ()) return checked;
      auto opcode = instruction.opcode ();
      if (opcode == Opcode::Ret) ordinary_return = true;
      if (opcode == Opcode::JRet) jump_return = true;
      if (ordinary_return && jump_return) return invalid ("function mixes ret and jret");
      if (opcode == Opcode::Bo || opcode == Opcode::Bno || opcode == Opcode::UBo
          || opcode == Opcode::UBno) {
        if (!flag_source) return invalid ("overflow branch has no preceding overflow instruction");
        auto source = flag_source->opcode ();
        switch (source) {
        case Opcode::Addo:
        case Opcode::Addos:
        case Opcode::Subo:
        case Opcode::Subos:
        case Opcode::Mulo:
        case Opcode::Mulos:
        case Opcode::UMulo:
        case Opcode::UMulos: break;
        default:
          return invalid (
            "overflow branch requires an overflow instruction separated only by register moves or "
            "stores");
        }
        bool unsigned_branch = opcode == Opcode::UBo || opcode == Opcode::UBno;
        if ((unsigned_branch && (source == Opcode::Mulo || source == Opcode::Mulos))
            || (!unsigned_branch && (source == Opcode::UMulo || source == Opcode::UMulos)))
          return invalid ("overflow branch has incompatible signedness");
      }
      if (opcode != Opcode::Mov
          || !detail::operand_access (instruction.operands ()[1]).get<const Register *> ())
        flag_source = &instruction;
      terminated = instruction_descriptor (instruction.opcode ()).get ()->flow != ControlFlow::Next;
    }
  }
  auto last = blocks_.back ()->terminator ().get ();
  if (!last && !blocks_.back ()->instructions_.empty ()
      && blocks_.back ()->instructions_.back ().opcode () == Opcode::JCall)
    return {};
  if (!last
      || (instruction_descriptor (last->opcode ()).get ()->flow != ControlFlow::Exit
          && last->opcode () != Opcode::Jmp && last->opcode () != Opcode::JmpIndirect
          && last->opcode () != Opcode::Switch))
    return invalid ("function falls through its final block");
  return {};
}

OptionalRef<const Symbol> Module::find_symbol (std::string_view name) const {
  auto found = names_.find (name);
  return ::mirnext::detail::borrow_optional (found == names_.end () ? nullptr : found->second);
}
Error Module::fail (Error error) {
  error.message = std::string (name ()) + ": " + error.message;
  if (!error_) error_ = std::move (error);
  return *error_;
}
Result<Symbol &> Module::symbol (std::string_view name, Symbol::Kind kind, bool allow_forward) {
  if (error_) return *error_;
  const bool anonymous = name.empty () && kind == Symbol::Kind::Data;
  if (!anonymous && !name_valid (name))
    return fail (invalid ("symbol name is empty or contains NUL"));
  if (auto found = names_.find (name); found != names_.end ()) {
    if (allow_forward && found->second->kind_ == Symbol::Kind::Forward) {
      if (found->second->exported () && kind != Symbol::Kind::Function
          && kind != Symbol::Kind::Data)
        return fail (invalid ("exported declaration must resolve to a function or data"));
      found->second->kind_ = kind;
      ++revisions_.symbols;
      return *(found->second);
    }
    return fail ({ErrorCode::DuplicateName, "duplicate symbol: " + std::string (name)});
  }
  auto value = std::unique_ptr<Symbol> (new Symbol (*this, name, kind));
  auto result = value.get ();
  symbols_.push_back (std::move (value));
  if (!anonymous) names_.emplace (result->name (), result);
  ++revisions_.symbols;
  return *(result);
}
Result<Symbol &> Module::forward (std::string_view name) {
  if (error_) return *error_;
  auto existing = names_.find (name);
  if (existing != names_.end ()
      && (existing->second->forward_declared_ || existing->second->kind () == Symbol::Kind::Import
          || existing->second->kind () == Symbol::Kind::Prototype))
    return fail ({ErrorCode::DuplicateName, "duplicate or invalid forward: " + std::string (name)});
  auto result = existing != names_.end () ? Result<Symbol &> (*(existing->second))
                                          : symbol (name, Symbol::Kind::Forward, false);
  if (!(result).has_value ()) return result;
  result.value ().forward_declared_ = true;
  items_.emplace_back (detail::NodeAccess::make_ForwardDeclaration (&(result).value ()));
  ++revisions_.symbols;
  return result;
}
Result<Symbol &> Module::reserve_symbol (std::string_view name) {
  if (error_) return *error_;
  if (auto found = names_.find (name); found != names_.end ()) return *(found->second);
  return symbol (name, Symbol::Kind::Forward, false);
}
Result<Symbol &> Module::declare_function (std::string_view name, Signature signature) {
  auto type = program ().intern_signature (std::move (signature));
  if (!(type).has_value ()) return fail (type.error ());
  auto result = symbol (name, Symbol::Kind::Function, true);
  if (!(result).has_value ()) return result;
  result.value ().signature_ = &(type).value ();
  items_.emplace_back (detail::NodeAccess::make_ForwardDeclaration (&(result).value ()));
  return result;
}
Result<Function &> Module::define_function (Symbol &symbol,
                                            std::span<const std::string_view> names) {
  if (error_) return *error_;
  if (&symbol.module () != this || symbol.kind () != Symbol::Kind::Function
      || !symbol.signature ().get () || symbol.function ().get ())
    return fail (invalid ("invalid or already defined function declaration"));
  if (symbol.signature ().get ()->variadic && symbol.signature ().get ()->parameters.empty ())
    return fail (invalid ("variadic function needs a fixed parameter"));
  if (!names.empty () && names.size () != symbol.signature ().get ()->parameters.size ())
    return fail (invalid ("argument name count does not match signature"));
  std::unordered_set<std::string_view> unique;
  for (auto name : names)
    if (!name_valid (name) || !unique.insert (name).second)
      return fail (invalid ("invalid or duplicate argument name"));
  auto function = std::unique_ptr<Function> (new Function (*this, symbol));
  for (std::size_t n = 0; n < symbol.signature ().get ()->parameters.size (); ++n) {
    auto result = function->add_register (symbol.signature ().get ()->parameters[n],
                                          names.empty () ? std::string_view{} : names[n], {}, true);
    if (!(result).has_value ()) return fail (result.error ());
  }
  auto result = function.get ();
  functions_.push_back (std::move (function));
  symbol.function_ = result;
  ++revisions_.instructions;
  ++revisions_.control_flow;
  ++revisions_.registers;
  ++revisions_.symbols;
  items_.emplace_back (*result);
  return *(result);
}
Result<Function &> Module::create_function (std::string_view name, Signature signature,
                                            std::span<const std::string_view> names) {
  auto valid = signature.verify ();
  if (!(valid).has_value ()) return fail (valid.error ());
  if (signature.variadic && signature.parameters.empty ())
    return fail (invalid ("variadic function needs a fixed parameter"));
  // Validate before publishing the symbol, so an invalid definition is atomic.
  if (!names.empty () && names.size () != signature.parameters.size ())
    return fail (invalid ("argument name count does not match signature"));
  std::unordered_set<std::string_view> unique;
  for (auto name : names)
    if (!name_valid (name) || !unique.insert (name).second)
      return fail (invalid ("invalid or duplicate argument name"));
  auto type = program ().intern_signature (std::move (signature));
  if (!(type).has_value ()) return fail (type.error ());
  auto value = symbol (name, Symbol::Kind::Function, true);
  if (!(value).has_value ()) return value.error ();
  value.value ().signature_ = &(type).value ();
  return define_function ((value).value (), names);
}
Result<Symbol &> Module::prototype (std::string_view name, Signature signature) {
  auto type = program ().intern_signature (std::move (signature));
  if (!(type).has_value ()) return fail (type.error ());
  auto result = symbol (name, Symbol::Kind::Prototype, true);
  if (!(result).has_value ()) return result;
  result.value ().signature_ = &(type).value ();
  items_.emplace_back ((result).value ());
  return result;
}
Result<Symbol &> Module::import (std::string_view name, std::optional<Signature> signature) {
  const Signature *type = nullptr;
  if (signature) {
    auto checked = program ().intern_signature (std::move (*signature));
    if (!(checked).has_value ()) return fail (checked.error ());
    type = &(checked).value ();
  }
  auto result = symbol (name, Symbol::Kind::Import, true);
  if (!(result).has_value ()) return result;
  result.value ().signature_ = type;
  items_.emplace_back ((result).value ());
  return result;
}
Result<GlobalData &> Module::create_data (std::string_view name, DataInitializer initializer,
                                          bool writable) {
  auto valid = check_data_definition (*this, initializer);
  if (!(valid).has_value ()) return fail (valid.error ());
  auto declaration = declare_data (name);
  if (!(declaration).has_value ()) return declaration.error ();
  return define_data ((declaration).value (), std::move (initializer), writable);
}
Result<GlobalData &> Module::create_literal (Operand literal) {
  Type type = detail::operand_access (literal).get<float> ()         ? Type (Type::Kind::F32)
              : detail::operand_access (literal).get<double> ()      ? Type (Type::Kind::F64)
              : detail::operand_access (literal).get<long double> () ? Type (Type::Kind::LongDouble)
                                                                     : Type (Type::Kind::U8);
  std::vector<Operand> values;
  if (auto text = detail::operand_access (literal).get<std::string> ()) {
    values.reserve (text->size () + 1);
    for (unsigned char byte : *text) values.emplace_back (static_cast<std::int64_t> (byte));
    if (text->empty () || text->back () != '\0') values.emplace_back (0);
  } else if (type.is_float ())
    values.push_back (std::move (literal));
  else
    return invalid ("literal pool requires a floating value or string");
  std::string name;
  do {
    name = ".literal" + std::to_string (next_literal_++);
  } while (find_symbol (name).get ());
  auto created = create_data (name, InitializedData{type, std::move (values)}, false);
  if (!(created).has_value ()) return created.error ();
  created.value ().literal_pool_ = true;
  return created;
}
Result<Symbol &> Module::declare_data (std::string_view name) {
  auto value = symbol (name, Symbol::Kind::Data, true);
  if (!(value).has_value ()) return value;
  value.value ().data_slot_ = items_.size ();
  items_.emplace_back (detail::NodeAccess::make_DataDeclaration (&(value).value ()));
  return value;
}
Result<GlobalData &> Module::define_data (Symbol &symbol, DataInitializer initializer,
                                          bool writable) {
  if (error_) return *error_;
  if (&symbol.module () != this || symbol.kind () != Symbol::Kind::Data || symbol.data ().get ()
      || !symbol.data_slot_)
    return fail (invalid ("invalid or already defined data declaration"));
  auto valid = check_data_definition (*this, initializer);
  if (!(valid).has_value ()) return fail (valid.error ());
  auto data
    = std::unique_ptr<GlobalData> (new GlobalData (symbol, std::move (initializer), writable));
  auto result = data.get ();
  data_.push_back (std::move (data));
  symbol.data_ = result;
  items_[*symbol.data_slot_] = ModuleItem (*result);
  symbol.data_slot_.reset ();
  if (auto ref = std::get_if<SymbolReference> (&result->initializer ()))
    symbol_data_uses_[detail::NodeAccess::symbol (*(ref))].push_back (result);
  else if (auto ref = std::get_if<LabelReference> (&result->initializer ())) {
    // Label data adds externally reachable entries to this function alone.
    // Publish that topology dependency without pretending its instructions changed.
    auto &function = const_cast<Function &> (detail::NodeAccess::label (*(ref))->function ());
    function_data_uses_[&function].labels.push_back (result);
    ++function.revisions_.control_flow;
    ++revisions_.control_flow;
  } else if (auto ref = std::get_if<ExpressionData> (&result->initializer ()))
    function_data_uses_[detail::NodeAccess::function (*(ref))].expressions.push_back (result);
  ++data_revision_;
  ++revisions_.symbols;
  return *(result);
}
Result<void> Module::export_symbol (const Symbol &symbol) {
  if (error_) return *error_;
  if (&symbol.module () != this || symbol.name ().empty ()
      || symbol.kind () == Symbol::Kind::Prototype || symbol.kind () == Symbol::Kind::Import)
    return fail (invalid ("export requires a module definition or forward declaration"));
  if (symbol.exported ()) return fail ({ErrorCode::DuplicateName, "duplicate export"});
  const_cast<Symbol &> (symbol).exported_ = true;
  items_.emplace_back (detail::NodeAccess::make_ExportDeclaration (&symbol));
  ++revisions_.symbols;
  return {};
}
Result<void> Module::rename_symbol (const Symbol &symbol, std::string_view name) {
  if (&symbol.module () != this || !name_valid (name))
    return invalid ("invalid symbol rename owner or name");
  if (symbol.name () == name) return {};
  if (find_symbol (name).get ())
    return Error{ErrorCode::DuplicateName, "duplicate symbol: " + std::string (name)};
  auto interned = program ().intern_string (name);
  names_.erase (symbol.name ());
  const_cast<Symbol &> (symbol).name_ = interned;
  names_.emplace (interned, const_cast<Symbol *> (&symbol));
  ++revisions_.symbols;
  return {};
}
Result<void> Module::verify () const {
  if (error_) return *error_;
  for (auto &symbol : symbols_) {
    if (symbol->kind () == Symbol::Kind::Forward
        || (symbol->kind () == Symbol::Kind::Function && !symbol->function ().get ())
        || (symbol->kind () == Symbol::Kind::Data && !symbol->data ().get ()))
      return invalid ("unresolved declaration: " + std::string (symbol->name ()));
    if (auto variable = symbol->variable ().get ()) {
      auto checked = validate_variable_binding (*symbol, variable->type (), variable->writable ());
      if (!(checked).has_value ()) return checked;
    }
  }
  std::unordered_set<const Function *> jump_returns;
  for (auto &function : functions_) {
    auto valid = function->verify ();
    if (!(valid).has_value ())
      return Error{valid.error ().code, std::string (name ()) + "/"
                                          + std::string (function->symbol ().name ()) + ": "
                                          + valid.error ().message};
    if (uses_jump_return (*function)) jump_returns.insert (function.get ());
  }
  for (auto &function : functions_)
    for (auto &block : function->blocks ())
      for (auto &instruction : block.instructions ()) {
        if (instruction_descriptor (instruction.opcode ()).get ()->count != OperandCount::Call)
          continue;
        auto target = symbol_operand (instruction.operands ()[1]);
        if (target && target->kind () == Symbol::Kind::Function) {
          auto valid = check_call_target (instruction, *target,
                                          jump_returns.contains (target->function ().get ()));
          if (!(valid).has_value ())
            return Error{valid.error ().code, std::string (name ()) + "/"
                                                + std::string (function->symbol ().name ()) + ": "
                                                + valid.error ().message};
        }
      }
  for (auto &data : data_) {
    auto valid = validate_initializer (*this, data->initializer ());
    if (!(valid).has_value ()) return valid;
  }
  return {};
}
NodeRange<NodeIterator<std::vector<std::unique_ptr<Module>>::const_iterator, true>>
Program::modules () const {
  static const std::vector<std::unique_ptr<Module>> empty;
  return nodes (storage_ ? storage_->modules : empty);
}
NodeRange<NodeIterator<std::vector<std::unique_ptr<Module>>::iterator, false>> Program::modules () {
  if (storage_) return mutable_nodes (storage_->modules);
  using Iterator = NodeIterator<std::vector<std::unique_ptr<Module>>::iterator, false>;
  return NodeRange (Iterator{}, Iterator{});
}
Program::Program () : storage_ (std::make_unique<detail::ProgramStorage> ()) {
  storage_->owner = this;
}
Program::~Program () = default;
Program::Program (Program &&other) noexcept : storage_ (std::move (other.storage_)) {
  if (storage_) storage_->owner = this;
}
Program &Program::operator= (Program &&other) noexcept {
  if (this != &other) {
    storage_ = std::move (other.storage_);
    if (storage_) storage_->owner = this;
  }
  return *this;
}
Result<Module &> Program::create_module (std::string_view name) {
  if (!storage_) return invalid ("moved-from program");
  if (storage_->edit_error) return *storage_->edit_error;
  if (!name_valid (name)) return invalid ("invalid module name");
  if (find_module (name).get ())
    return Error{ErrorCode::DuplicateName, "duplicate module: " + std::string (name)};
  auto module = std::unique_ptr<Module> (new Module (*this, name));
  auto result = module.get ();
  storage_->modules.push_back (std::move (module));
  return *(result);
}
OptionalRef<const Module> Program::find_module (std::string_view name) const {
  if (!storage_) return {};
  for (auto &module : storage_->modules)
    if (module->name () == name) return ::mirnext::detail::borrow_optional (module.get ());
  return {};
}
const Type &Program::intern_type (Type type) { return *storage_->types.insert (type).first; }
Result<const Signature &> Program::intern_signature (Signature signature) {
  auto valid = signature.verify ();
  if (!(valid).has_value ()) return valid.error ();
  for (auto type : signature.results) intern_type (type);
  for (auto type : signature.parameters) intern_type (type);
  return *(&*storage_->signatures.insert (std::move (signature)).first);
}
std::string_view Program::intern_string (std::string_view value) {
  return *storage_->strings.emplace (value).first;
}
Result<void> Program::status () const { return analysis_status (false); }
Result<void> Program::analysis_status (bool allow_pending) const {
  if (!storage_) return invalid ("moved-from program");
  if (storage_->edit_error) return *storage_->edit_error;
  if (storage_->pending_edits && !allow_pending)
    return invalid ("program has an unfinished edit batch");
  return {};
}
Result<void> Program::verify () const {
  auto ready = status ();
  if (!(ready).has_value ()) return ready;
  StringMap<const Symbol *> exports;
  std::unordered_set<const Function *> jump_returns;
  for (auto &module : storage_->modules) {
    auto valid = module->verify ();
    if (!(valid).has_value ()) return valid;
    for (auto &symbol : module->symbols ())
      if (symbol.exported () && !exports.emplace (symbol.name (), &symbol).second)
        return Error{ErrorCode::DuplicateName,
                     "duplicate exported symbol: " + std::string (symbol.name ())};
    for (auto &function : module->functions ())
      if (uses_jump_return (function)) jump_returns.insert (&function);
  }
  for (auto &module : storage_->modules) {
    for (auto &symbol : module->symbols ())
      if (symbol.kind () == Symbol::Kind::Import && symbol.variable ().get ()) {
        auto found = exports.find (symbol.name ());
        if (found == exports.end ()) continue;
        auto target = found->second->variable ().get ();
        auto checked
          = validate_variable_binding (*found->second, symbol.variable ().get ()->type (),
                                       symbol.variable ().get ()->writable ());
        if (!(checked).has_value ()
            || (target
                && (target->type () != symbol.variable ().get ()->type ()
                    || (symbol.variable ().get ()->writable () && !target->writable ()))))
          return invalid (
            std::string (module->name ()) + "/" + std::string (symbol.name ())
            + ": imported variable type or write permission does not match definition");
      }
    for (auto &symbol : module->symbols ())
      if (symbol.kind () == Symbol::Kind::Import && symbol.signature ().get ()) {
        auto found = exports.find (symbol.name ());
        if (found == exports.end ()) continue;
        if (!found->second->signature ().get ()
            || *symbol.signature ().get () != *found->second->signature ().get ())
          return invalid (std::string (module->name ()) + "/" + std::string (symbol.name ())
                          + ": import signature does not match exported definition");
      }
    for (auto &function : module->functions ())
      for (auto &block : function.blocks ())
        for (auto &instruction : block.instructions ()) {
          if (instruction_descriptor (instruction.opcode ()).get ()->count != OperandCount::Call)
            continue;
          auto target = symbol_operand (instruction.operands ()[1]);
          if (!target || target->kind () != Symbol::Kind::Import) continue;
          auto found = exports.find (target->name ());
          if (found == exports.end ()) continue;
          auto valid
            = check_call_target (instruction, *found->second,
                                 jump_returns.contains (found->second->function ().get ()));
          if (!(valid).has_value ())
            return Error{valid.error ().code, std::string (module->name ()) + "/"
                                                + std::string (function.symbol ().name ())
                                                + ": cross-module " + valid.error ().message};
        }
  }
  return {};
}

Result<void> validate_initializer (const Module &module, const DataInitializer &initializer) {
  return std::visit (
    [&] (const auto &value) -> Result<void> {
      using T = std::decay_t<decltype (value)>;
      if constexpr (std::is_same_v<T, InitializedData>) {
        if (!value.type.valid () || (!value.type.is_scalar () && !value.type.is_vector ()))
          return invalid ("data element type must be scalar or vector");
        auto expected = type_role (value.type);
        for (auto &element : value.values) {
          bool valid
            = expected == 'v'
                ? detail::operand_access (element).template get<VectorConstant> ()
                    && detail::operand_access (element).template get<VectorConstant> ()->type
                         == value.type
              : expected == 'i'
                ? detail::operand_access (element).template get<std::int64_t> ()
                    || detail::operand_access (element).template get<std::uint64_t> ()
              : expected == 'f' ? detail::operand_access (element).template get<float> () != nullptr
              : expected == 'd'
                ? detail::operand_access (element).template get<double> () != nullptr
                : detail::operand_access (element).template get<long double> () != nullptr;
          if (!valid) return invalid ("data initializer has the wrong literal type");
        }
      } else if constexpr (std::is_same_v<T, SymbolReference>) {
        if (!detail::NodeAccess::symbol (value)
            || &detail::NodeAccess::symbol (value)->module () != &module
            || detail::NodeAccess::symbol (value)->name ().empty ()
            || detail::NodeAccess::symbol (value)->kind () == Symbol::Kind::Prototype)
          return invalid ("invalid reference data symbol");
      } else if constexpr (std::is_same_v<T, LabelReference>) {
        if (!detail::NodeAccess::label (value)
            || &detail::NodeAccess::label (value)->function ().module () != &module
            || (detail::NodeAccess::base (value)
                && &detail::NodeAccess::base (value)->function ()
                     != &detail::NodeAccess::label (value)->function ()))
          return invalid ("invalid label data owner");
      } else if constexpr (std::is_same_v<T, ExpressionData>) {
        if (!detail::NodeAccess::function (value)
            || &detail::NodeAccess::function (value)->module () != &module)
          return invalid ("invalid expression function owner");
        auto &signature = detail::NodeAccess::function (value)->signature ();
        if (signature.variadic || !signature.parameters.empty () || signature.results.size () != 1)
          return invalid ("expression initializer requires one result and no parameters");
        for (auto &block : detail::NodeAccess::function (value)->blocks ()) {
          std::unordered_map<const Register *, const GlobalData *> addresses;
          for (auto &instruction : block.instructions ()) {
            if (instruction_descriptor (instruction.opcode ()).value ().count == OperandCount::Call)
              return invalid ("expression initializer must not call functions");
            for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
              auto &operand = instruction.operands ()[n];
              if (auto memory = detail::operand_access (operand).template get<Memory> ()) {
                auto source = memory->literal_source ().get ();
                auto known = addresses.find (detail::NodeAccess::base (*(memory)));
                if (!source
                    || output_operand (*detail::NodeAccess::function (value), instruction, n)
                    || known == addresses.end () || known->second != source)
                  return invalid (
                    "expression initializer must not access memory except verified literal pools");
              }
            }
            for (std::size_t n = 0; n < instruction.operands ().size (); ++n)
              if (auto reg = detail::operand_access (instruction.operands ()[n])
                               .template get<const Register *> ();
                  reg && output_operand (*detail::NodeAccess::function (value), instruction, n))
                addresses.erase (*reg);
            if (instruction.opcode () == Opcode::Mov) {
              auto reg = detail::operand_access (instruction.operands ()[0])
                           .template get<const Register *> ();
              auto ref = detail::operand_access (instruction.operands ()[1])
                           .template get<SymbolReference> ();
              if (reg && ref && !ref->displacement && ref->symbol ().data ().has_value ()
                  && ref->symbol ().data ().value ().is_literal_pool ())
                addresses.insert_or_assign (*reg, ref->symbol ().data ().get ());
            }
          }
        }
      }
      return {};
    },
    initializer);
}

Result<void> validate_instruction (const Function &function, Opcode opcode,
                                   std::span<const Operand> operands) {
  auto descriptor = instruction_descriptor (opcode).get ();
  if (!descriptor || opcode == Opcode::Label)
    return invalid ("labels are block anchors, not executable instructions");
  const bool address = opcode == Opcode::Addr || opcode == Opcode::Addr8 || opcode == Opcode::Addr16
                       || opcode == Opcode::Addr32;
  const bool transfer = opcode == Opcode::Mov || opcode == Opcode::FMov || opcode == Opcode::DMov
                        || opcode == Opcode::LDMov || opcode == Opcode::VMov;
  for (std::size_t n = 0; n < operands.size (); ++n)
    if (auto reg = detail::operand_access (operands[n]).get<const Register *> ();
        reg && *reg && (*reg)->is_memory_backed () && !address && !transfer)
      return invalid ("memory-backed register requires an address and load/store");
  for (std::size_t n = 0; n < operands.size (); ++n)
    if (auto memory = detail::operand_access (operands[n]).get<Memory> ()) {
      const bool call = descriptor->count == OperandCount::Call;
      if (memory->type.is_block () && !call)
        return invalid ("block memory is only valid in call arguments");
      if (memory->type.kind () == Type::Kind::Undef
          && !((opcode == Opcode::VaStart && n == 0)
               || ((opcode == Opcode::VaArg || opcode == Opcode::VaBlockArg) && n == 1)
               || (opcode == Opcode::VaEnd && n == 0)))
        return invalid ("undef memory is only valid as a va_list address");
    }
  if (descriptor->count == OperandCount::Call) {
    if (operands.size () < 2) return invalid ("call requires a signature and callee");
    auto signature_symbol = symbol_operand (operands[0]);
    if (!signature_symbol || &signature_symbol->module () != &function.module ()
        || !signature_symbol->signature ().get ())
      return invalid ("call requires an owned signature symbol");
    auto &signature = *signature_symbol->signature ().get ();
    if (opcode == Opcode::JCall
        && (!signature.results.empty () || !signature.parameters.empty () || signature.variadic))
      return invalid ("jump call requires a void signature without parameters");
    auto fixed = 2 + signature.results.size () + signature.parameters.size ();
    if (operands.size () < fixed || (!signature.variadic && operands.size () != fixed))
      return invalid ("call operand count does not match signature");
    auto checked = check_role (function, operands[1], 'i');
    if (!(checked).has_value ()) return checked;
    if (auto memory = detail::operand_access (operands[1]).get<Memory> ();
        memory && memory->type.is_block ())
      return invalid ("call target cannot be block memory");
    if (auto target = symbol_operand (operands[1])) {
      if (target->variable ().get ()) return invalid ("variable symbol is not callable");
      if (target->kind () != Symbol::Kind::Function && target->kind () != Symbol::Kind::Import
          && target->kind () != Symbol::Kind::Forward)
        return invalid ("callee symbol is not callable");
      if (target->signature ().get () && !signature_equal (signature, *target->signature ().get ()))
        return invalid ("callee signature mismatch");
    }
    std::size_t n = 2;
    for (auto type : signature.results) {
      if (type.is_vector () && operand_type (operands[n]) != std::optional (type))
        return invalid ("call vector result type mismatch");
      checked
        = check_role (function, operands[n++], static_cast<char> (std::toupper (type_role (type))));
      if (!(checked).has_value ()) return checked;
    }
    for (auto type : signature.parameters) {
      auto &operand = operands[n++];
      if (type.is_vector () && operand_type (operand) != std::optional (type))
        return invalid ("call vector argument type mismatch");
      if (type.is_block ()) {
        auto memory = detail::operand_access (operand).get<Memory> ();
        if (!memory || memory->type != type)
          return invalid ("block argument size or ABI class mismatch");
        checked = check_role (function, operand, 'a');
      } else {
        if (auto memory = detail::operand_access (operand).get<Memory> ();
            memory && memory->type.is_block ())
          return invalid ("scalar parameter received a block argument");
        checked = check_role (function, operand, type_role (type));
      }
      if (!(checked).has_value ()) return checked;
    }
    for (; n < operands.size (); ++n) {
      checked = check_role (function, operands[n], 'a');
      if (!(checked).has_value ()) return checked;
      if (auto type = operand_type (operands[n]); type && type->is_vector ())
        return invalid ("v128 variadic arguments are not supported");
      auto reg = detail::operand_access (operands[n]).get<const Register *> ();
      auto memory = detail::operand_access (operands[n]).get<Memory> ();
      if (detail::operand_access (operands[n]).get<float> ()
          || (reg && (*reg)->type ().kind () == Type::Kind::F32)
          || (memory && memory->type.kind () == Type::Kind::F32))
        return invalid ("variadic float must be promoted to double");
      if (memory && memory->type.is_block ()
          && (memory->type.block_size () == 0 || memory->type.kind () == Type::Kind::ReturnBlock))
        return invalid ("invalid variadic block argument");
    }
    return {};
  }
  if (descriptor->count == OperandCount::Return) {
    if (operands.size () != function.signature ().results.size ())
      return invalid ("return count does not match signature");
    for (std::size_t n = 0; n < operands.size (); ++n) {
      auto type = function.signature ().results[n];
      if (type.is_vector () && operand_type (operands[n]) != std::optional (type))
        return invalid ("return vector type mismatch");
      auto checked = check_role (function, operands[n], type_role (type));
      if (!(checked).has_value ()) return checked;
    }
    return {};
  }
  if (descriptor->count == OperandCount::Switch) {
    if (operands.size () < 2) return invalid ("switch requires an index and targets");
    if (auto value = detail::operand_access (operands[0]).get<std::int64_t> ();
        value && (*value < 0 || static_cast<std::uint64_t> (*value) >= operands.size () - 1))
      return invalid ("constant switch index is out of range");
    if (auto value = detail::operand_access (operands[0]).get<std::uint64_t> ();
        value && *value >= operands.size () - 1)
      return invalid ("constant switch index is out of range");
    for (std::size_t n = 0; n < operands.size (); ++n) {
      auto checked = check_role (function, operands[n], n ? 'l' : 'i');
      if (!(checked).has_value ()) return checked;
    }
    return {};
  }
  if (operands.size () != descriptor->roles.size ())
    return invalid ("wrong operand count for " + std::string (descriptor->name));
  for (std::size_t n = 0; n < operands.size (); ++n) {
    auto checked = check_role (function, operands[n], descriptor->roles[n]);
    if (!(checked).has_value ()) return checked;
  }
  if (detail::vector_opcode (opcode)) {
    std::array<Type, 3> inputs{Type::i64 (), Type::i64 (), Type::i64 ()};
    for (unsigned n = 1; n < operands.size (); ++n)
      inputs[n - 1] = operand_type (operands[n]).value_or (Type::i64 ());
    auto destination = operand_type (operands[0]);
    if (!destination
        || !detail::vector_types (opcode, *destination,
                                  std::span (inputs).first (operands.size () - 1)))
      return invalid ("invalid SIMD128 operand/result types for " + std::string (descriptor->name));
    auto vector = opcode == Opcode::VSplat ? destination : operand_type (operands[1]);
    if (opcode == Opcode::VExtract || opcode == Opcode::VInsert) {
      auto &index = operands[opcode == Opcode::VExtract ? 2 : 3];
      auto signed_index = detail::operand_access (index).get<std::int64_t> ();
      auto unsigned_index = detail::operand_access (index).get<std::uint64_t> ();
      if ((!signed_index && !unsigned_index)
          || (signed_index
              && (*signed_index < 0 || std::uint64_t (*signed_index) >= vector->vector_lanes ()))
          || (unsigned_index && *unsigned_index >= vector->vector_lanes ()))
        return invalid ("vector lane index must be an in-range integer literal");
    }
    if (opcode == Opcode::VShuffle) {
      auto mask = detail::operand_access (operands[3]).get<VectorConstant> ();
      if (!mask || mask->type != Type::vector128 (Type::VectorElement::U8, 16))
        return invalid ("shuffle requires a constant u8x16 lane mask");
      for (unsigned n = 0; n < 16; ++n) {
        auto index = std::to_integer<unsigned> (mask->bytes[n]);
        if (n < vector->vector_lanes () ? index >= 2 * vector->vector_lanes () : index != 0)
          return invalid ("shuffle lane index out of range or nonzero unused mask lane");
      }
    }
  }
  if (address) {
    auto source = detail::operand_access (operands[1]).get<const Register *> ();
    if (source && !(*source)->hardware_binding ().empty ())
      return invalid (
        "hardware-bound register has no memory address; copy it to an ordinary local register "
        "first");
  }
  if (opcode == Opcode::VaStart && !function.signature ().variadic)
    return invalid ("va_start requires a variadic function");
  if (opcode == Opcode::VaArg && !detail::operand_access (operands[2]).get<Memory> ())
    return invalid ("va_arg requires a memory type operand");
  if (opcode == Opcode::VaArg
      && detail::operand_access (operands[2]).get<Memory> ()->type.is_vector ())
    return invalid ("v128 va_arg is not supported");
  if (opcode == Opcode::JRet
      && (!function.signature ().results.empty () || !function.signature ().parameters.empty ()
          || function.signature ().variadic))
    return invalid ("jump return requires a void function without parameters");
  if (opcode == Opcode::Alloca) {
    if (auto size = detail::operand_access (operands[1]).get<std::int64_t> (); size && *size < 0)
      return invalid ("negative allocation size");
  }
  if (opcode == Opcode::VaBlockArg) {
    if (auto size = detail::operand_access (operands[2]).get<std::int64_t> (); size && *size <= 0)
      return invalid ("invalid variadic block size");
    if (auto size = detail::operand_access (operands[2]).get<std::uint64_t> (); size && *size == 0)
      return invalid ("invalid variadic block size");
    if (auto abi = detail::operand_access (operands[3]).get<std::int64_t> ();
        abi && (*abi < 0 || *abi > 4))
      return invalid ("invalid variadic block class");
    if (auto abi = detail::operand_access (operands[3]).get<std::uint64_t> (); abi && *abi > 4)
      return invalid ("invalid variadic block class");
  }
  if (opcode == Opcode::PrSet || opcode == Opcode::PrBeq || opcode == Opcode::PrBne) {
    auto &location = operands[opcode == Opcode::PrSet ? 0 : 1];
    if (!detail::operand_access (location).get<const Register *> ()
        && !detail::operand_access (location).get<Memory> ())
      return invalid ("property location must be register or memory");
    if (!detail::operand_access (operands.back ()).get<std::int64_t> ())
      return invalid ("property must be an integer constant");
  }
  return {};
}
}  // namespace mirnext::ir
