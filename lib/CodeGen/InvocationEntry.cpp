/* Licensed under the MIT License. AArch64 invocation checkpoints. */
#include "mirnext/CodeGen/InvocationEntry.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
namespace mirnext::codegen {
namespace {
struct Code {
  std::vector<std::uint8_t> bytes;
  void word(std::uint32_t value) { for (unsigned n = 0; n < 4; ++n) bytes.push_back(std::uint8_t(value>>(n*8))); }
  void address(unsigned reg,std::uintptr_t value) {
    constexpr std::uint32_t moves[]{0xd2800000,0xf2a00000,0xf2c00000,0xf2e00000};
    for (unsigned n = 0; n < 4; ++n) word(moves[n] | std::uint32_t((value>>(n*16))&0xffff)<<5 | reg);
  }
  void gp(bool load,unsigned reg,std::size_t offset) { word((load ? 0xf9400120 : 0xf9000120) | std::uint32_t(offset/8)<<10 | reg); }
  void vector(bool load,unsigned reg) { word((load ? 0x3dc00120 : 0x3d800120) | (16+reg)<<10 | reg); }
  void finish(bool failed) {
    for (unsigned n = 19; n < 30; ++n) gp(true,n,n*8);
    for (unsigned n = failed ? 0 : 8; n < (failed ? 32u : 16u); ++n) vector(true,n);
    if (failed) {
      gp(true,10,784); word(0xd51b440a); gp(true,10,792); word(0xd51b442a);
      word(0xd2800000); word(0xd2800001); // deterministic ignored integer results
      word(0x4f00e400); word(0x4f00e401);
    }
    gp(true,30,776); gp(true,10,768); word(0x9100015f); word(0xd65f03c0); // mov sp,x10; ret
  }
};
}
Result<InvocationEntryCode> make_invocation_entries(const target::TargetMachine &target,std::uintptr_t callback) {
  if (target.architecture() == target::Architecture::X86_64) return detail::make_x86_invocation_entries(target,callback);
  if (target.architecture() != target::Architecture::AArch64 || !callback || callback%4)
    return Error{ErrorCode::InvalidArgument,"invalid AArch64 invocation checkpoint callback"};
  Code call; call.bytes.reserve(384); call.word(0xaa0303e9); // state x3 -> reserved x9
  for (unsigned n = 19; n < 30; ++n) call.gp(false,n,n*8);
  call.gp(false,30,776);
  for (unsigned n = 0; n < 32; ++n) call.vector(false,n);
  call.word(0xd53b440a); call.gp(false,10,784); call.word(0xd53b442a); call.gp(false,10,792);
  call.word(0x910003ea); call.gp(false,10,768); // mov x10,sp; save original host SP
  // Keep the state pointer outside the called adapter's stack and register
  // allocation. MIR hardware bindings may overwrite ordinary callee saves.
  call.word(0xd10043ff); call.word(0xf90003e9); // sub sp,sp,16; str x9,[sp]
  call.word(0xaa0003ea); call.word(0xaa0103e0); call.word(0xaa0203e1); call.word(0xd63f0140);
  call.word(0xf94003e9); call.finish(false);
  Code recovery; recovery.address(9,callback); recovery.word(0xd63f0120); recovery.word(0xaa0003e9);
  // Missing activation is an internal ABI contract violation, not a runtime
  // compilation error. Do not dereference a null checkpoint or RET mid-frame.
  recovery.word(0xb5000049); recovery.word(0xd4200000); // cbnz x9,+8; brk
  recovery.finish(true);
  return InvocationEntryCode{std::move(call.bytes),std::move(recovery.bytes)};
}
}
