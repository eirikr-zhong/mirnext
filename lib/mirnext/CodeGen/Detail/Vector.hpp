/* Licensed under the MIT License. Portable SIMD preparation and shape selection. */
#pragma once
#include <map>
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen::detail {
inline unsigned vector_shape (ir::Type type) {
  if (!type.is_vector ()) return 0;
  if (type.vector_lane_type ().is_float ()) return type.vector_lanes () == 4 ? 5 : 6;
  return type.vector_lanes () == 16  ? 1
         : type.vector_lanes () == 8 ? 2
         : type.vector_lanes () == 4 ? 3
                                     : 4;
}
inline std::optional<ir::Type> vector_type (std::span<const MachineOperand> operands) {
  for (auto &operand : operands) {
    if (auto reg = operand_access (operand).get<const MachineVirtualRegister *> ();
        reg && (*reg)->type ().is_vector ())
      return (*reg)->type ();
    if (auto memory = operand_access (operand).get<MachineMemory> ();
        memory && memory->type.is_vector ())
      return memory->type;
  }
  return {};
}
inline unsigned vector_shape (std::span<const MachineOperand> operands) {
  auto type = vector_type (operands);
  return type ? vector_shape (*type) : 0;
}
using VectorConstantCache
  = std::map<std::pair<ir::Type, std::array<std::byte, 16>>, MachineOperand>;
Result<MachineOperand> prepare_vector_constant (MachineFunction &, MachineBasicBlock &,
                                                const ir::VectorConstant &, VectorConstantCache &);
Result<void> prepare_vector_operation (MachineFunction &, MachineBasicBlock &, ir::Opcode, ir::Type,
                                       std::span<const MachineOperand>, VectorConstantCache &,
                                       const ir::VectorConstant *shuffle_mask = nullptr);
}  // namespace mirnext::codegen::detail
