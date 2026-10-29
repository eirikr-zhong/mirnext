/* Licensed under the MIT License. Internal target selection over typed operands. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen {
bool aarch64_instruction_ok (MachineOpcode, std::span<const MachineOperand>);
enum class AArch64Overflow { Default, Subtract, Multiply };
enum class AArch64FieldKind : std::uint8_t {
  Register,
  BitImmediate,
  IndexedMemory,
  DisplacedMemory,
  RightShift,
  LeftShift,
  WideImmediate,
  ArithmeticImmediate,
  ImmediateR,
  ImmediateS,
  ShortLabel,
  Label,
  SwitchTable
};
struct AArch64Field {
  AArch64FieldKind kind;
  std::uint8_t parameter = 0, shift = 0, flags = 0;
};
struct AArch64Word {
  std::uint32_t opcode = 0, mask = UINT32_MAX;
  std::size_t first_field = 0, field_count = 0;
};
struct AArch64Recipe {
  std::span<const AArch64Word> words;
  std::span<const AArch64Field> fields;
};
// Static, densely compiled encoding fields from the same declaration table as
// legalization. No template-string parsing occurs in the encoder.
std::optional<AArch64Recipe> aarch64_encoding_recipe (MachineOpcode,
                                                      std::span<const MachineOperand>,
                                                      AArch64Overflow = AArch64Overflow::Default,
                                                      unsigned vector_shape = 0);
Result<std::uint32_t> aarch64_branch_displacement (std::int64_t, bool short_branch);
}  // namespace mirnext::codegen
