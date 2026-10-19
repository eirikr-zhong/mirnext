/* Licensed under the MIT License. Target lowering over owning machine nodes. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"

namespace mirnext::codegen {
// Rewrites a prepared machine function in place. No legacy model or runtime
// participates. A target without a migrated implementation is diagnosed.
Result<void> machinize(MachineFunction &);
// Match the target's ordered instruction constraints without assigning physical
// registers or resolving relocations. False means no direct encoding pattern;
// bookkeeping nodes may still be retained. An unavailable target is diagnosed.
Result<bool> has_instruction_pattern(const MachineInstruction &);
// Legalize a machinized function in place, preserving original reload order.
Result<void> legalize_operands(MachineFunction &);
}
