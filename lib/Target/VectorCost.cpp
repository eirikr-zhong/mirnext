/* Licensed under the MIT License. Costs follow NEON and SSE2 lowering sequences. */
#include "mirnext/Target/VectorCost.hpp"
#include "mirnext/IR/Vector.hpp"
namespace mirnext::target {
Result<VectorCostModel> VectorCostModel::get (Architecture architecture) {
  auto rules = AddressingRules::get (architecture);
  if (!rules.has_value ()) return rules.error ();
  return VectorCostModel (rules.value ().architecture ());
}
std::optional<unsigned> VectorCostModel::operation (ir::Opcode op, ir::Type type) const {
  using ir::Opcode;
  std::array inputs{type, type};
  if (!type.is_vector () || !ir::detail::vector_types (op, type, inputs)) return {};
  switch (op) {
  case Opcode::VMov:
  case Opcode::VAdd:
  case Opcode::VSub:
  case Opcode::VAnd:
  case Opcode::VOr:
  case Opcode::VXor: return 1;
  case Opcode::VNeg:
    return architecture_ == Architecture::AArch64 ? 1
           : type.vector_lane_type ().is_float () ? 5
                                                  : 2;
  case Opcode::VDiv: return 8;
  case Opcode::VMul:
    if (type.vector_lane_type ().is_float ()) return 1;
    if (type.vector_lanes () == 2) return architecture_ == Architecture::AArch64 ? 9 : 10;
    if (type.vector_lanes () == 4 && architecture_ == Architecture::X86_64) return 6;
    return 1;
  default: return {};
  }
}
unsigned VectorCostModel::splat (ir::Type type) const {
  return architecture_ == Architecture::AArch64 ? 1 : type.vector_lanes () >= 8 ? 4 : 2;
}
unsigned VectorCostModel::scalar (ir::Opcode op) const {
  using ir::Opcode;
  switch (op) {
  case Opcode::Mul:
  case Opcode::Muls: return 2;
  case Opcode::FDiv:
  case Opcode::DDiv: return 8;
  default: return 1;
  }
}
}  // namespace mirnext::target
