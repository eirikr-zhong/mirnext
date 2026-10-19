/* Rules derived from MIR, Copyright (C) 2020-2024 Vladimir Makarov.
   Licensed under the MIT License. Ordered reloads over owning machine IR. */
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include <algorithm>
#include <array>

namespace mirnext::codegen {
namespace {
using ir::Opcode; using ir::Type; using K = Type::Kind;
Error invalid(const MachineFunction &f, std::string reason) {
  return {ErrorCode::InvalidOperand, std::string(f.module_name()) + "/" + std::string(f.name())
    + " [" + std::string(f.target().name()) + ", legalize-operands]: " + std::move(reason)};
}
struct ReloadType { Type type; bool output; };
ReloadType reload_type(const MachineFunction &f, const MachineInstruction &i, std::size_t n) {
  auto code = std::get<Opcode>(i.opcode());
  if (code == Opcode::Call || code == Opcode::Inline || code == Opcode::JCall) {
    auto &s = *i.operands()[0].get<MachineSymbolReference>()->symbol->signature;
    if (n - 2 < s.results.size()) return {s.results[n - 2], true};
    auto arg = n - 2 - s.results.size();
    if (arg < s.parameters.size()) return {s.parameters[arg], false};
    return {i.operands()[n].get<MachineMemory>()->type, false};
  }
  if (code == Opcode::Ret) return {f.signature().results[n], false};
  if (code == Opcode::Switch) return {Type::i64(), false};
  auto role = ir::instruction_descriptor(code)->roles[n];
  return {Type(role == 'F' || role == 'f' ? K::F32 : role == 'D' || role == 'd' ? K::F64
    : role == 'E' || role == 'e' ? K::LongDouble : K::I64), role >= 'A' && role <= 'Z'};
}
unsigned type_index(Type t) { return t.kind() == K::F32 ? 1 : t.kind() == K::F64 ? 2 : t.kind() == K::LongDouble ? 3 : 0; }
Opcode move(Type t) { return t.kind() == K::F32 ? Opcode::FMov : t.kind() == K::F64 ? Opcode::DMov : t.kind() == K::LongDouble ? Opcode::LDMov : Opcode::Mov; }
Result<void> reload(MachineRewriter &rw, MachineInstruction &i, MachineOperand memory, const MachineVirtualRegister &reg, ReloadType type) {
  std::vector<MachineOperand> operands; operands.reserve(2);
  if (type.output) { operands.push_back(std::move(memory)); operands.emplace_back(reg); }
  else { operands.emplace_back(reg); operands.push_back(std::move(memory)); }
  auto result = type.output ? rw.insert_after(i, move(type.type), std::move(operands)) : rw.insert_before(i, move(type.type), std::move(operands));
  if (!result) return result.error(); return {};
}
Result<void> inout_reloads(MachineFunction &f, MachineRewriter &rw, MachineInstruction &i) {
  auto type = reload_type(f, i, 0); type.type = f.target().scalar_type(type.type);
  auto reg = f.create_register(type.type); if (!reg) return reg.error();
  auto output = i.operands()[0], input = i.operands()[1];
  std::vector<MachineOperand> operands(i.operands().begin(), i.operands().end());
  operands[0] = operands[1] = MachineOperand(**reg);
  auto changed = rw.replace(i, i.opcode(), std::move(operands)); if (!changed) return changed.error();
  changed = reload(rw, i, std::move(input), **reg, {type.type, false}); if (!changed) return changed.error();
  return reload(rw, i, std::move(output), **reg, {type.type, true});
}
}
Result<void> legalize_operands(MachineFunction &f) {
  const auto x86 = f.target().architecture() == target::Architecture::X86_64;
  if (!x86 && f.target().architecture() != target::Architecture::AArch64) return invalid(f, "target is not migrated");
  if (!f.state().machinized) return invalid(f, "function has not been machinized");
  if (f.state().operands_legalized) return invalid(f, "operands have already been legalized");
  auto valid = f.verify(); if (!valid) return valid.error();
  auto instruction_ok = [&](MachineOpcode code, std::span<const MachineOperand> operands) {
    return x86 ? x86_instruction_ok(code, operands) : aarch64_instruction_ok(code, operands);
  };
  auto can_reload = [&](Type type) { return type.is_scalar() && (!x86 || type.kind() != K::LongDouble); };
  MachineRewriter rw(f); std::array<const MachineVirtualRegister *, 4> reusable{};
  for (auto &b : f.blocks()) for (auto at = b.instructions().begin(); at != b.instructions().end();) {
    auto &i = *at++; // Inserted reloads are deliberately not visited again.
    if (i.opcode() == MachineOpcode(MachinePseudoOpcode::Use)) continue;
    if (std::holds_alternative<MachinePseudoOpcode>(i.opcode())) {
      if (instruction_ok(i.opcode(), i.operands())) continue;
      return invalid(f, "private instruction has no target pattern");
    }
    auto code = std::get<Opcode>(i.opcode());
    if (code == Opcode::Label || code == Opcode::Addr || code == Opcode::Addr8 || code == Opcode::Addr16 || code == Opcode::Addr32) continue;
    const bool inout = x86 && x86_inout_opcode(code);
    if (inout && !x86_operands_equal(i.operands()[0], i.operands()[1])) {
      auto result = inout_reloads(f, rw, i); if (!result) return result.error();
    }
    if (instruction_ok(i.opcode(), i.operands())) continue;
    if (std::ranges::none_of(i.operands(), [](const auto &o) { return o.template get<MachineMemory>() != nullptr; })) continue;
    // Keep the original trial order and reuse failed trial temporaries only
    // for the same scalar type. Tied operands must be reloaded together.
    std::vector<MachineOperand> operands(i.operands().begin(), i.operands().end()); bool changed = false;
    for (std::size_t n = 0; n < operands.size(); ++n) {
      if (!operands[n].get<MachineMemory>()) continue;
      if (inout && n < 2) continue;
      auto type = reload_type(f, i, n); if (!can_reload(type.type)) continue;
      type.type = f.target().scalar_type(type.type); auto &reg = reusable[type_index(type.type)];
      if (!reg) { auto created = f.create_register(type.type); if (!created) return created.error(); reg = *created; }
      auto memory = operands[n]; operands[n] = MachineOperand(*reg);
      if (instruction_ok(i.opcode(), operands)) {
        auto result = rw.replace(i, i.opcode(), std::move(operands)); if (!result) return result.error();
        result = reload(rw, i, std::move(memory), *reg, type); if (!result) return result.error();
        reg = nullptr; changed = true; break;
      }
      operands[n] = std::move(memory);
    }
    if (changed) continue;
    if (inout && i.operands()[0].get<MachineMemory>()) {
      auto result = inout_reloads(f, rw, i); if (!result) return result.error();
      if (instruction_ok(i.opcode(), i.operands())) continue;
      operands.assign(i.operands().begin(), i.operands().end());
    }
    for (std::size_t n = 0; n < operands.size(); ++n) {
      if (!operands[n].get<MachineMemory>()) continue;
      if (inout && n < 2) continue;
      auto type = reload_type(f, i, n); if (!can_reload(type.type)) continue;
      type.type = f.target().scalar_type(type.type);
      auto reg = f.create_register(type.type); if (!reg) return reg.error();
      // Copy before replacing: the old C fallback aliases its overwritten
      // operand and emits a self move, losing the original memory access.
      auto memory = operands[n]; operands[n] = MachineOperand(**reg);
      auto result = reload(rw, i, std::move(memory), **reg, type); if (!result) return result.error();
      changed = true;
    }
    if (changed) { auto result = rw.replace(i, i.opcode(), std::move(operands)); if (!result) return result.error(); }
  }
  auto state = f.state(); state.operands_legalized = true;
  auto result = rw.set_state(state); if (!result) return result.error(); return f.verify();
}
}
