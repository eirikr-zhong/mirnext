/* Licensed under the MIT License. Native lazy entry dispatch and AArch64 emission. */
#include "mirnext/CodeGen/RuntimeEntry.hpp"
#include "mirnext/CodeGen/BlockEntry.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
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
  std::vector<std::uint8_t> finish () {
    std::vector<std::uint8_t> bytes;
    bytes.reserve (words.size () * 4);
    for (auto value : words)
      for (unsigned n = 0; n < 4; ++n) bytes.push_back (std::uint8_t (value >> (n * 8)));
    return bytes;
  }
};
Error invalid () {
  return {ErrorCode::InvalidArgument, "invalid AArch64 lazy entry target or addresses"};
}
}  // namespace
Result<std::vector<std::uint8_t>> make_lazy_entry (const target::TargetMachine &target,
                                                   LazyEntryAddresses addresses) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_lazy_entry (target, addresses);
  if (target.architecture () != target::Architecture::AArch64 || !addresses.destination
      || addresses.destination % alignof (std::uintptr_t) || !addresses.descriptor
      || !addresses.resolver || addresses.resolver % 4 || addresses.x87_results)
    return invalid ();
  Code code;
  // r9 is reserved by the MIR target. Warm calls only read the destination
  // slot and tail-branch; neither code protection nor the resolver is involved.
  const auto slot_load = code.words.size ();
  code.word (0);           // ldr x9,slot-address literal
  code.word (0xf9400129);  // ldr x9,[x9]
  const auto warm_branch = code.words.size ();
  code.word (0);  // cbnz x9,warm
  const auto descriptor_load = code.words.size ();
  code.word (0);
  const auto resolver_load = code.words.size ();
  code.word (0);
  code.word (0xd61f0120);
  code.words[warm_branch] = 0xb5000009 | std::uint32_t (code.words.size () - warm_branch) << 5;
  code.word (0xd61f0120);
  code.word (0xd503201f);  // br x9; align literals to eight bytes
  auto literal = [&] (std::size_t instruction, unsigned reg, std::uintptr_t value) {
    code.words[instruction]
      = 0x58000000 | std::uint32_t (code.words.size () - instruction) << 5 | reg;
    code.word (std::uint32_t (value));
    code.word (std::uint32_t (value >> 32));
  };
  literal (slot_load, 9, addresses.destination);
  literal (descriptor_load, 10, addresses.descriptor);
  literal (resolver_load, 9, addresses.resolver);
  // One cache line per entry; warm dispatch executes only four instructions.
  while (code.words.size () < lazy_entry_bytes / 4) code.word (0xd503201f);
  return code.finish ();
}
Result<std::vector<std::uint8_t>> make_lazy_resolver (const target::TargetMachine &target,
                                                      std::uintptr_t callback) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_lazy_resolver (target, callback);
  if (target.architecture () != target::Architecture::AArch64 || !callback) return invalid ();
  Code code;
  // Preserve every usable MIR register, not just the host argument registers:
  // bound caller-clobbered registers may carry values into a cold callee.
  // SP remains 16-byte aligned and stack/variadic arguments stay untouched.
  code.word (0xd10003ff | 784u << 10);  // sub sp,sp,#784
  for (unsigned n = 0; n < 31; ++n) code.word (0xf90003e0 | n << 10 | n);
  code.word (0xaa0a03e0);  // descriptor -> x0, before using reserved x10 as scratch
  // Compilation must be invisible to the callee's floating environment too.
  // Use the unused GP save slot and the next eight bytes, then align vectors.
  code.word (0xd53b440a);
  code.word (0xf90003ea | 31u << 10);  // mrs x10,fpcr; str x10,[sp,#248]
  code.word (0xd53b442a);
  code.word (0xf90003ea | 32u << 10);  // mrs x10,fpsr; str x10,[sp,#256]
  for (unsigned n = 0; n < 32; ++n) code.word (0x3d8003e0 | (17 + n) << 10 | n);
  code.address (10, callback);
  code.word (0xd63f0140);  // blr x10
  code.word (0xaa0003e9);  // mov x9,x0
  for (unsigned n = 0; n < 32; ++n) code.word (0x3dc003e0 | (17 + n) << 10 | n);
  code.word (0xf94003ea | 31u << 10);
  code.word (0xd51b440a);  // ldr x10,[sp,#248]; msr fpcr,x10
  code.word (0xf94003ea | 32u << 10);
  code.word (0xd51b442a);  // ldr x10,[sp,#256]; msr fpsr,x10
  for (unsigned n = 0; n < 31; ++n)
    if (n != 9) code.word (0xf94003e0 | n << 10 | n);
  code.word (0x910003ff | 784u << 10);  // add sp,sp,#784
  code.word (0xb4000049);
  code.word (0xd61f0120);  // cbz x9,failure; br x9
  // The resolver has reported a typed diagnostic to ExecutionInvocation. Return
  // deterministic zero result registers without entering an unpublished body.
  for (unsigned n = 0; n < 8; ++n) {
    code.word (0xd2800000 | n);
    code.word (0x4f00e400 | n);
  }
  code.word (0xd65f03c0);
  return code.finish ();
}
Result<std::vector<std::uint8_t>> make_block_entry (const target::TargetMachine &target,
                                                    BlockEntryAddresses a) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_block_entry (target, a);
  if (target.architecture () != target::Architecture::AArch64 || !a.destination
      || a.destination % alignof (std::uintptr_t) || !a.descriptor || !a.resolver || a.resolver % 4)
    return Error{ErrorCode::InvalidArgument, "invalid AArch64 block entry target or addresses"};
  Code c;
  // Both paths leave SP, NZCV and usable registers untouched. The initial slot
  // points at the cold stub; no null test or mutable instruction is necessary.
  c.word (0x58000109);
  c.word (0xf9400129);
  c.word (0xd61f0120);
  c.word (0xd503201f);
  c.word (0x580000ca);
  c.word (0x580000e9);
  c.word (0xd61f0120);
  c.word (0xd503201f);
  for (auto value : {a.destination, a.descriptor, a.resolver}) {
    c.word (std::uint32_t (value));
    c.word (std::uint32_t (value >> 32));
  }
  while (c.words.size () < block_entry_bytes / 4) c.word (0xd503201f);
  return c.finish ();
}
Result<std::vector<std::uint8_t>> make_block_resolver (const target::TargetMachine &target,
                                                       std::uintptr_t callback,
                                                       std::uintptr_t failure) {
  if (target.architecture () == target::Architecture::X86_64)
    return detail::make_x86_block_resolver (target, callback, failure);
  if (target.architecture () != target::Architecture::AArch64 || !callback || callback % 4
      || !failure || failure % 4)
    return Error{ErrorCode::InvalidArgument,
                 "invalid AArch64 block resolver target callback or failure continuation"};
  Code c;
  c.word (0xd10003ff | 784u << 10);
  for (unsigned n = 0; n < 31; ++n) c.word (0xf90003e0 | n << 10 | n);
  c.word (0xaa0a03e0);  // descriptor -> x0
  c.word (0xd53b440a);
  c.word (0xf90003ea | 31u << 10);  // FPCR
  c.word (0xd53b442a);
  c.word (0xf90003ea | 32u << 10);  // FPSR
  c.word (0xd53b420a);
  c.word (0xf90003ea | 33u << 10);  // NZCV
  for (unsigned n = 0; n < 32; ++n) c.word (0x3d8003e0 | (17 + n) << 10 | n);
  c.address (10, callback);
  c.word (0xd63f0140);
  c.word (0xaa0003e9);
  c.word (0xb50000a9);  // cbnz x9,+20: skip four failure address chunks
  c.address (9, failure);
  for (unsigned n = 0; n < 32; ++n) c.word (0x3dc003e0 | (17 + n) << 10 | n);
  c.word (0xf94003ea | 31u << 10);
  c.word (0xd51b440a);
  c.word (0xf94003ea | 32u << 10);
  c.word (0xd51b442a);
  c.word (0xf94003ea | 33u << 10);
  c.word (0xd51b420a);
  for (unsigned n = 0; n < 31; ++n)
    if (n != 9) c.word (0xf94003e0 | n << 10 | n);
  c.word (0x910003ff | 784u << 10);
  c.word (0xd61f0120);
  return c.finish ();
}
}  // namespace mirnext::codegen
