/* Licensed under the MIT License. Private checked compilation primitives. */
#pragma once
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
namespace mirnext::codegen {
Result<MachineCoalescing> coalesce_registers (MachineFunction &, const MachineGraphAnalysis &,
                                              unsigned optimization_level = 2,
                                              std::size_t register_limit = 10000);
Result<CompiledFunction> encode_machine (const MachineFunction &, const MachineFrameResult &,
                                         MachineEncodingOptions = {});
Result<MachineRewriteResult> rewrite_allocated (MachineFunction &, const MachineGraphAnalysis &,
                                                const MachineLiveness &, const MachineLiveRanges &,
                                                MachineAllocation);
Result<MachineEdgeSplit> split_machine_edge (MachineFunction &, const MachineControlFlow &,
                                             std::size_t edge);
Result<MachineSpillResult> split_allocated_spills (MachineFunction &, MachineRewriteResult);
Result<MachineFrameResult> form_frame (MachineFunction &, MachinePostResult,
                                       MachineFrameOptions = {});
Result<MachinePostResult> optimize_allocated_machine (MachineFunction &, MachineSpillResult,
                                                      MachinePostOptions = {});
Result<void> machinize (MachineFunction &);
Result<void> legalize_operands (MachineFunction &);
Result<MachineFunction> prepare_machine_ir (const ir::Function &, const target::TargetMachine &);
}  // namespace mirnext::codegen
