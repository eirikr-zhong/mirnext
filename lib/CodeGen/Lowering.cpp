/* Licensed under the MIT License. Owning target lowering dispatch. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/TargetLowering.hpp"
namespace mirnext::codegen {
Result<void> machinize (MachineFunction &f) {
  switch (f.target ().architecture ()) {
  case target::Architecture::AArch64: return machinize_aarch64 (f);
  case target::Architecture::X86_64: return machinize_x86 (f);
  default: return Error{ErrorCode::InvalidArgument, "unsupported machine lowering target"};
  }
}
}  // namespace mirnext::codegen
