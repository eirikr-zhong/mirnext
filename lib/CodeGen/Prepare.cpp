#include "mirnext/IR/Vector.hpp"
/* Licensed under the MIT License. Direct canonical IR to owning machine IR. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/CodeGen/Detail/Vector.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace mirnext::codegen {
namespace {
ir::Type abi_type (ir::Type type, const target::TargetMachine &target) {
  if (type.kind () == ir::Type::Kind::Bool) return ir::Type::i64 ();
  return type.is_float () ? target.scalar_type (type) : type;
}
ir::Signature signature (ir::Signature value, const target::TargetMachine &target) {
  for (auto &type : value.results) type = abi_type (type, target);
  for (auto &type : value.parameters) type = abi_type (type, target);
  return value;
}
MachineSymbol symbol (const ir::Symbol &source, const target::TargetMachine &target) {
  MachineSymbol::Kind kind = MachineSymbol::Kind::Forward;
  switch (source.kind ()) {
  case ir::Symbol::Kind::Function: kind = MachineSymbol::Kind::Function; break;
  case ir::Symbol::Kind::Data: kind = MachineSymbol::Kind::Data; break;
  case ir::Symbol::Kind::Prototype: kind = MachineSymbol::Kind::Prototype; break;
  case ir::Symbol::Kind::Import: kind = MachineSymbol::Kind::Import; break;
  case ir::Symbol::Kind::Forward: break;
  }
  MachineSymbol result{std::string (source.module ().name ()),
                       std::string (source.name ()),
                       kind,
                       source.signature ().get ()
                         ? std::optional (signature (*source.signature ().get (), target))
                         : std::nullopt,
                       source.exported (),
                       {}};
  if (source.name ().empty ()) {
    std::size_t ordinal = 0;
    for (auto &candidate : source.module ().symbols ()) {
      if (&candidate == &source) {
        result.anonymous_ordinal = ordinal;
        break;
      }
      ++ordinal;
    }
  }
  return result;
}
}  // namespace
Result<MachineFunction> prepare_machine_ir (const ir::Function &source,
                                            const target::TargetMachine &target) {
  auto error = [&] (std::string why) {
    return Error{ErrorCode::InvalidOperand, std::string (source.module ().name ()) + "/"
                                              + std::string (source.symbol ().name ()) + " ["
                                              + std::string (target.name ())
                                              + ", prepare-machine-ir]: " + std::move (why)};
  };
  auto valid = source.module ().program ().status ();
  if (!(valid).has_value ()) return error (valid.error ().message);
  valid = source.verify ();
  if (!(valid).has_value ()) return error (valid.error ().message);
  auto phase = source.require_register_ir ();
  if (!(phase).has_value ()) return error (phase.error ().message);
  if (source.needs_materialization ())
    return error ("stack scopes must be materialized before machine preparation");
  Result<MachineFunction> result (std::in_place, std::string (source.module ().name ()),
                                  std::string (source.symbol ().name ()),
                                  signature (source.signature (), target), target);
  auto &function = (result).value ();
  MachineRewriter rewriter (function);
  // A hardware declaration reserves a module-wide observable register even
  // in functions that do not themselves declare or read that binding.
  std::vector<target::PhysicalRegister> global_registers;
  for (auto &f : source.module ().functions ())
    if (f.has_hardware_bindings ()) {
      for (auto &reg : f.registers ())
        if (!reg.hardware_binding ().empty ()) {
          auto physical = target.find_register (reg.hardware_binding ());
          if (!(physical).has_value ()) return error (physical.error ().message);
          if (std::find (global_registers.begin (), global_registers.end (), (physical).value ())
              == global_registers.end ())
            global_registers.push_back ((physical).value ());
        }
    }
  auto globals = rewriter.set_global_registers (global_registers);
  if (!(globals).has_value ()) return globals.error ();
  std::unordered_map<const ir::Register *, const MachineVirtualRegister *> registers;
  std::vector<MachineOperand> hardware_uses;
  auto add_register = [&] (const ir::Register &reg) -> Result<void> {
    std::optional<target::PhysicalRegister> binding;
    if (!reg.hardware_binding ().empty ()) {
      auto found = target.find_register (reg.hardware_binding ());
      if (!(found).has_value ()) return error (found.error ().message);
      binding = (found).value ();
    }
    auto added
      = detail::access (function).create_register (abi_type (reg.type (), target),
                                                   std::string (reg.name ()), reg.is_argument (),
                                                   reg.is_memory_backed (), binding);
    if (!(added).has_value ()) return error (added.error ().message);
    registers.emplace (&reg, &(added).value ());
    if (binding && !reg.is_memory_backed ()) hardware_uses.emplace_back ((added).value ());
    return {};
  };
  for (auto reg : ir::detail::ReferenceAccess::pointers (source.arguments ())) {
    auto added = add_register (*reg);
    if (!(added).has_value ()) return added.error ();
  }
  for (auto &reg : source.registers ())
    if (!reg.is_argument ()) {
      auto added = add_register (reg);
      if (!(added).has_value ()) return added.error ();
    }
  std::unordered_map<const ir::BasicBlock *, MachineBasicBlock *> blocks;
  MachineBasicBlock *prologue = nullptr;
  if (!ir::detail::ReferenceAccess::pointers (source.blocks ().begin ()->labels ()).empty ())
    prologue = &detail::access (function).create_block ();
  for (auto &block : source.blocks ())
    blocks.emplace (&block, &detail::access (function).create_block (std::string (block.name ())));
  // Preserve the generator's leading address anchor separately from public
  // labels, including an empty prologue before an already labelled first block.
  auto entry
    = detail::access (function).create_label (prologue ? *prologue
                                                       : *blocks.at (&*source.blocks ().begin ()),
                                              {}, false, {}, true);
  if (!(entry).has_value ()) return entry.error ();
  std::unordered_set<const ir::Label *> address_taken;
  for (auto data : ir::detail::ReferenceAccess::pointers (source.label_data_uses ()))
    if (auto ref = std::get_if<ir::LabelReference> (&data->initializer ())) {
      if (&ir::detail::NodeAccess::label (*(ref))->function () == &source)
        address_taken.insert (ir::detail::NodeAccess::label (*(ref)));
      if (ir::detail::NodeAccess::base (*(ref))
          && &ir::detail::NodeAccess::base (*(ref))->function () == &source)
        address_taken.insert (ir::detail::NodeAccess::base (*(ref)));
    }
  for (auto &block : source.blocks ())
    for (auto &insn : block.instructions ())
      if (insn.opcode () == ir::Opcode::LAddr)
        address_taken.insert (
          *ir::detail::operand_access (insn.operands ()[1]).get<const ir::Label *> ());
  std::unordered_map<const ir::Label *, const MachineLabel *> labels;
  std::unordered_map<const ir::Label *, std::size_t> label_ordinals;
  for (auto &label : source.labels ()) label_ordinals.emplace (&label, label_ordinals.size ());
  for (auto &block : source.blocks ())
    for (auto label : ir::detail::ReferenceAccess::pointers (block.labels ())) {
      auto created
        = detail::access (function).create_label (*blocks.at (&block), std::string (label->name ()),
                                                  address_taken.contains (label),
                                                  label_ordinals.at (label));
      if (!(created).has_value ()) return created.error ();
      labels.emplace (label, &(created).value ());
    }
  std::unordered_map<const ir::Symbol *, const MachineSymbol *> symbols;
  std::unordered_map<const ir::MemoryLocation *, const MachineMemoryLocation *> locations;
  MachineBasicBlock *current_block = nullptr;
  detail::VectorConstantCache vector_constants;
  auto convert = [&] (const ir::Operand &operand) -> Result<MachineOperand> {
    if (auto constant = ir::detail::operand_access (operand).get<ir::VectorConstant> ()) {
      if (!current_block) return error ("vector constant has no insertion block");
      return detail::prepare_vector_constant (function, *current_block, *constant,
                                              vector_constants);
    }
    if (auto reg = ir::detail::operand_access (operand).get<const ir::Register *> ())
      return detail::NodeAccess::make_MachineOperand (*registers.at (*reg));
    if (auto label = ir::detail::operand_access (operand).get<const ir::Label *> ())
      return detail::NodeAccess::make_MachineOperand (*labels.at (*label));
    if (auto reference = ir::detail::operand_access (operand).get<ir::SymbolReference> ()) {
      auto [at, fresh] = symbols.try_emplace (ir::detail::NodeAccess::symbol (*(reference)));
      if (fresh) {
        auto interned = detail::access (function).intern_symbol (
          symbol (*ir::detail::NodeAccess::symbol (*(reference)), target));
        if (!(interned).has_value ()) return interned.error ();
        at->second = &(interned).value ();
      }
      return detail::NodeAccess::make_MachineOperand (
        detail::NodeAccess::make_MachineSymbolReference (at->second, reference->displacement));
    }
    if (auto m = ir::detail::operand_access (operand).get<ir::Memory> ()) {
      const MachineMemoryLocation *location = nullptr;
      if (m->location ().get ()) {
        auto [at, fresh] = locations.try_emplace (m->location ().get ());
        if (fresh) at->second = &detail::access (function).create_memory_location ();
        location = at->second;
      }
      auto type = abi_type (m->type, target);
      if (type.kind () == ir::Type::Kind::Pointer) type = ir::Type::i64 ();
      return detail::NodeAccess::make_MachineOperand (
        detail::NodeAccess::make_MachineMemory (type,
                                                ir::detail::NodeAccess::base (*(m))
                                                  ? detail::NodeAccess::optional_register (
                                                      detail::NodeAccess::make_MachineRegister (
                                                        registers.at (
                                                          ir::detail::NodeAccess::base (*(m)))))
                                                  : std::nullopt,
                                                ir::detail::NodeAccess::index (*(m))
                                                  ? detail::NodeAccess::optional_register (
                                                      detail::NodeAccess::make_MachineRegister (
                                                        registers.at (
                                                          ir::detail::NodeAccess::index (*(m)))))
                                                  : std::nullopt,
                                                m->displacement,
                                                ir::detail::NodeAccess::index (*(m)) ? m->scale : 1,
                                                m->alias, m->nonalias,
                                                ::mirnext::detail::borrow_optional (location)));
    }
    if (auto v = ir::detail::operand_access (operand).get<std::int64_t> ())
      return detail::NodeAccess::make_MachineOperand (*v);
    if (auto v = ir::detail::operand_access (operand).get<std::uint64_t> ())
      return detail::NodeAccess::make_MachineOperand (*v);
    if (auto v = ir::detail::operand_access (operand).get<float> ())
      return detail::NodeAccess::make_MachineOperand (*v);
    if (auto v = ir::detail::operand_access (operand).get<double> ())
      return detail::NodeAccess::make_MachineOperand (*v);
    if (auto v = ir::detail::operand_access (operand).get<long double> ()) {
      if constexpr (sizeof (long double) == sizeof (double))
        return detail::NodeAccess::make_MachineOperand (static_cast<double> (*v));
      else
        return detail::NodeAccess::make_MachineOperand (*v);
    }
    return error ("string operands must be simplified before machine preparation");
  };
  for (auto &block : source.blocks ())
    for (auto &insn : block.instructions ()) {
      auto &destination = *blocks.at (&block);
      if (current_block != &destination) {
        vector_constants.clear ();
        current_block = &destination;
      }
      if (insn.opcode () == ir::Opcode::Ret && !hardware_uses.empty ()) {
        auto use = rewriter.append (destination, MachinePseudoOpcode::Use, hardware_uses);
        if (!(use).has_value ()) return use.error ();
      }
      std::vector<MachineOperand> operands;
      operands.reserve (insn.operands ().size ());
      for (std::size_t n = 0; n < insn.operands ().size (); ++n) {
        if (insn.opcode () == ir::Opcode::VShuffle && n == 3) continue;
        auto &operand = insn.operands ()[n];
        auto converted = convert (operand);
        if (!(converted).has_value ()) return converted.error ();
        operands.push_back (std::move ((converted).value ()));
      }
      if (ir::detail::vector_opcode (insn.opcode ()) && insn.opcode () != ir::Opcode::VMov
          && insn.opcode () != ir::Opcode::VAdd && insn.opcode () != ir::Opcode::VSub
          && insn.opcode () != ir::Opcode::VAnd && insn.opcode () != ir::Opcode::VOr
          && insn.opcode () != ir::Opcode::VXor) {
        auto &operand = insn.operands ()[insn.opcode () == ir::Opcode::VSplat ? 0 : 1];
        auto reg = ir::detail::operand_access (operand).get<const ir::Register *> ();
        auto memory = ir::detail::operand_access (operand).get<ir::Memory> ();
        auto constant = ir::detail::operand_access (operand).get<ir::VectorConstant> ();
        auto type = reg ? (*reg)->type () : memory ? memory->type : constant->type;
        auto mask = insn.opcode () == ir::Opcode::VShuffle
                      ? ir::detail::operand_access (insn.operands ()[3]).get<ir::VectorConstant> ()
                      : nullptr;
        auto lowered = detail::prepare_vector_operation (function, destination, insn.opcode (),
                                                         type, operands, vector_constants, mask);
        if (!lowered.has_value ()) return error (lowered.error ().message);
        continue;
      }
      auto added
        = rewriter.append (destination, target.opcode (insn.opcode ()), std::move (operands));
      if (!(added).has_value ()) return added.error ();
    }
  // CFG construction verifies the complete machine function once. Repeating
  // that same ownership/type walk here would double symbol checks on large IR.
  auto cfg = MachineControlFlow::build (function);
  if (!(cfg).has_value ()) return cfg.error ();
  return std::move ((result).value ());
}
}  // namespace mirnext::codegen
