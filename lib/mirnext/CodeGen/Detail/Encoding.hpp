/* Licensed under the MIT License. Native target encoding boundaries. */
#pragma once
#include "mirnext/CodeGen/CompiledFunction.hpp"
namespace mirnext::codegen::detail {
Result<CompiledFunction> encode_aarch64 (const MachineFunction &, const MachineFrameResult &,
                                         MachineEncodingOptions);
Result<CompiledFunction> encode_x86 (const MachineFunction &, const MachineFrameResult &,
                                     MachineEncodingOptions);
}  // namespace mirnext::codegen::detail
