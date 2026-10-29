/* Licensed under the MIT License. Shared native vector roles and constraints. */
#pragma once
#include "mirnext/CodeGen/Detail/Vector.hpp"
namespace mirnext::codegen::detail {
struct NativeVectorDescriptor {
  MachinePseudoOpcode opcode;
  std::string_view name, roles;
  target::Architecture architecture;
  bool tied;
  unsigned maximum;
};
inline const NativeVectorDescriptor *native_vector_descriptor (MachineOpcode code) {
  auto op = std::get_if<MachinePseudoOpcode> (&code);
  if (!op || *op <= MachinePseudoOpcode::X86Mov128Unaligned) return nullptr;
  static constexpr NativeVectorDescriptor descriptors[]{
#define MIRNEXT_NATIVE_X86(name, roles, tied, maximum, constraints, encoding) \
  {MachinePseudoOpcode::name, #name, roles, target::Architecture::X86_64, tied, maximum},
#define MIRNEXT_NATIVE_A64(name, roles, tied, maximum, constraints, encoding) \
  {MachinePseudoOpcode::name, #name, roles, target::Architecture::AArch64, tied, maximum},
#include "mirnext/Target/NativeVectorOps.def"
#undef MIRNEXT_NATIVE_A64
#undef MIRNEXT_NATIVE_X86
  };
  auto n = static_cast<unsigned> (*op)
           - static_cast<unsigned> (MachinePseudoOpcode::X86Mov128Unaligned) - 1;
  return n < std::size (descriptors) ? &descriptors[n] : nullptr;
}
inline std::optional<std::uint64_t> native_integer (const MachineOperand &o) {
  if (auto n = operand_access (o).get<std::uint64_t> ()) return *n;
  if (auto n = operand_access (o).get<std::int64_t> (); n && *n >= 0) return *n;
  return {};
}
inline ir::Type native_operand_type (const MachineOperand &o, char role) {
  if (auto r = operand_access (o).get<const MachineVirtualRegister *> ()) return (*r)->type ();
  return role == 'V' || role == 'v' || role == 'B' || role == 'b'
           ? ir::Type::vector128 (ir::Type::VectorElement::U8, 16)
           : ir::Type::i64 ();
}
inline bool native_vector_operands (const NativeVectorDescriptor &d,
                                    std::span<const MachineOperand> ops,
                                    const target::TargetMachine *target = nullptr) {
  if (ops.size () != d.roles.size () || (target && target->architecture () != d.architecture))
    return false;
  if (d.tied && ops[0] != ops[1]) return false;
  for (unsigned n = 0; n < ops.size (); ++n) {
    char role = d.roles[n];
    if (role == 'n') {
      auto value = native_integer (ops[n]);
      if (!value || *value > d.maximum) return false;
      if (d.opcode == MachinePseudoOpcode::A64VectorExtractSigned && (*value & 7) == 0)
        return false;
      if ((d.opcode == MachinePseudoOpcode::A64VectorDupGP && *value != 1 && *value != 2
           && *value != 4 && *value != 8)
          || ((d.opcode == MachinePseudoOpcode::A64VectorDupLane
               || d.opcode == MachinePseudoOpcode::A64VectorExtract
               || d.opcode == MachinePseudoOpcode::A64VectorExtractSigned
               || d.opcode == MachinePseudoOpcode::A64VectorInsertGP
               || d.opcode == MachinePseudoOpcode::A64VectorInsertFP)
              && (!*value || (*value & 15) == 0)))
        return false;
      continue;
    }
    bool integer = role == 'I' || role == 'i';
    if (auto r = operand_access (ops[n]).get<const MachineVirtualRegister *> ()) {
      if (!*r
          || (integer                        ? !(*r)->type ().is_integer ()
              : (role == 'B' || role == 'b') ? !(((*r)->type ().kind () == ir::Type::Kind::F32
                                                  || (*r)->type ().kind () == ir::Type::Kind::F64)
                                                 || (*r)->type ().is_vector ())
                                             : !(*r)->type ().is_vector ()))
        return false;
    } else if (auto r = operand_access (ops[n]).get<target::PhysicalRegister> ()) {
      if (r->architecture () != d.architecture
          || (integer && d.architecture == target::Architecture::AArch64 && r->encoding () == 31))
        return false;
      unsigned boundary = d.architecture == target::Architecture::AArch64 ? 32 : 16;
      if (r->encoding () >= boundary * 2 || (r->encoding () < boundary) != integer) return false;
      if (target && !target->register_info (*r).has_value ()) return false;
    } else
      return false;
  }
  return true;
}
}  // namespace mirnext::codegen::detail
