/* Licensed under the MIT License. Bit-level native checkpoint ABI probes. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/InvocationEntry.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Execution/detail/NativeBoundary.hpp"
#include <cfenv>
using namespace mirnext;
using namespace mirnext::codegen;
using namespace mirnext::execution;
namespace {
struct Probe {
  NativeInvocationState original, before, after, checkpoint;
  std::uint64_t result = UINT64_MAX;
};
thread_local NativeInvocationState *active = nullptr;
std::uintptr_t checkpoint () noexcept {
  std::fesetround (FE_DOWNWARD);
  std::feraiseexcept (FE_INEXACT);
  return reinterpret_cast<std::uintptr_t> (active);
}
std::vector<std::uint8_t> arm_probe (Probe &probe, std::uintptr_t entry, std::uintptr_t recovery,
                                     bool fail, unsigned extra, bool raw = false,
                                     std::size_t *target_offset = nullptr) {
  struct Code {
    std::vector<std::uint32_t> words;
    void word (std::uint32_t value) { words.push_back (value); }
    void address (unsigned r, std::uint64_t value) {
      constexpr std::uint32_t moves[]{0xd2800000, 0xf2a00000, 0xf2c00000, 0xf2e00000};
      for (unsigned n = 0; n < 4; ++n)
        word (moves[n] | std::uint32_t ((value >> (n * 16)) & 65535) << 5 | r);
    }
    void snapshot (NativeInvocationState &s, bool restore = false) {
      address (9, reinterpret_cast<std::uintptr_t> (&s));
      for (unsigned n = 19; n < 31; ++n) word ((restore ? 0xf9400120 : 0xf9000120) | n << 10 | n);
      for (unsigned n = 0; n < 32; ++n)
        word ((restore ? 0x3dc00120 : 0x3d800120) | (16 + n) << 10 | n);
      if (!restore) {
        word (0x910003ea);
        word (0xf900012a | 96u << 10);
        word (0xd53b440a);
        word (0xf900012a | 98u << 10);
        word (0xd53b442a);
        word (0xf900012a | 99u << 10);
      }
    }
    std::vector<std::uint8_t> finish () {
      std::vector<std::uint8_t> bytes;
      for (auto w : words)
        for (unsigned n = 0; n < 4; ++n) bytes.push_back (std::uint8_t (w >> (n * 8)));
      return bytes;
    }
  } c;
  c.snapshot (probe.original);
  for (unsigned n = 19; n < 29; ++n) c.address (n, 0x123456789abc0000ull + n);
  for (unsigned n = 8; n < 16; ++n) c.word (0x4f00e400 | (n + 1) << 5 | n);
  c.snapshot (probe.before);
  // ADR x0,target below; invoke takes two unused pointer arguments and state.
  auto target = c.words.size ();
  c.word (0);
  c.address (3, reinterpret_cast<std::uintptr_t> (&probe.checkpoint));
  c.address (9, entry);
  c.word (0xd63f0120);
  c.address (9, reinterpret_cast<std::uintptr_t> (&probe.result));
  c.word (0xf9000120);
  c.snapshot (probe.after);
  c.snapshot (probe.original, true);
  c.word (0xd65f03c0);
  c.words[target]
    = 0x10000000 | std::uint32_t (c.words.size () - target) << 5;  // aligned ADR's immlo is zero
  if (target_offset) *target_offset = c.words.size () * 4;
  for (unsigned n = 19; n < (raw ? 29u : 30u); ++n) c.address (n, 0xdeadbeef00000000ull + n);
  for (unsigned n = 8; n < 16; ++n) c.word (0x4f00e400 | n);
  if (fail) {
    if (extra) c.word (0xd10003ff | extra << 10);
    c.address (9, recovery);
    c.word (0xd61f0120);
  } else {
    c.address (0, 42);
    c.word (0xd65f03c0);
  }
  return c.finish ();
}
std::vector<std::uint8_t> x86_probe (const target::TargetMachine &target, Probe &probe,
                                     std::uintptr_t entry, std::uintptr_t recovery, bool fail,
                                     unsigned extra, bool raw = false,
                                     std::size_t *target_offset = nullptr) {
  auto layout = codegen::detail::x86_invocation_frame (target.abi ());
  REQUIRE ((layout).has_value ());
  auto &f = (layout).value ();
  struct Code {
    std::vector<std::uint8_t> bytes;
    void emit (std::initializer_list<std::uint8_t> v) { bytes.insert (bytes.end (), v); }
    void integer (std::uint64_t v, unsigned size) {
      for (unsigned n = 0; n < size; ++n) bytes.push_back (std::uint8_t (v >> (n * 8)));
    }
    void address (unsigned r, std::uint64_t v) {
      emit ({std::uint8_t (0x48 | (r >> 3)), std::uint8_t (0xb8 | (r & 7))});
      integer (v, 8);
    }
    void memory (std::uint8_t op, unsigned r, std::size_t offset, bool vector = false) {
      if (vector) emit ({0xf3});
      emit ({std::uint8_t ((vector ? 0x41 : 0x49) | ((r >> 3) << 2))});
      if (vector) emit ({0x0f});
      emit ({op, std::uint8_t (0x83 | ((r & 7) << 3))});
      integer (offset, 4);
    }
    void stack (bool restore, std::size_t size) {
      emit ({0x48, 0x81, std::uint8_t (restore ? 0xc4 : 0xec)});
      integer (size, 4);
    }
    void snapshot (NativeInvocationState &s, const codegen::detail::X86InvocationFrame &f,
                   bool restore = false) {
      address (11, reinterpret_cast<std::uintptr_t> (&s));
      for (unsigned n = 0; n < f.preserved_count; ++n)
        memory (restore ? 0x8b : 0x89, unsigned (f.preserved[n]), unsigned (f.preserved[n]) * 8);
      emit ({0x49, 0x0f, 0xae, std::uint8_t (restore ? 0x8b : 0x83)});
      integer (800, 4);
      if (!restore) {
        emit ({0x49, 0x89, 0xe2});
        memory (0x89, 10, 768);
        for (unsigned n = 0; n < 16; ++n) memory (0x7f, n, 256 + n * 16, true);
      }
    }
  } c;
  c.snapshot (probe.original, f);
  c.stack (false, f.frame_bytes);
  for (unsigned n = 0; n < f.preserved_count; ++n)
    c.address (unsigned (f.preserved[n]), 0x123456789abc0000ull + unsigned (f.preserved[n]));
  c.address (11, reinterpret_cast<std::uintptr_t> (&probe.before));
  for (unsigned n = 6; n < 16; ++n) c.memory (0x6f, n, 256 + n * 16, true);
  if (target.abi () == target::ABI::X86SysV)
    c.emit ({0xd9, 0xe8, 0xd9, 0xeb});  // two live x87 values
  c.snapshot (probe.before, f);
  // RIP-relative LEA resolves the local target without embedding a publication
  // address. Invoke consumes four native ABI arguments.
  auto first = unsigned (f.parameters[0]);
  c.emit ({std::uint8_t (0x48 | (first >> 3) << 2), 0x8d, std::uint8_t (0x05 | (first & 7) << 3)});
  auto displacement = c.bytes.size ();
  c.integer (0, 4);
  c.address (unsigned (f.parameters[3]), reinterpret_cast<std::uintptr_t> (&probe.checkpoint));
  c.address (11, entry);
  c.emit ({0x41, 0xff, 0xd3});
  c.address (11, reinterpret_cast<std::uintptr_t> (&probe.result));
  c.memory (0x89, 0, 0);
  c.snapshot (probe.after, f);
  c.stack (true, f.frame_bytes);
  c.snapshot (probe.original, f, true);
  c.emit ({0xc3});
  auto delta = c.bytes.size () - displacement - 4;
  for (unsigned n = 0; n < 4; ++n) c.bytes[displacement + n] = std::uint8_t (delta >> (n * 8));
  if (target_offset) *target_offset = c.bytes.size ();
  for (unsigned n = 0; n < f.preserved_count; ++n)
    if (!raw || f.preserved[n] != target::X86Register::BP)
      c.address (unsigned (f.preserved[n]), 0xdeadbeef00000000ull + unsigned (f.preserved[n]));
  for (unsigned n = 6; n < 16; ++n) {
    c.emit ({0x66});
    if (n >= 8) c.emit ({0x45});
    c.emit ({0x0f, 0xef, std::uint8_t (0xc0 | ((n & 7) << 3) | (n & 7))});
  }
  if (fail) {
    c.emit ({0xdb, 0xe3});
    if (extra) c.stack (false, extra);
    c.address (11, recovery);
    c.emit ({0x41, 0xff, 0xe3});
  } else {
    c.address (0, 42);
    c.emit ({0xc3});
  }
  return std::move (c.bytes);
}
}  // namespace
TEST_CASE (
  "native checkpoint recovery restores host stack nonvolatile registers vectors and floating state "
  "bit for bit") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto bytes
    = make_invocation_entries ((target).value (), reinterpret_cast<std::uintptr_t> (&checkpoint));
  REQUIRE ((bytes).has_value ());
  auto reservation = CodeMemory ().reserve (
    std::array{bytes.value ().invoke.size (), bytes.value ().recover.size ()});
  REQUIRE ((reservation).has_value ());
  const std::array<std::span<const std::uint8_t>, 2> entries{bytes.value ().invoke,
                                                             bytes.value ().recover};
  auto code = reservation.value ().publish (entries);
  REQUIRE ((code).has_value ());
  bool arm = target.value ().architecture () == target::Architecture::AArch64;
  const auto extras
    = arm ? std::array<unsigned, 4>{0, 16, 512, 528} : std::array<unsigned, 4>{0, 8, 512, 520};
  for (bool fail : {false, true})
    for (unsigned extra : extras) {
      CAPTURE (fail);
      CAPTURE (extra);
      Probe probe{};
      for (unsigned n = 0; n < 16; ++n)
        probe.before.vectors[n] = {0x123456789abcdef0ull + n, 0xfedcba9876543210ull - n};
      active = &probe.checkpoint;
      auto body = arm ? arm_probe (probe, ((code).value ())[0].address (),
                                   ((code).value ())[1].address (), fail, extra)
                      : x86_probe ((target).value (), probe, ((code).value ())[0].address (),
                                   ((code).value ())[1].address (), fail, extra);
      auto slot = CodeMemory ().reserve (std::array{body.size ()});
      REQUIRE ((slot).has_value ());
      const std::array<std::span<const std::uint8_t>, 1> data{body};
      auto executable = slot.value ().publish (data);
      REQUIRE ((executable).has_value ());
      reinterpret_cast<void (*) ()> (((executable).value ())[0].address ()) ();
      active = nullptr;
      CHECK (probe.result == (fail ? 0 : 42));
      CHECK (probe.after.original_sp == probe.before.original_sp);
      if (arm) {
        for (unsigned n = 19; n < 30; ++n)
          CHECK (probe.after.general[n] == probe.before.general[n]);
        for (unsigned n = 8; n < 16; ++n) CHECK (probe.after.vectors[n] == probe.before.vectors[n]);
        CHECK (probe.after.fpcr == probe.before.fpcr);
        CHECK (probe.after.fpsr == probe.before.fpsr);
      } else {
        auto f = codegen::detail::x86_invocation_frame (target.value ().abi ());
        REQUIRE ((f).has_value ());
        for (unsigned n = 0; n < f.value ().preserved_count; ++n)
          CHECK (probe.after.general[unsigned (f.value ().preserved[n])]
                 == probe.before.general[unsigned (f.value ().preserved[n])]);
        if (target.value ().abi () == target::ABI::X86Windows)
          for (unsigned n = 6; n < 16; ++n)
            CHECK (probe.after.vectors[n] == probe.before.vectors[n]);
        for (auto n : {0, 1, 2, 3, 4, 24, 25, 26, 27})
          CHECK (probe.after.extended[n] == probe.before.extended[n]);
        if (target.value ().abi () == target::ABI::X86SysV)
          for (unsigned n = 32; n < 64; ++n)
            if ((n - 32) % 16 < 10) CHECK (probe.after.extended[n] == probe.before.extended[n]);
      }
    }
}
TEST_CASE (
  "raw native boundaries retain successful hardware register effects and restore them only on "
  "failure") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto boundary = execution::detail::NativeBoundary::create ();
  REQUIRE ((boundary).has_value ());
  bool arm = target.value ().architecture () == target::Architecture::AArch64;
  const auto extras
    = arm ? std::array<unsigned, 4>{0, 16, 512, 528} : std::array<unsigned, 4>{0, 8, 512, 520};
  for (bool fail : {false, true})
    for (auto extra : extras) {
      CAPTURE (fail);
      CAPTURE (extra);
      Probe probe{};
      std::size_t target_offset = 0;
      for (unsigned n = 0; n < 16; ++n)
        probe.before.vectors[n] = {0x123456789abcdef0ull + n, 0xfedcba9876543210ull - n};
      auto emit = [&] (std::uintptr_t entry) {
        return arm ? arm_probe (probe, entry, boundary.value ().recovery ().recovery_address (),
                                fail, extra, true, &target_offset)
                   : x86_probe ((target).value (), probe, entry,
                                boundary.value ().recovery ().recovery_address (), fail, extra,
                                true, &target_offset);
      };
      auto body = emit (0);
      auto slot = CodeMemory ().reserve (std::array{body.size ()});
      REQUIRE ((slot).has_value ());
      auto base = slot.value ().address (0);
      REQUIRE ((base).has_value ());
      const std::array destinations{
        execution::detail::NativeBoundaryTarget{(base).value () + target_offset, 0}};
      auto entries = boundary.value ().add_entries (destinations);
      REQUIRE ((entries).has_value ());
      body = emit (((entries).value ())[0]);
      const std::array<std::span<const std::uint8_t>, 1> data{body};
      auto executable = slot.value ().publish (data);
      REQUIRE ((executable).has_value ());
      reinterpret_cast<void (*) ()> (((executable).value ())[0].address ()) ();
      CHECK (probe.result == (fail ? 0 : 42));
      CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
      CHECK (probe.after.original_sp == probe.before.original_sp);
      if (arm) {
        for (unsigned n = 19; n < 29; ++n)
          CHECK (probe.after.general[n]
                 == (fail ? probe.before.general[n] : 0xdeadbeef00000000ull + n));
        CHECK (probe.after.general[29] == probe.before.general[29]);
        for (unsigned n = 8; n < 16; ++n)
          CHECK (probe.after.vectors[n]
                 == (fail ? probe.before.vectors[n] : std::array<std::uint64_t, 2>{0, 0}));
        CHECK (probe.after.fpcr == probe.before.fpcr);
        CHECK (probe.after.fpsr == probe.before.fpsr);
      } else {
        auto f = codegen::detail::x86_invocation_frame (target.value ().abi ());
        REQUIRE ((f).has_value ());
        for (unsigned n = 0; n < f.value ().preserved_count; ++n) {
          auto reg = f.value ().preserved[n];
          CHECK (probe.after.general[unsigned (reg)]
                 == (fail || reg == target::X86Register::BP
                       ? probe.before.general[unsigned (reg)]
                       : 0xdeadbeef00000000ull + unsigned (reg)));
        }
        if (target.value ().abi () == target::ABI::X86Windows)
          for (unsigned n = 6; n < 16; ++n)
            CHECK (probe.after.vectors[n]
                   == (fail ? probe.before.vectors[n] : std::array<std::uint64_t, 2>{0, 0}));
        for (auto n : {0, 1, 2, 3, 4, 24, 25, 26, 27})
          CHECK (probe.after.extended[n] == probe.before.extended[n]);
        if (target.value ().abi () == target::ABI::X86SysV)
          for (unsigned n = 32; n < 64; ++n)
            if ((n - 32) % 16 < 10) CHECK (probe.after.extended[n] == probe.before.extended[n]);
      }
    }
}
