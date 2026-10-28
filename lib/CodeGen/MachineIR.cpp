/* Licensed under the MIT License. Machine ownership and checked mutation. */
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/EditState.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/CodeGen/MachineIR.hpp"
#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mirnext::codegen {
namespace {
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", machine-ir]: " + std::move (why)};
}
bool call (MachineOpcode code) {
  auto p = std::get_if<ir::Opcode> (&code);
  return p && (*p == ir::Opcode::Call || *p == ir::Opcode::Inline || *p == ir::Opcode::JCall);
}
char role (ir::Type type) {
  using K = ir::Type::Kind;
  return type.kind () == K::F32          ? 'f'
         : type.kind () == K::F64        ? 'd'
         : type.kind () == K::LongDouble ? 'e'
                                         : 'i';
}
bool valid_abi_type (const MachineFunction &f, ir::Type type) {
  if (!type.valid () || type.kind () == ir::Type::Kind::Undef) return false;
  if (!type.is_block ()) return true;
  if (!type.block_size () || type.block_size () > static_cast<std::size_t> (INT64_MAX) - 15)
    return false;
  auto classification = static_cast<unsigned> (type.block_class ());
  return f.target ().architecture () != target::Architecture::X86_64 || !classification
         || (type.block_size () <= 16 && (classification < 3 || type.block_size () > 8));
}
}  // namespace
std::string_view opcode_name (MachineOpcode code) {
  if (auto op = std::get_if<ir::Opcode> (&code)) {
    auto descriptor = ir::instruction_descriptor (*op).get ();
    return descriptor ? descriptor->name : "invalid-opcode";
  }
  switch (std::get<MachinePseudoOpcode> (code)) {
  case MachinePseudoOpcode::Use: return "use";
  case MachinePseudoOpcode::X86Mov128: return "x86-mov128";
  case MachinePseudoOpcode::X86Mov128Unaligned: return "x86-mov128-unaligned";
  }
  return "invalid-pseudo";
}
const MachineMemoryLocation &MachineFunction::create_memory_location () {
  auto location = std::unique_ptr<MachineMemoryLocation> (
    new MachineMemoryLocation (*this, memory_locations_.size ()));
  memory_locations_.push_back (std::move (location));
  return *memory_locations_.back ();
}
MachineVirtualRegister::MachineVirtualRegister (MachineFunction &function, ir::Type declared,
                                                std::string name, bool argument, bool memory_backed,
                                                std::optional<target::PhysicalRegister> binding)
  : function_ (&function),
    type_ (function.target ().scalar_type (declared)),
    declared_ (declared),
    name_ (std::move (name)),
    argument_ (argument),
    memory_backed_ (memory_backed),
    binding_ (binding) {}
MachineInstruction::MachineInstruction (ConstructionKey, MachineBasicBlock &block,
                                        MachineOpcode code, std::vector<MachineOperand> operands)
  : block_ (&block), opcode_ (code), operands_ (std::move (operands)) {}
MachineFunction::MachineFunction (std::string module, std::string name, ir::Signature signature,
                                  target::TargetMachine target)
  : edit_state_ (std::make_unique<detail::MachineEditState> (detail::MachineEditState{this})),
    module_ (std::move (module)),
    name_ (std::move (name)),
    signature_ (std::move (signature)),
    target_ (std::move (target)) {
  if (target_.architecture () == target::Architecture::AArch64)
    state_.target = MachineFunctionState::AArch64{};
  else
    state_.target = MachineFunctionState::X86{};
}
MachineFunction::MachineFunction (MachineFunction &&other) noexcept
  : edit_state_ (std::move (other.edit_state_)),
    module_ (std::move (other.module_)),
    name_ (std::move (other.name_)),
    signature_ (std::move (other.signature_)),
    target_ (std::move (other.target_)),
    state_ (std::move (other.state_)),
    blocks_ (std::move (other.blocks_)),
    registers_ (std::move (other.registers_)),
    labels_ (std::move (other.labels_)),
    source_label_ordinals_ (std::move (other.source_label_ordinals_)),
    slots_ (std::move (other.slots_)),
    memory_locations_ (std::move (other.memory_locations_)),
    symbols_ (std::move (other.symbols_)),
    arguments_ (std::move (other.arguments_)),
    global_registers_ (std::move (other.global_registers_)),
    register_names_ (std::move (other.register_names_)),
    bindings_ (std::move (other.bindings_)),
    symbol_names_ (std::move (other.symbol_names_)),
    known_symbols_ (std::move (other.known_symbols_)),
    revision_ (std::move (other.revision_)),
    next_instruction_ (std::move (other.next_instruction_)) {
  if (edit_state_) edit_state_->owner = this;
  ++other.revision_;
  for (auto &block : blocks_) block->function_ = this;
  for (auto &reg : registers_) reg->function_ = this;
  for (auto &slot : slots_) slot->function_ = this;
  for (auto &location : memory_locations_) location->function_ = this;
}
MachineFunction &MachineFunction::operator= (MachineFunction &&other) noexcept {
  if (this == &other) return *this;
  edit_state_ = std::move (other.edit_state_);
  module_ = std::move (other.module_);
  name_ = std::move (other.name_);
  signature_ = std::move (other.signature_);
  target_ = std::move (other.target_);
  state_ = std::move (other.state_);
  blocks_ = std::move (other.blocks_);
  registers_ = std::move (other.registers_);
  labels_ = std::move (other.labels_);
  source_label_ordinals_ = std::move (other.source_label_ordinals_);
  slots_ = std::move (other.slots_);
  memory_locations_ = std::move (other.memory_locations_);
  symbols_ = std::move (other.symbols_);
  arguments_ = std::move (other.arguments_);
  global_registers_ = std::move (other.global_registers_);
  register_names_ = std::move (other.register_names_);
  bindings_ = std::move (other.bindings_);
  symbol_names_ = std::move (other.symbol_names_);
  known_symbols_ = std::move (other.known_symbols_);
  revision_ = std::move (other.revision_);
  next_instruction_ = std::move (other.next_instruction_);

  if (edit_state_) edit_state_->owner = this;
  ++other.revision_;
  for (auto &block : blocks_) block->function_ = this;
  for (auto &reg : registers_) reg->function_ = this;
  for (auto &slot : slots_) slot->function_ = this;
  for (auto &location : memory_locations_) location->function_ = this;
  return *this;
}
Result<MachineVirtualRegister &> MachineFunction::create_register (
  ir::Type type, std::string name, bool argument, bool memory_backed,
  std::optional<target::PhysicalRegister> binding) {
  if (!type.valid () || (!type.is_scalar () && !(argument && type.is_block ())))
    return invalid (*this, "invalid register type");
  if (!valid_abi_type (*this, type))
    return invalid (*this, "invalid block ABI size or classification");
  if (argument
      && (arguments_.size () >= signature_.parameters.size ()
          || type != signature_.parameters[arguments_.size ()]))
    return invalid (*this, "argument does not match signature");
  if (!name.empty () && register_names_.contains (name))
    return invalid (*this, "duplicate register name: " + name);
  if (binding) {
    if (memory_backed) return invalid (*this, "hardware-bound register cannot have memory storage");
    auto info = target_.register_info (*binding).get ();
    if (!info || info->reserved || !target_.accepts (*binding, type))
      return invalid (*this, "invalid hardware binding: " + name);
    if (auto found = bindings_.find (binding->encoding ()); found != bindings_.end ()) {
      auto r = found->second;
      if (argument || memory_backed != r->is_memory_backed ()
          || target_.scalar_type (type) != r->type ())
        return invalid (*this, "incompatible repeated hardware binding: " + name);
      return *(r);
    }
  }
  auto reg = std::unique_ptr<MachineVirtualRegister> (
    new MachineVirtualRegister (*this, type, std::move (name), argument, memory_backed, binding));
  auto ptr = reg.get ();
  registers_.push_back (std::move (reg));
  if (!ptr->name ().empty ()) register_names_.emplace (ptr->name (), ptr);
  if (binding) {
    bindings_.emplace (binding->encoding (), ptr);
    if (std::find (global_registers_.begin (), global_registers_.end (), *binding)
        == global_registers_.end ())
      global_registers_.push_back (*binding);
  }
  if (argument) arguments_.push_back (ptr);
  ++revision_;
  return *(ptr);
}
Result<MachineStackSlot &> MachineFunction::create_stack_slot (std::size_t size,
                                                               std::size_t alignment) {
  if (!size || !std::has_single_bit (alignment) || size > static_cast<std::size_t> (INT64_MAX))
    return invalid (*this, "invalid stack slot size or alignment");
  auto slot = std::unique_ptr<MachineStackSlot> (new MachineStackSlot (*this, size, alignment));
  auto ptr = slot.get ();
  slots_.push_back (std::move (slot));
  ++revision_;
  return *(ptr);
}
MachineBasicBlock &MachineFunction::create_block (std::string name) {
  blocks_.push_back (
    std::unique_ptr<MachineBasicBlock> (new MachineBasicBlock (*this, std::move (name))));
  blocks_.back ()->position_ = std::prev (blocks_.end ());
  ++revision_;
  return *blocks_.back ();
}
Result<MachineLabel &> MachineFunction::create_label (MachineBasicBlock &block, std::string name,
                                                      bool address_taken,
                                                      std::optional<std::size_t> source_ordinal,
                                                      bool function_entry) {
  if (&block.function () != this) return invalid (*this, "label block belongs to another function");
  if (!name.empty ())
    for (auto &l : labels_)
      if (l->name () == name) return invalid (*this, "duplicate label name: " + name);
  if (source_ordinal && source_label_ordinals_.contains (*source_ordinal))
    return invalid (*this, "duplicate source label identity");
  auto label = std::unique_ptr<MachineLabel> (
    new MachineLabel (block, std::move (name), address_taken, source_ordinal, function_entry));
  auto ptr = label.get ();
  labels_.push_back (std::move (label));
  block.labels_.push_back (ptr);
  if (source_ordinal) source_label_ordinals_.insert (*source_ordinal);
  ++revision_;
  return *(ptr);
}
Result<const MachineSymbol &> MachineFunction::intern_symbol (MachineSymbol symbol) {
  if (symbol.module.empty () || (symbol.name.empty () && !symbol.anonymous_ordinal)
      || static_cast<unsigned> (symbol.kind)
           > static_cast<unsigned> (MachineSymbol::Kind::BuiltinPrototype))
    return invalid (*this, "invalid symbol identity");
  if ((symbol.kind == MachineSymbol::Kind::BuiltinPrototype) != symbol.builtin.has_value ()
      || (symbol.builtin
          && static_cast<unsigned> (*symbol.builtin)
               > static_cast<unsigned> (Builtin::LongDoubleGe)))
    return invalid (*this, "invalid builtin prototype identity");
  if (symbol.signature) {
    auto valid = symbol.signature->verify ();
    if (!(valid).has_value ()) return invalid (*this, valid.error ().message);
  }
  if ((symbol.kind == MachineSymbol::Kind::Function || symbol.kind == MachineSymbol::Kind::Prototype
       || symbol.kind == MachineSymbol::Kind::BuiltinPrototype)
      && !symbol.signature)
    return invalid (*this, "function symbol requires a signature");
  auto found = symbol_names_.find (symbol.name);
  if (found != symbol_names_.end ())
    for (auto candidate : found->second) {
      if (*candidate == symbol) return *(candidate);
      if (candidate->module == symbol.module
          && candidate->anonymous_ordinal == symbol.anonymous_ordinal
          && candidate->builtin == symbol.builtin)
        return invalid (*this, "conflicting symbol identity: " + symbol.module + '/' + symbol.name);
    }
  symbols_.push_back (std::make_unique<MachineSymbol> (std::move (symbol)));
  auto &owned = *symbols_.back ();
  known_symbols_.insert (&owned);
  symbol_names_[owned.name].push_back (&owned);
  ++revision_;
  return owned;
}
std::size_t MachineFunction::instruction_count () const {
  std::size_t size = 0;
  for (auto &block : blocks_) size += block->instructions_.size ();
  return size;
}
Result<void> MachineRewriter::validate (MachineOpcode code,
                                        std::span<const MachineOperand> operands,
                                        bool lowered_call) const {
  auto reg_valid = [&] (const MachineRegister &reg, bool address) {
    if (auto v = std::get_if<const MachineVirtualRegister *> (detail::NodeAccess::storage (&reg)))
      return !function_.state ().operands_rewritten && *v && &(*v)->function () == &function_
             && (!address || (*v)->type ().is_integer ());
    auto physical = std::get<target::PhysicalRegister> (detail::NodeAccess::storage (reg));
    auto info = function_.target ().register_info (physical).get ();
    return info && (!address || info->bank == target::RegisterBank::Integer);
  };
  for (auto &o : operands) {
    if (auto reg = detail::operand_access (o).get<const MachineVirtualRegister *> ();
        reg && !reg_valid (detail::NodeAccess::make_MachineRegister (*reg), false))
      return invalid (function_, "foreign or null virtual register");
    if (auto reg = detail::operand_access (o).get<target::PhysicalRegister> ();
        reg && !reg_valid (*reg, false))
      return invalid (function_, "foreign physical register");
    if (auto label = detail::operand_access (o).get<const MachineLabel *> ();
        label && (!*label || &(*label)->block ().function () != &function_))
      return invalid (function_, "foreign or null label");
    if (auto symbol = detail::operand_access (o).get<MachineSymbolReference> ();
        symbol && !function_.known_symbols_.contains (detail::NodeAccess::symbol (*(symbol))))
      return invalid (function_, "foreign or null symbol relocation");
    if (auto slot = detail::operand_access (o).get<StackSlotReference> ();
        slot
        && (!detail::NodeAccess::slot (*(slot))
            || &detail::NodeAccess::slot (*(slot))->function () != &function_))
      return invalid (function_, "foreign or null stack slot");
    if (auto builtin = detail::operand_access (o).get<BuiltinReference> ();
        builtin
        && static_cast<unsigned> (builtin->builtin) > static_cast<unsigned> (Builtin::LongDoubleGe))
      return invalid (function_, "unknown builtin");
    if (auto memory = detail::operand_access (o).get<MachineMemory> ()) {
      if (detail::NodeAccess::location (*(memory))
          && &detail::NodeAccess::location (*(memory))->function () != &function_)
        return invalid (function_, "foreign memory location");
      if (!memory->type.valid () || (memory->base && !reg_valid (*memory->base, true))
          || (memory->index && !reg_valid (*memory->index, true))
          || (memory->index && memory->scale != 1 && memory->scale != 2 && memory->scale != 4
              && memory->scale != 8))
        return invalid (function_, "invalid memory address or type");
      if (memory->type.is_block () && !valid_abi_type (function_, memory->type))
        return invalid (function_, "invalid block memory ABI size or classification");
    }
  }
  auto matches = [&] (const MachineOperand &o, char r) {
    const bool output = r >= 'A' && r <= 'Z';
    if (output) r += 'a' - 'A';
    const bool reg = detail::operand_access (o).get<const MachineVirtualRegister *> ()
                     || detail::operand_access (o).get<target::PhysicalRegister> ();
    if (r == 'l') return detail::operand_access (o).get<const MachineLabel *> () != nullptr;
    if (r == 'r') return reg;
    if (output && !reg && !detail::operand_access (o).get<MachineMemory> ()
        && !detail::operand_access (o).get<StackSlotReference> ())
      return false;
    if (r == 'a') return true;
    if (auto v = detail::operand_access (o).get<const MachineVirtualRegister *> ())
      return role ((*v)->type ()) == r;
    if (auto v = detail::operand_access (o).get<target::PhysicalRegister> ())
      return (function_.target ().register_info (*v).get ()->bank == target::RegisterBank::Integer)
             == (r == 'i');
    if (auto m = detail::operand_access (o).get<MachineMemory> ())
      return m->type.is_scalar () && role (m->type) == r;
    if (detail::operand_access (o).get<StackSlotReference> ())
      return true;  // Its scalar access type is supplied by the instruction.
    if (detail::operand_access (o).get<const MachineLabel *> ()) return false;
    return detail::operand_access (o).get<float> ()         ? r == 'f'
           : detail::operand_access (o).get<double> ()      ? r == 'd'
           : detail::operand_access (o).get<long double> () ? r == 'e'
                                                            : r == 'i';
  };
  if (auto pseudo = std::get_if<MachinePseudoOpcode> (&code)) {
    if (*pseudo == MachinePseudoOpcode::Use) {
      for (auto &o : operands)
        if (!matches (o, 'r')) return invalid (function_, "USE requires registers");
    } else if (*pseudo == MachinePseudoOpcode::X86Mov128
               || *pseudo == MachinePseudoOpcode::X86Mov128Unaligned) {
      auto vector = [&] (const MachineOperand &o) {
        auto physical = detail::operand_access (o).get<target::PhysicalRegister> ();
        return physical
               && function_.target ().register_info (*physical).get ()->bank
                    == target::RegisterBank::Floating;
      };
      if (function_.target ().architecture () != target::Architecture::X86_64
          || operands.size () != 2
          || (!vector (operands[0]) && !detail::operand_access (operands[0]).get<MachineMemory> ()
              && !detail::operand_access (operands[0]).get<StackSlotReference> ())
          || (!vector (operands[1]) && !detail::operand_access (operands[1]).get<MachineMemory> ()
              && !detail::operand_access (operands[1]).get<StackSlotReference> ())
          || (!vector (operands[0]) && !vector (operands[1])))
        return invalid (function_, "invalid aligned SSE move");
    } else
      return invalid (function_, "unknown machine pseudo opcode");
    return {};
  }
  const auto opcode = std::get<ir::Opcode> (code);
  auto descriptor = ir::instruction_descriptor (opcode).get ();
  if (!descriptor || opcode == ir::Opcode::Label)
    return invalid (function_, "labels are block anchors, not instructions");
  if (descriptor->count == ir::OperandCount::Fixed) {
    if (operands.size () != descriptor->roles.size ())
      return invalid (function_, "wrong operand count for " + std::string (descriptor->name));
    for (std::size_t p = 0; p < operands.size (); ++p)
      if (!matches (operands[p], descriptor->roles[p]))
        return invalid (function_, "wrong operand type for " + std::string (descriptor->name));
    if (opcode == ir::Opcode::Addr || opcode == ir::Opcode::Addr8 || opcode == ir::Opcode::Addr16
        || opcode == ir::Opcode::Addr32)
      if (auto source = detail::operand_access (operands[1]).get<const MachineVirtualRegister *> ();
          source && (*source)->binding ())
        return invalid (function_,
                        "hardware-bound register has no memory address; copy it to an ordinary "
                        "local register first");
  } else if (descriptor->count == ir::OperandCount::Return) {
    if (operands.size () != function_.signature ().results.size ())
      return invalid (function_, "wrong return count");
    for (std::size_t p = 0; p < operands.size (); ++p)
      if (!matches (operands[p],
                    role (function_.target ().scalar_type (function_.signature ().results[p]))))
        return invalid (function_, "wrong return type");
  } else if (descriptor->count == ir::OperandCount::Switch) {
    if (operands.size () < 2 || !matches (operands[0], 'i'))
      return invalid (function_, "invalid switch selector or missing targets");
    for (std::size_t p = 1; p < operands.size (); ++p)
      if (!matches (operands[p], 'l')) return invalid (function_, "invalid switch target");
  } else {
    if (operands.size () < 2) return invalid (function_, "call requires signature and target");
    auto proto = detail::operand_access (operands[0]).get<MachineSymbolReference> ();
    if (!proto || proto->displacement || !detail::NodeAccess::symbol (*(proto))->signature)
      return invalid (function_, "call requires signature symbol");
    if (!matches (operands[1], 'i')) return invalid (function_, "call target must be an address");
    const auto &s = *detail::NodeAccess::symbol (*(proto))->signature;
    const auto fixed = 2 + s.results.size () + s.parameters.size ();
    auto signature = s.verify ();
    if (!(signature).has_value ()) return invalid (function_, signature.error ().message);
    for (auto type : s.parameters)
      if (!valid_abi_type (function_, type))
        return invalid (function_, "invalid prototype block ABI");
    if (operands.size () < fixed || (!s.variadic && operands.size () != fixed))
      return invalid (function_, "wrong call operand count");
    for (std::size_t p = 0; p < s.results.size (); ++p) {
      if (lowered_call
          && !detail::operand_access (operands[p + 2]).get<target::PhysicalRegister> ())
        return invalid (function_, "lowered result requires a physical register");
      if (!matches (operands[p + 2],
                    role (function_.target ().scalar_type (s.results[p])) + 'A' - 'a'))
        return invalid (function_, "invalid call result");
    }
    if (opcode == ir::Opcode::JCall
        && (!s.results.empty () || !s.parameters.empty () || s.variadic))
      return invalid (function_, "jump call requires an empty signature");
    for (std::size_t p = 2 + s.results.size (); p < operands.size (); ++p) {
      auto param = p - 2 - s.results.size ();
      auto memory = detail::operand_access (operands[p]).get<MachineMemory> ();
      if (lowered_call) {
        if (!detail::operand_access (operands[p]).get<target::PhysicalRegister> () && !memory)
          return invalid (function_,
                          "lowered argument requires a physical register or memory location");
        if (memory && memory->type.kind () != ir::Type::Kind::Undef && !memory->type.is_scalar ()
            && memory->type.kind () != ir::Type::Kind::ReturnBlock)
          return invalid (function_, "invalid lowered argument memory");
        if (param < s.parameters.size ()) {
          auto t = s.parameters[param];
          if (!t.is_block () && !matches (operands[p], role (function_.target ().scalar_type (t))))
            return invalid (function_, "invalid lowered scalar argument");
          if (!t.is_block () && memory && memory->type.kind () == ir::Type::Kind::Undef)
            return invalid (function_, "undefined memory cannot hold a scalar argument");
          if (t.is_block () && detail::operand_access (operands[p]).get<target::PhysicalRegister> ()
              && !matches (operands[p], 'i'))
            return invalid (function_, "block address requires an integer physical register");
        }
      } else if (param < s.parameters.size () && s.parameters[param].is_block ()) {
        if (!memory || memory->type != s.parameters[param])
          return invalid (function_, "invalid call block argument");
      } else if (param < s.parameters.size ()
                 && !matches (operands[p],
                              role (function_.target ().scalar_type (s.parameters[param]))))
        return invalid (function_, "invalid call argument");
      else if (param >= s.parameters.size ()
               && (detail::operand_access (operands[p]).get<const MachineLabel *> ()
                   || detail::operand_access (operands[p]).get<BuiltinReference> ()
                   || (memory && memory->type.kind () == ir::Type::Kind::ReturnBlock)))
        return invalid (function_, "invalid variadic argument");
    }
  }
  return {};
}
Result<MachineInstruction &> MachineRewriter::append (MachineBasicBlock &block, MachineOpcode code,
                                                      std::vector<MachineOperand> operands) {
  if (function_.next_instruction_ == UINT64_MAX)
    return invalid (function_, "instruction identity overflow");
  if (&block.function () != &function_) return invalid (function_, "foreign insert block");
  auto checked = validate (code, operands);
  if (!(checked).has_value ()) return checked.error ();
  block.instructions_.emplace_back (MachineInstruction::ConstructionKey{}, block, code,
                                    std::move (operands));
  auto &insn = block.instructions_.back ();
  insn.creation_order_ = function_.next_instruction_++;
  insn.position_ = std::prev (block.instructions_.end ());
  ++function_.revision_;
  return insn;
}
Result<MachineInstruction &> MachineRewriter::insert_before (MachineInstruction &anchor,
                                                             MachineOpcode code,
                                                             std::vector<MachineOperand> operands) {
  if (function_.next_instruction_ == UINT64_MAX)
    return invalid (function_, "instruction identity overflow");
  if (&anchor.block ().function () != &function_)
    return invalid (function_, "foreign instruction anchor");
  auto checked = validate (code, operands);
  if (!(checked).has_value ()) return checked.error ();
  auto &block = *anchor.block_;
  auto at = block.instructions_.emplace (anchor.position_, MachineInstruction::ConstructionKey{},
                                         block, code, std::move (operands));
  at->entry_setup_ = anchor.entry_setup_;
  at->creation_order_ = function_.next_instruction_++;
  at->position_ = at;
  ++function_.revision_;
  return *at;
}
Result<MachineInstruction &> MachineRewriter::insert_after (MachineInstruction &anchor,
                                                            MachineOpcode code,
                                                            std::vector<MachineOperand> operands) {
  if (function_.next_instruction_ == UINT64_MAX)
    return invalid (function_, "instruction identity overflow");
  if (&anchor.block ().function () != &function_)
    return invalid (function_, "foreign instruction anchor");
  auto checked = validate (code, operands);
  if (!(checked).has_value ()) return checked.error ();
  auto &block = *anchor.block_;
  auto at = block.instructions_.emplace (std::next (anchor.position_),
                                         MachineInstruction::ConstructionKey{}, block, code,
                                         std::move (operands));
  at->entry_setup_ = anchor.entry_setup_;
  at->creation_order_ = function_.next_instruction_++;
  at->position_ = at;
  ++function_.revision_;
  return *at;
}
Result<OptionalRef<MachineInstruction>> MachineRewriter::previous (MachineInstruction &insn) const {
  if (&insn.block ().function () != &function_)
    return invalid (function_, "foreign instruction navigation");
  if (insn.position_ == insn.block_->instructions_.begin ())
    return OptionalRef<MachineInstruction>{};
  return OptionalRef<MachineInstruction> (*std::prev (insn.position_));
}
Result<OptionalRef<MachineInstruction>> MachineRewriter::next (MachineInstruction &insn) const {
  if (&insn.block ().function () != &function_)
    return invalid (function_, "foreign instruction navigation");
  auto next = std::next (insn.position_);
  if (next == insn.block_->instructions_.end ()) return OptionalRef<MachineInstruction>{};
  return OptionalRef<MachineInstruction> (*next);
}
Result<void> MachineRewriter::replace (MachineInstruction &insn, MachineOpcode code,
                                       std::vector<MachineOperand> operands) {
  if (&insn.block ().function () != &function_) return invalid (function_, "foreign instruction");
  auto checked = validate (code, operands, insn.lowered_call_ && call (code));
  if (!(checked).has_value ()) return checked;
  if (call (code)) {
    checked = validate_dependencies (code, operands, insn.call_dependencies ());
    if (!(checked).has_value ()) return checked;
  }
  insn.opcode_ = code;
  insn.operands_ = std::move (operands);
  if (!call (code)) {
    insn.call_arguments_.clear ();
    insn.call_dependencies_.clear ();
    insn.lowered_call_ = false;
  }
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::lower_call (MachineInstruction &insn, MachineOpcode code,
                                          std::vector<MachineOperand> operands) {
  if (&insn.block ().function () != &function_ || !call (insn.opcode ()) || !call (code))
    return invalid (function_, "lowering requires a call instruction");
  auto checked = validate (code, operands, true);
  if (!(checked).has_value ()) return checked;
  checked = validate_dependencies (code, operands, insn.call_dependencies ());
  if (!(checked).has_value ()) return checked;
  insn.opcode_ = code;
  insn.operands_ = std::move (operands);
  insn.lowered_call_ = true;
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::erase (MachineInstruction &insn) {
  if (&insn.block ().function () != &function_)
    return invalid (function_, "foreign erased instruction");
  if (function_.edit_state_->tracks_handles)
    function_.edit_state_->erased[&insn] = function_.revision_ + 1;
  insn.block_->instructions_.erase (insn.position_);
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::set_entry_setup (MachineInstruction &i, bool setup) {
  if (&i.block ().function () != &function_)
    return invalid (function_, "foreign entry instruction");
  if (i.entry_setup_ != setup) {
    i.entry_setup_ = setup;
    ++function_.revision_;
  }
  return {};
}
Result<void> MachineRewriter::move_before (MachineInstruction &insn, MachineInstruction &anchor) {
  if (&insn.block ().function () != &function_ || &anchor.block ().function () != &function_)
    return invalid (function_, "foreign moved instruction");
  if (&insn == &anchor) return {};
  anchor.block_->instructions_.splice (anchor.position_, insn.block_->instructions_,
                                       insn.position_);
  insn.block_ = anchor.block_;
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::move_to (MachineInstruction &insn, MachineBasicBlock &block,
                                       bool at_end) {
  if (&insn.block ().function () != &function_ || &block.function () != &function_)
    return invalid (function_, "foreign moved instruction/block");
  auto destination = at_end ? block.instructions_.end () : block.instructions_.begin ();
  block.instructions_.splice (destination, insn.block_->instructions_, insn.position_);
  insn.block_ = &block;
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::move_label (MachineLabel &label, MachineBasicBlock &block) {
  if (&label.block ().function () != &function_ || &block.function () != &function_)
    return invalid (function_, "foreign moved label/block");
  auto &labels = label.block_->labels_;
  auto at = std::find (labels.begin (), labels.end (), &label);
  if (at == labels.end ()) return invalid (function_, "unowned label");
  labels.erase (at);
  block.labels_.push_back (&label);
  label.block_ = &block;
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::erase_label (MachineLabel &label) {
  if (&label.block ().function () != &function_ || label.address_taken ())
    return invalid (function_, "foreign or address-taken erased label");
  for (auto &b : function_.blocks ())
    for (auto &i : b.instructions ())
      for (auto &o : i.operands ())
        if (auto target = detail::operand_access (o).get<const MachineLabel *> ();
            target && *target == &label)
          return invalid (function_, "cannot erase a referenced label");
  if (function_.edit_state_->tracks_handles)
    function_.edit_state_->erased[&label] = function_.revision_ + 1;
  std::erase (label.block_->labels_, &label);
  if (label.source_ordinal_) function_.source_label_ordinals_.erase (*label.source_ordinal_);
  std::erase_if (function_.labels_, [&] (auto &owned) { return owned.get () == &label; });
  ++function_.revision_;
  return {};
}
Result<MachineBasicBlock &> MachineRewriter::create_block_before (MachineBasicBlock &anchor) {
  if (&anchor.function () != &function_)
    return invalid (function_, "foreign block insertion anchor");
  auto &block = detail::access (function_).create_block ();
  function_.blocks_.splice (anchor.position_, function_.blocks_, block.position_);
  return block;
}
Result<MachineBasicBlock &> MachineRewriter::create_block_after (MachineBasicBlock &anchor) {
  if (&anchor.function () != &function_)
    return invalid (function_, "foreign block insertion anchor");
  auto &block = detail::access (function_).create_block ();
  function_.blocks_.splice (std::next (anchor.position_), function_.blocks_, block.position_);
  return block;
}
Result<void> MachineRewriter::set_call_arguments (
  MachineInstruction &insn, std::span<const target::PhysicalRegister> registers) {
  if (&insn.block ().function () != &function_ || !call (insn.opcode ()))
    return invalid (function_, "call registers require a call instruction");
  for (auto reg : registers)
    if (!function_.target ().register_info (reg).get ())
      return invalid (function_, "foreign physical call register");
  std::vector<target::PhysicalRegister> unique;
  for (auto reg : registers)
    if (std::find (unique.begin (), unique.end (), reg) == unique.end ()) unique.push_back (reg);
  insn.call_arguments_ = std::move (unique);
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::validate_dependencies (
  MachineOpcode code, std::span<const MachineOperand> ops,
  std::span<const CallArgumentDependency> dependencies) const {
  if (!dependencies.empty () && (!call (code) || !function_.state ().operands_rewritten))
    return invalid (function_, "argument dependencies require an allocated call");
  std::size_t previous = 0;
  bool first = true;
  for (auto d : dependencies) {
    auto m = d.operand < ops.size () ? detail::operand_access (ops[d.operand]).get<MachineMemory> ()
                                     : nullptr;
    if (!detail::NodeAccess::value (d) || &detail::NodeAccess::value (d)->function () != &function_
        || !detail::NodeAccess::value (d)->type ().is_integer () || d.operand < 2 || !m
        || m->type.kind () != ir::Type::Kind::Undef || m->base || m->index
        || (!first && d.operand <= previous))
      return invalid (function_, "invalid stack argument dependency");
    previous = d.operand;
    first = false;
  }
  return {};
}
Result<void> MachineRewriter::set_call_dependencies (
  MachineInstruction &insn, std::span<const CallArgumentDependency> dependencies) {
  if (&insn.block ().function () != &function_ || !insn.has_lowered_call ())
    return invalid (function_, "argument dependencies require an owned lowered call");
  auto checked = validate_dependencies (insn.opcode (), insn.operands (), dependencies);
  if (!(checked).has_value ()) return checked;
  insn.call_dependencies_.assign (dependencies.begin (), dependencies.end ());
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::set_state (MachineFunctionState state) {
  if (state.frame_formed && !state.post_allocation_optimized)
    return invalid (function_, "frame formation requires post-allocation optimization");
  if (state.post_allocation_optimized && !state.edge_spills_inserted)
    return invalid (function_, "post-allocation optimization requires edge preservation");
  if (state.edge_spills_inserted && !state.operands_rewritten)
    return invalid (function_, "edge spills require allocated operands");
  if (state.operands_rewritten && !state.operands_legalized)
    return invalid (function_, "rewritten operands require legalization");
  if (state.operands_legalized && !state.machinized)
    return invalid (function_, "legalized operands require machinization");
  if ((function_.target ().architecture () == target::Architecture::X86_64)
      != std::holds_alternative<MachineFunctionState::X86> (state.target))
    return invalid (function_, "function state belongs to another architecture");
  function_.state_ = std::move (state);
  ++function_.revision_;
  return {};
}
Result<void> MachineRewriter::set_global_registers (
  std::span<const target::PhysicalRegister> registers) {
  std::vector<target::PhysicalRegister> globals;
  for (auto reg : registers) {
    auto info = function_.target ().register_info (reg).get ();
    if (!info || info->reserved) return invalid (function_, "invalid global physical register");
    if (std::find (globals.begin (), globals.end (), reg) == globals.end ())
      globals.push_back (reg);
  }
  for (auto &reg : function_.registers_)
    if (auto binding = reg->binding ())
      if (std::find (globals.begin (), globals.end (), *binding) == globals.end ())
        globals.push_back (*binding);
  function_.global_registers_ = std::move (globals);
  ++function_.revision_;
  return {};
}
MachineFunction::~MachineFunction () = default;
Result<void> MachineFunction::status () const {
  if (!edit_state_) return invalid (*this, "moved-from machine function");
  if (edit_state_->error) return *edit_state_->error;
  if (edit_state_->pending) return invalid (*this, "machine edit batch has not finished");
  return {};
}
Result<void> MachineFunction::verify () const {
  auto edit_status = status ();
  if (!(edit_status).has_value ()) return edit_status.error ();
  if (state_.frame_formed && !state_.post_allocation_optimized)
    return invalid (*this, "frame formation without post-allocation optimization");
  if (state_.post_allocation_optimized && !state_.edge_spills_inserted)
    return invalid (*this, "post-allocation optimization without edge preservation");
  if (state_.edge_spills_inserted && !state_.operands_rewritten)
    return invalid (*this, "edge spills without allocated operands");
  if (state_.operands_rewritten && !state_.operands_legalized)
    return invalid (*this, "rewritten operands without legalization");
  if (state_.operands_legalized && !state_.machinized)
    return invalid (*this, "legalized operands without machinization");
  auto signature = signature_.verify ();
  if (!(signature).has_value ()) return invalid (*this, signature.error ().message);
  if (arguments_.size () != signature_.parameters.size ())
    return invalid (*this, "missing machine arguments");
  for (auto type : signature_.parameters)
    if (!valid_abi_type (*this, type)) return invalid (*this, "invalid function block ABI");
  if (blocks_.empty ()) return invalid (*this, "missing machine body");
  if ((target_.architecture () == target::Architecture::X86_64)
      != std::holds_alternative<MachineFunctionState::X86> (state_.target))
    return invalid (*this, "invalid architecture state");
  MachineRewriter checked (const_cast<MachineFunction &> (*this));
  for (auto &symbol : symbols_)
    if (symbol->signature) {
      auto valid = symbol->signature->verify ();
      if (!(valid).has_value ()) return invalid (*this, valid.error ().message);
      for (auto type : symbol->signature->parameters)
        if (!valid_abi_type (*this, type)) return invalid (*this, "invalid symbol block ABI");
    }
  for (auto &block : blocks_) {
    bool terminated = false;
    for (auto &insn : block->instructions ()) {
      if (terminated) return invalid (*this, "control-flow instruction in middle of machine block");
      if (&insn.block () != block.get ()) return invalid (*this, "broken instruction owner");
      auto valid = checked.validate (insn.opcode (), insn.operands (), insn.has_lowered_call ());
      if (!(valid).has_value ()) return valid;
      valid = checked.validate_dependencies (insn.opcode (), insn.operands (),
                                             insn.call_dependencies ());
      if (!(valid).has_value ()) return valid;
      if (state_.operands_rewritten)
        for (auto &o : insn.operands ()) {
          auto virtual_register = [] (MachineRegister r) {
            return std::holds_alternative<const MachineVirtualRegister *> (
              detail::NodeAccess::storage (r));
          };
          if (detail::operand_access (o).get<const MachineVirtualRegister *> ())
            return invalid (*this, "virtual operand after allocation rewrite");
          if (auto m = detail::operand_access (o).get<MachineMemory> ();
              m
              && ((m->base && virtual_register (*m->base))
                  || (m->index && virtual_register (*m->index))))
            return invalid (*this, "virtual address after allocation rewrite");
        }
      if (!call (insn.opcode ()) && !insn.call_arguments ().empty ())
        return invalid (*this, "call metadata on non-call");
      for (auto reg : insn.call_arguments ())
        if (!target_.register_info (reg).get ()) return invalid (*this, "foreign call register");
      auto code = insn.opcode ();
      if (auto opcode = std::get_if<ir::Opcode> (&code))
        terminated = ir::instruction_descriptor (*opcode).get ()->flow != ir::ControlFlow::Next;
    }
  }
  return {};
}
std::string MachineFunction::dump () const {
  std::ostringstream out;
  out << "machine " << module_ << '/' << name_ << " target=" << target_.name () << '\n';
  std::unordered_map<const MachineVirtualRegister *, std::size_t> regs;
  for (auto &reg : registers_) {
    auto id = regs.size ();
    regs.emplace (reg.get (), id);
    out << "  %" << id << ' ' << reg->name ();
    if (auto binding = reg->binding ()) out << ':' << target_.register_info (*binding).get ()->name;
    out << " type=" << static_cast<unsigned> (reg->type ().kind ());
    if (reg->is_memory_backed ()) out << " memory-backed";
    out << '\n';
  }
  std::unordered_map<const MachineLabel *, std::size_t> labels;
  for (auto &label : labels_) labels.emplace (label.get (), labels.size ());
  std::unordered_map<const MachineStackSlot *, std::size_t> slots;
  for (auto &slot : slots_) slots.emplace (slot.get (), slots.size ());
  auto reg = [&] (MachineRegister value) {
    if (auto v = std::get_if<const MachineVirtualRegister *> (detail::NodeAccess::storage (&value)))
      out << '%' << regs.at (*v);
    else
      out << '$'
          << target_
               .register_info (
                 std::get<target::PhysicalRegister> (detail::NodeAccess::storage (value)))
               .get ()
               ->name;
  };
  std::size_t b = 0;
  for (auto &block : blocks_) {
    out << "bb" << b++ << ' ' << block->name () << ":\n";
    for (auto label : ir::detail::ReferenceAccess::pointers (block->labels ()))
      out << "  L" << labels.at (label) << ' ' << label->name ()
          << (label->address_taken () ? " address-taken" : "") << '\n';
    for (auto &insn : block->instructions ()) {
      out << "    " << opcode_name (insn.opcode ());
      for (auto &o : insn.operands ()) {
        out << ' ';
        if (auto v = detail::operand_access (o).get<const MachineVirtualRegister *> ())
          reg (detail::NodeAccess::make_MachineRegister (*v));
        else if (auto v = detail::operand_access (o).get<target::PhysicalRegister> ())
          reg (*v);
        else if (auto v = detail::operand_access (o).get<const MachineLabel *> ())
          out << 'L' << labels.at (*v);
        else if (auto v = detail::operand_access (o).get<MachineSymbolReference> ()) {
          out << '@' << detail::NodeAccess::symbol (*(v))->module << '/'
              << detail::NodeAccess::symbol (*(v))->name;
          if (detail::NodeAccess::symbol (*(v))->anonymous_ordinal)
            out << "anonymous#" << *detail::NodeAccess::symbol (*(v))->anonymous_ordinal;
          if (detail::NodeAccess::symbol (*(v))->builtin)
            out << "{builtin-prototype:"
                << static_cast<unsigned> (*detail::NodeAccess::symbol (*(v))->builtin) << '}';
          out << '+' << v->displacement;
        } else if (auto v = detail::operand_access (o).get<StackSlotReference> ())
          out << "slot" << slots.at (detail::NodeAccess::slot (*(v))) << '+' << v->displacement;
        else if (auto v = detail::operand_access (o).get<BuiltinReference> ())
          out << "builtin:" << static_cast<unsigned> (v->builtin);
        else if (auto m = detail::operand_access (o).get<MachineMemory> ()) {
          out << "mem(" << static_cast<unsigned> (m->type.kind ()) << ',' << m->type.block_size ()
              << ',' << m->displacement;
          if (m->base) {
            out << ',';
            reg (*m->base);
          }
          if (m->index) {
            out << ',';
            reg (*m->index);
            out << '*' << m->scale;
          }
          out << ",alias=" << m->alias << ",nonalias=" << m->nonalias << ')';
        } else if (auto v = detail::operand_access (o).get<std::int64_t> ())
          out << *v;
        else if (auto v = detail::operand_access (o).get<std::uint64_t> ())
          out << *v << 'u';
        else if (auto v = detail::operand_access (o).get<float> ())
          out << "f:0x" << std::hex << std::bit_cast<std::uint32_t> (*v) << std::dec;
        else if (auto v = detail::operand_access (o).get<double> ())
          out << "d:0x" << std::hex << std::bit_cast<std::uint64_t> (*v) << std::dec;
        else if (auto v = detail::operand_access (o).get<long double> ()) {
          constexpr auto bytes
            = std::numeric_limits<long double>::digits == 64 ? 10 : sizeof (long double);
          out << "ld:0x" << std::hex << std::setfill ('0');
          for (unsigned n = 0; n < bytes; ++n)
            out << std::setw (2)
                << static_cast<unsigned> (reinterpret_cast<const unsigned char *> (v)[n]);
          out << std::dec << std::setfill (' ');
        }
      }
      if (!insn.call_arguments ().empty ()) {
        out << " [call-regs";
        for (auto r : insn.call_arguments ()) out << ' ' << target_.register_info (r).get ()->name;
        out << ']';
      }
      for (auto d : insn.call_dependencies ()) {
        out << " [stack-argument " << d.operand << " depends-on ";
        reg (detail::NodeAccess::make_MachineRegister (detail::NodeAccess::value (d)));
        out << ']';
      }
      out << '\n';
    }
  }
  return out.str ();
}
Result<MachineControlFlow> MachineControlFlow::build (const MachineFunction &function) {
  auto valid = function.verify ();
  if (!(valid).has_value ()) return valid.error ();
  MachineControlFlow cfg;
  cfg.owner_ = &function;
  cfg.revision = function.revision ();
  detail::NodeAccess::blocks (cfg) = {nullptr, nullptr};
  std::unordered_map<const MachineBasicBlock *, std::size_t> indices;
  for (auto &block : function.blocks ()) {
    indices.emplace (&block, detail::NodeAccess::blocks (cfg).size ());
    detail::NodeAccess::blocks (cfg).push_back (&block);
  }
  std::vector<bool> addressed (detail::NodeAccess::blocks (cfg).size ());
  for (auto &block : function.blocks ()) {
    for (auto label : ir::detail::ReferenceAccess::pointers (block.labels ()))
      if (label->address_taken ()) addressed[indices.at (&label->block ())] = true;
    for (auto &insn : block.instructions ())
      if (insn.opcode () == MachineOpcode (ir::Opcode::LAddr)) {
        auto label = *detail::operand_access (insn.operands ()[1]).get<const MachineLabel *> ();
        addressed[indices.at (&label->block ())] = true;
      }
  }
  cfg.incoming.resize (detail::NodeAccess::blocks (cfg).size ());
  cfg.outgoing.resize (detail::NodeAccess::blocks (cfg).size ());
  auto edge
    = [&] (std::size_t from, std::size_t to, EdgeKind kind, std::size_t operand = SIZE_MAX) {
        auto id = cfg.edges.size ();
        cfg.edges.push_back ({from, to, kind, operand});
        cfg.outgoing[from].push_back (id);
        cfg.incoming[to].push_back (id);
      };
  for (std::size_t b = 2; b < detail::NodeAccess::blocks (cfg).size (); ++b) {
    const MachineInstruction *last = nullptr;
    auto instructions = detail::NodeAccess::blocks (cfg)[b]->instructions ();
    if (instructions.begin () != instructions.end ()) last = &*std::prev (instructions.end ());
    auto code = last ? last->opcode () : MachineOpcode (MachinePseudoOpcode::Use);
    auto opcode = std::get_if<ir::Opcode> (&code);
    bool fallthrough = true;
    if (opcode) {
      auto descriptor = ir::instruction_descriptor (*opcode).get ();
      fallthrough = descriptor->flow != ir::ControlFlow::Exit && *opcode != ir::Opcode::Jmp;
      if (fallthrough && b + 1 < detail::NodeAccess::blocks (cfg).size ())
        edge (b, b + 1, EdgeKind::Fallthrough);
      if (descriptor->flow != ir::ControlFlow::Next)
        for (std::size_t p = 0; p < last->operands ().size (); ++p)
          if (auto label
              = detail::operand_access (last->operands ()[p]).get<const MachineLabel *> ())
            edge (b, indices.at (&(*label)->block ()),
                  *opcode == ir::Opcode::Switch ? EdgeKind::SwitchCase : EdgeKind::Branch, p);
    } else if (b + 1 < detail::NodeAccess::blocks (cfg).size ())
      edge (b, b + 1, EdgeKind::Fallthrough);
  }
  // As in the original generator, append indirect edges after direct edges;
  // target layout order also fixes incoming order for later data-flow analyses.
  for (std::size_t b = 2; b < detail::NodeAccess::blocks (cfg).size (); ++b) {
    auto instructions = detail::NodeAccess::blocks (cfg)[b]->instructions ();
    if (instructions.begin () != instructions.end ()
        && std::prev (instructions.end ())->opcode () == MachineOpcode (ir::Opcode::JmpIndirect))
      for (std::size_t to = 2; to < detail::NodeAccess::blocks (cfg).size (); ++to)
        if (addressed[to]) edge (b, to, EdgeKind::Indirect);
  }
  for (std::size_t b = 0; b < detail::NodeAccess::blocks (cfg).size (); ++b) {
    if (b != 0 && cfg.incoming[b].empty ()) edge (0, b, EdgeKind::Entry);
    if (b != 1 && cfg.outgoing[b].empty ()) edge (b, 1, EdgeKind::Exit);
  }
  return cfg;
}
Result<void> MachineControlFlow::verify (const MachineFunction &function) const {
  if (!current (function)) return invalid (function, "stale or foreign machine CFG");
  auto canonical = build (function);
  if (!(canonical).has_value ()) return canonical.error ();
  if (blocks_ != detail::NodeAccess::blocks (((canonical)).value ())
      || incoming.size () != blocks_.size () || outgoing.size () != blocks_.size ()
      || edges.size () != canonical.value ().edges.size ())
    return invalid (function, "invalid machine CFG dimensions/layout");
  std::vector<unsigned char> in (edges.size ()), out (edges.size ());
  for (std::size_t b = 0; b < blocks_.size (); ++b) {
    for (auto e : incoming[b])
      if (e >= edges.size () || edges[e].destination != b || in[e]++)
        return invalid (function, "invalid incoming machine edge");
    for (auto e : outgoing[b])
      if (e >= edges.size () || edges[e].source != b || out[e]++)
        return invalid (function, "invalid outgoing machine edge");
  }
  for (std::size_t e = 0; e < edges.size (); ++e)
    if (!in[e] || !out[e]) return invalid (function, "unowned machine edge");
  auto keys = [] (const MachineControlFlow &g) {
    std::vector<std::tuple<std::size_t, std::size_t, EdgeKind, std::size_t>> v;
    for (auto e : g.edges) v.emplace_back (e.source, e.destination, e.kind, e.operand);
    std::sort (v.begin (), v.end ());
    return v;
  };
  if (keys (*this) != keys ((canonical).value ()))
    return invalid (function, "machine edges disagree with block-owned control flow");
  return {};
}
}  // namespace mirnext::codegen
