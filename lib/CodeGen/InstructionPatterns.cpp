/* Licensed under the MIT License. Selection dispatch over owning machine IR. */
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/InstructionPatterns.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"

namespace mirnext::codegen {
bool detail::instruction_pattern_matches (const target::TargetMachine &target, MachineOpcode code,
                                          std::span<const MachineOperand> ops) {
  switch (target.architecture ()) {
  case target::Architecture::X86_64: return x86_instruction_ok (code, ops);
  case target::Architecture::AArch64: return aarch64_instruction_ok (code, ops);
  default: return false;
  }
}
Result<bool> has_instruction_pattern (const MachineInstruction &i) {
  auto &f = i.block ().function ();
  switch (f.target ().architecture ()) {
  case target::Architecture::X86_64:
  case target::Architecture::AArch64:
    return detail::instruction_pattern_matches (f.target (), i.opcode (), i.operands ());
  default:
    return Error{ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/"
                                              + std::string (f.name ()) + " ["
                                              + std::string (f.target ().name ())
                                              + ", instruction-pattern]: unsupported target"};
  }
}
}  // namespace mirnext::codegen
