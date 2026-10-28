/* Licensed under the MIT License. Private checked machine rewrites. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen {
class MachineRewriter {
 public:
  explicit MachineRewriter (MachineFunction &function) : function_ (function) {}
  Result<MachineInstruction &> append (MachineBasicBlock &, MachineOpcode,
                                       std::vector<MachineOperand>);
  Result<MachineInstruction &> insert_before (MachineInstruction &, MachineOpcode,
                                              std::vector<MachineOperand>);
  Result<MachineInstruction &> insert_after (MachineInstruction &, MachineOpcode,
                                             std::vector<MachineOperand>);
  Result<OptionalRef<MachineInstruction>> previous (MachineInstruction &) const;
  Result<OptionalRef<MachineInstruction>> next (MachineInstruction &) const;
  Result<void> replace (MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<void> erase (MachineInstruction &);
  Result<void> move_before (MachineInstruction &, MachineInstruction &);
  Result<void> move_to (MachineInstruction &, MachineBasicBlock &, bool at_end = true);
  Result<void> move_label (MachineLabel &, MachineBasicBlock &);
  Result<void> erase_label (MachineLabel &);
  Result<MachineBasicBlock &> create_block_before (MachineBasicBlock &);
  Result<MachineBasicBlock &> create_block_after (MachineBasicBlock &);
  Result<void> set_call_arguments (MachineInstruction &, std::span<const target::PhysicalRegister>);
  // Dependencies are attached after allocation; they cannot enter coalescing
  // or be mistaken for executable virtual addresses.
  Result<void> set_call_dependencies (MachineInstruction &,
                                      std::span<const CallArgumentDependency>);
  // Atomic replacement of an ABI-lowered call. Its block operands describe
  // physical argument locations, rather than public IR block addresses.
  Result<void> lower_call (MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<void> set_state (MachineFunctionState);
  Result<void> set_global_registers (std::span<const target::PhysicalRegister>);
  Result<void> set_entry_setup (MachineInstruction &, bool = true);

 private:
  friend class MachineFunction;
  Result<void> validate (MachineOpcode, std::span<const MachineOperand>,
                         bool lowered_call = false) const;
  Result<void> validate_dependencies (MachineOpcode, std::span<const MachineOperand>,
                                      std::span<const CallArgumentDependency>) const;
  MachineFunction &function_;
};
}  // namespace mirnext::codegen
