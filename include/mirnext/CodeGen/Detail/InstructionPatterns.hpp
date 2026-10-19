/* Licensed under the MIT License. Target selection for owning machine passes. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen::detail {
bool instruction_pattern_matches(const target::TargetMachine &, MachineOpcode, std::span<const MachineOperand>);
}
