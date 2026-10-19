/* Licensed under the MIT License. Allocation-free operand occurrence traversal. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen::detail {
inline bool is_call(MachineOpcode code) {
  auto op = std::get_if<ir::Opcode>(&code);
  return op && (*op == ir::Opcode::Call || *op == ir::Opcode::Inline || *op == ir::Opcode::JCall);
}
inline bool operand_is_output(const MachineInstruction &i, std::size_t p) {
  auto code = i.opcode();
  if (auto pseudo = std::get_if<MachinePseudoOpcode>(&code)) return (*pseudo == MachinePseudoOpcode::X86Mov128 || *pseudo == MachinePseudoOpcode::X86Mov128Unaligned) && p == 0;
  auto op = std::get<ir::Opcode>(code);
  // Retain the original allocator descriptor, including historical LADDR uses.
  if (op == ir::Opcode::LAddr) return false;
  if (is_call(op)) return p >= 2 && p - 2 < i.operands()[0].get<MachineSymbolReference>()->symbol->signature->results.size();
  auto d = ir::instruction_descriptor(op);
  return d->count == ir::OperandCount::Fixed && d->roles[p] >= 'A' && d->roles[p] <= 'Z';
}
template<class Visitor> void visit_registers(const MachineInstruction &i, bool definitions, Visitor visit) {
  auto dependencies = i.call_dependencies(); auto dependency = dependencies.begin();
  for (std::size_t p = 0; p < i.operands().size(); ++p) {
    const auto &o = i.operands()[p];
    if (auto r = o.get<const MachineVirtualRegister *>(); r && operand_is_output(i, p) == definitions) visit(MachineRegister(*r));
    else if (auto r = o.get<target::PhysicalRegister>(); r && operand_is_output(i, p) == definitions) visit(MachineRegister(*r));
    else if (!definitions) if (auto m = o.get<MachineMemory>()) { if (m->base) visit(*m->base); if (m->index) visit(*m->index); }
    if (!definitions && dependency != dependencies.end() && dependency->operand == p) visit(MachineRegister((dependency++)->value));
  }
}
}
