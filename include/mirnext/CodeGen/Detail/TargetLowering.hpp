/* Licensed under the MIT License. Private owning target dispatch. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen {
Result<void> machinize_aarch64(MachineFunction &);
Result<void> machinize_x86(MachineFunction &);
}
