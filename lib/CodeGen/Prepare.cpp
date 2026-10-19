/* Licensed under the MIT License. Direct canonical IR to owning machine IR. */
#include "mirnext/CodeGen/MachineIR.hpp"
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
                       source.signature ()
                         ? std::optional (signature (*source.signature (), target))
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
Result<std::unique_ptr<MachineFunction>> prepare_machine_ir (const ir::Function &source,
                                                             const target::TargetMachine &target) {
  auto error = [&] (std::string why) {
    return Error{ErrorCode::InvalidOperand, std::string (source.module ().name ()) + "/"
                                              + std::string (source.symbol ().name ()) + " ["
                                              + std::string (target.name ())
                                              + ", prepare-machine-ir]: " + std::move (why)};
  };
  auto valid = source.verify ();
  if (!valid) return error (valid.error ().message);
  auto phase = source.require_register_ir ();
  if (!phase) return error (phase.error ().message);
  if (source.needs_materialization ())
    return error ("stack scopes must be materialized before machine preparation");
  auto result = std::make_unique<MachineFunction> (std::string (source.module ().name ()),
                                                   std::string (source.symbol ().name ()),
                                                   signature (source.signature (), target), target);
  auto &function = *result;
  MachineRewriter rewriter (function);
  // A hardware declaration reserves a module-wide observable register even
  // in functions that do not themselves declare or read that binding.
  std::vector<target::PhysicalRegister> global_registers;
  for (auto &f : source.module ().functions ())
    if (f.has_hardware_bindings ()) {
      for (auto &reg : f.registers ())
        if (!reg.hardware_binding ().empty ()) {
          auto physical = target.find_register (reg.hardware_binding ());
          if (!physical) return error (physical.error ().message);
          if (std::find (global_registers.begin (), global_registers.end (), *physical)
              == global_registers.end ())
            global_registers.push_back (*physical);
        }
    }
  auto globals = rewriter.set_global_registers (global_registers);
  if (!globals) return globals.error ();
  std::unordered_map<const ir::Register *, const MachineVirtualRegister *> registers;
  std::vector<MachineOperand> hardware_uses;
  auto add_register = [&] (const ir::Register &reg) -> Result<void> {
    std::optional<target::PhysicalRegister> binding;
    if (!reg.hardware_binding ().empty ()) {
      auto found = target.find_register (reg.hardware_binding ());
      if (!found) return error (found.error ().message);
      binding = *found;
    }
    auto added
      = function.create_register (abi_type (reg.type (), target), std::string (reg.name ()),
                                  reg.is_argument (), reg.is_memory_backed (), binding);
    if (!added) return error (added.error ().message);
    registers.emplace (&reg, *added);
    if (binding && !reg.is_memory_backed ()) hardware_uses.emplace_back (**added);
    return {};
  };
  for (auto reg : source.arguments ()) {
    auto added = add_register (*reg);
    if (!added) return added.error ();
  }
  for (auto &reg : source.registers ())
    if (!reg.is_argument ()) {
      auto added = add_register (reg);
      if (!added) return added.error ();
    }
  std::unordered_map<const ir::BasicBlock *, MachineBasicBlock *> blocks;
  MachineBasicBlock *prologue = nullptr;
  if (!source.blocks ().begin ()->labels ().empty ()) prologue = &function.create_block ();
  for (auto &block : source.blocks ())
    blocks.emplace (&block, &function.create_block (std::string (block.name ())));
  // Preserve the generator's leading address anchor separately from public
  // labels, including an empty prologue before an already labelled first block.
  auto entry
    = function.create_label (prologue ? *prologue : *blocks.at (&*source.blocks ().begin ()), {},
                             false, {}, true);
  if (!entry) return entry.error ();
  std::unordered_set<const ir::Label *> address_taken;
  for (auto data : source.label_data_uses ())
    if (auto ref = std::get_if<ir::LabelReference> (&data->initializer ())) {
      if (&ref->label->function () == &source) address_taken.insert (ref->label);
      if (ref->base && &ref->base->function () == &source) address_taken.insert (ref->base);
    }
  for (auto &block : source.blocks ())
    for (auto &insn : block.instructions ())
      if (insn.opcode () == ir::Opcode::LAddr)
        address_taken.insert (*insn.operands ()[1].get<const ir::Label *> ());
  std::unordered_map<const ir::Label *, const MachineLabel *> labels;
  std::unordered_map<const ir::Label *, std::size_t> label_ordinals;
  for (auto &label : source.labels ()) label_ordinals.emplace (&label, label_ordinals.size ());
  for (auto &block : source.blocks ())
    for (auto label : block.labels ()) {
      auto created
        = function.create_label (*blocks.at (&block), std::string (label->name ()),
                                 address_taken.contains (label), label_ordinals.at (label));
      if (!created) return created.error ();
      labels.emplace (label, *created);
    }
  std::unordered_map<const ir::Symbol *, const MachineSymbol *> symbols;
  std::unordered_map<const ir::MemoryLocation *, const MachineMemoryLocation *> locations;
  auto convert = [&] (const ir::Operand &operand) -> Result<MachineOperand> {
    if (auto reg = operand.get<const ir::Register *> ())
      return MachineOperand (*registers.at (*reg));
    if (auto label = operand.get<const ir::Label *> ()) return MachineOperand (*labels.at (*label));
    if (auto reference = operand.get<ir::SymbolReference> ()) {
      auto [at, fresh] = symbols.try_emplace (reference->symbol);
      if (fresh) {
        auto interned = function.intern_symbol (symbol (*reference->symbol, target));
        if (!interned) return interned.error ();
        at->second = *interned;
      }
      return MachineOperand (MachineSymbolReference{at->second, reference->displacement});
    }
    if (auto m = operand.get<ir::Memory> ()) {
      const MachineMemoryLocation *location = nullptr;
      if (m->location ()) {
        auto [at, fresh] = locations.try_emplace (m->location ());
        if (fresh) at->second = &function.create_memory_location ();
        location = at->second;
      }
      auto type = abi_type (m->type, target);
      if (type.kind () == ir::Type::Kind::Pointer) type = ir::Type::i64 ();
      return MachineOperand (
        MachineMemory{type,
                      m->base ? std::optional<MachineRegister> (registers.at (m->base))
                              : std::nullopt,
                      m->index ? std::optional<MachineRegister> (registers.at (m->index))
                               : std::nullopt,
                      m->displacement, m->index ? m->scale : 1, m->alias, m->nonalias, location});
    }
    if (auto v = operand.get<std::int64_t> ()) return MachineOperand (*v);
    if (auto v = operand.get<std::uint64_t> ()) return MachineOperand (*v);
    if (auto v = operand.get<float> ()) return MachineOperand (*v);
    if (auto v = operand.get<double> ()) return MachineOperand (*v);
    if (auto v = operand.get<long double> ()) {
      if constexpr (sizeof (long double) == sizeof (double))
        return MachineOperand (static_cast<double> (*v));
      else
        return MachineOperand (*v);
    }
    return error ("string operands must be simplified before machine preparation");
  };
  for (auto &block : source.blocks ())
    for (auto &insn : block.instructions ()) {
      auto &destination = *blocks.at (&block);
      if (insn.opcode () == ir::Opcode::Ret && !hardware_uses.empty ()) {
        auto use = rewriter.append (destination, MachinePseudoOpcode::Use, hardware_uses);
        if (!use) return use.error ();
      }
      std::vector<MachineOperand> operands;
      operands.reserve (insn.operands ().size ());
      for (auto &operand : insn.operands ()) {
        auto converted = convert (operand);
        if (!converted) return converted.error ();
        operands.push_back (std::move (*converted));
      }
      auto added
        = rewriter.append (destination, target.opcode (insn.opcode ()), std::move (operands));
      if (!added) return added.error ();
    }
  // CFG construction verifies the complete machine function once. Repeating
  // that same ownership/type walk here would double symbol checks on large IR.
  auto cfg = MachineControlFlow::build (function);
  if (!cfg) return cfg.error ();
  return result;
}
}  // namespace mirnext::codegen
