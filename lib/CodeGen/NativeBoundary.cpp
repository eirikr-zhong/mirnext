/* Licensed under the MIT License. AArch64 signature-independent ABI boundary. */
#include "mirnext/CodeGen/NativeBoundary.hpp"
namespace mirnext::codegen {
namespace {
struct Code {
  std::vector<std::uint32_t> words;
  void word (std::uint32_t value) { words.push_back (value); }
  void address (unsigned reg, std::uintptr_t value) {
    constexpr std::uint32_t moves[]{0xd2800000, 0xf2a00000, 0xf2c00000, 0xf2e00000};
    for (unsigned n = 0; n < 4; ++n)
      word (moves[n] | std::uint32_t ((value >> (n * 16)) & 0xffff) << 5 | reg);
  }
  void stack_gp (bool load, unsigned reg, std::size_t offset) {
    word ((load ? 0xf94003e0 : 0xf90003e0) | std::uint32_t (offset / 8) << 10 | reg);
  }
  void state_gp (unsigned reg, std::size_t offset) {
    word (0xf9400120 | std::uint32_t (offset / 8) << 10 | reg);
  }
  std::vector<std::uint8_t> finish () {
    std::vector<std::uint8_t> bytes;
    bytes.reserve (words.size () * 4);
    for (auto value : words)
      for (unsigned n = 0; n < 4; ++n) bytes.push_back (std::uint8_t (value >> (n * 8)));
    return bytes;
  }
};
Error invalid () {
  return {ErrorCode::InvalidArgument, "invalid AArch64 native boundary callback or entry"};
}
}  // namespace
Result<std::vector<std::uint8_t>> make_native_boundary_gate (const target::TargetMachine &target,
                                                             std::uintptr_t descriptor,
                                                             std::uintptr_t enter) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_native_boundary_gate (target, descriptor, enter);
  if (target.architecture () != target::Architecture::AArch64 || !descriptor || !enter || enter % 4)
    return invalid ();
  Code c;
  c.word (0x5800008a);
  c.word (0x580000a9);
  c.word (0xd61f0120);
  c.word (0xd503201f);
  for (auto value : {descriptor, enter}) {
    c.word (std::uint32_t (value));
    c.word (std::uint32_t (value >> 32));
  }
  return c.finish ();
}
Result<NativeBoundaryCode> make_native_boundary_entries (const target::TargetMachine &target,
                                                         std::uintptr_t enter,
                                                         std::uintptr_t leave) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_native_boundary_entries (target, enter, leave);
  if (target.architecture () != target::Architecture::AArch64 || !enter || enter % 4 || !leave
      || leave % 4)
    return invalid ();
  constexpr unsigned frame = sizeof (NativeInvocationState);
  static_assert (frame % 16 == 0 && frame < 4096);
  Code c;
  c.word (0xd10003ff | frame << 10);
  for (unsigned n = 0; n < 31; ++n) c.stack_gp (false, n, n * 8);
  // GP31 has no architectural value; use its slot for incoming NZCV.
  c.word (0xd53b420a);
  c.stack_gp (false, 10, 248);
  for (unsigned n = 0; n < 32; ++n) c.word (0x3d8003e0 | (16 + n) << 10 | n);
  c.word (0x910003ea | frame << 10);
  c.stack_gp (false, 10, 768);
  c.stack_gp (false, 30, 776);
  c.word (0xd53b440a);
  c.stack_gp (false, 10, 784);
  c.word (0xd53b442a);
  c.stack_gp (false, 10, 792);
  c.stack_gp (false, 31, 1312);
  c.word (0x910003e0);
  c.stack_gp (true, 1, 80);
  c.address (10, enter);
  c.word (0xd63f0140);
  c.word (0xaa0003e9);
  c.word (0xb5000049);
  c.word (0xd4200000);  // owned checkpoint is mandatory
  for (unsigned n = 0; n < 32; ++n) c.word (0x3dc00120 | (16 + n) << 10 | n);
  c.state_gp (10, 784);
  c.word (0xd51b440a);
  c.state_gp (10, 792);
  c.word (0xd51b442a);
  c.state_gp (10, 248);
  c.word (0xd51b420a);
  for (unsigned n = 0; n < 30; ++n)
    if (n != 9 && n != 10) c.state_gp (n, n * 8);
  c.state_gp (30, 776);
  c.state_gp (10, 768);
  c.word (0x9100015f);  // rewritten LR; original argument stack
  c.state_gp (10, 80);
  c.word (0xd61f0140);  // target in reserved x10
  Code out;
  out.word (0xd10003ff | 784u << 10);
  for (unsigned n = 0; n < 31; ++n) out.stack_gp (false, n, n * 8);
  out.word (0xd53b440a);
  out.stack_gp (false, 10, 248);
  out.word (0xd53b442a);
  out.stack_gp (false, 10, 256);
  out.word (0xd53b420a);
  out.stack_gp (false, 10, 264);
  for (unsigned n = 0; n < 32; ++n) out.word (0x3d8003e0 | (17 + n) << 10 | n);
  out.address (10, leave);
  out.word (0xd63f0140);
  out.word (0xaa0003e9);
  out.word (0xb5000049);
  out.word (0xd4200000);
  for (unsigned n = 0; n < 32; ++n) out.word (0x3dc003e0 | (17 + n) << 10 | n);
  out.stack_gp (true, 10, 248);
  out.word (0xd51b440a);
  out.stack_gp (true, 10, 256);
  out.word (0xd51b442a);
  out.stack_gp (true, 10, 264);
  out.word (0xd51b420a);
  for (unsigned n = 0; n < 31; ++n)
    if (n != 9) out.stack_gp (true, n, n * 8);
  out.word (0x910003ff | 784u << 10);
  out.word (0xd61f0120);
  return NativeBoundaryCode{c.finish (), out.finish ()};
}
}  // namespace mirnext::codegen
