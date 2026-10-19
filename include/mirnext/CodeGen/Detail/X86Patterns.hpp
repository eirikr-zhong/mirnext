/* Licensed under the MIT License. Ordered x86 constraints over owning operands. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"

namespace mirnext::codegen {
struct X86BranchRange {
  // Label offset minus instruction offset, before choosing the short encoding.
  std::int64_t displacement;
  std::size_t maximum_instruction_size;
};
struct X86PatternOptions {
  bool allow_short_branches = true;
  std::optional<X86BranchRange> branch_range;
  // Use each candidate's precompiled maximum, rather than a supplied test bound.
  bool encoding_layout = false;
};
struct X86InstructionPattern {
  std::size_t index;
  std::string_view constraints, encoding;
  MachineOpcode opcode;
};
std::span<const X86InstructionPattern> x86_instruction_patterns();
// Target-independent selection does not enable cross-target code generation.
// Relocations remain symbolic and only match a full-width immediate.
std::optional<X86InstructionPattern> x86_instruction_pattern(MachineOpcode,
  std::span<const MachineOperand>, X86PatternOptions = {});
bool x86_instruction_ok(MachineOpcode, std::span<const MachineOperand>);
bool x86_inout_opcode(ir::Opcode);
bool x86_operands_equal(const MachineOperand &, const MachineOperand &);
}
