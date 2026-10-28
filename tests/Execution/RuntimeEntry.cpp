/* Licensed under the MIT License. Native immutable entry ABI boundary. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/RuntimeEntry.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include <array>
#include <cfenv>
#include <cstdarg>
#include <set>
using namespace mirnext;
using namespace mirnext::codegen;
using namespace mirnext::execution;
TEST_CASE (
  "x86 cold resolver layouts preserve usable registers and align full floating state for both "
  "ABIs") {
  using R = target::X86Register;
  for (auto abi : {target::ABI::X86SysV, target::ABI::X86Windows}) {
    auto frame = codegen::detail::x86_resolver_frame (abi);
    REQUIRE ((frame).has_value ());
    CHECK (frame.value ().argument == (abi == target::ABI::X86Windows ? R::CX : R::DI));
    CHECK (frame.value ().shadow_bytes == (abi == target::ABI::X86Windows ? 32 : 0));
    CHECK (frame.value ().frame_bytes == (abi == target::ABI::X86Windows ? 656 : 624));
    CHECK (frame.value ().fx_offset % 16 == 0);
    CHECK (frame.value ().fx_offset >= frame.value ().shadow_bytes);
    CHECK (frame.value ().metadata_offset == frame.value ().frame_bytes);
    CHECK (frame.value ().saves.size () == 13);
    // Ordinary function entry is SP = 8 mod 16. A cold-only metadata push
    // plus this frame gives aligned callback SP and aligned FXSAVE64 storage.
    constexpr std::uintptr_t original_sp = 0x100000008;
    auto sp = original_sp - 8 - frame.value ().frame_bytes;
    CHECK (sp % 16 == 0);
    CHECK ((sp + frame.value ().fx_offset) % 16 == 0);
    CHECK (sp + frame.value ().metadata_offset == original_sp - 8);
    std::set<R> registers;
    std::set<std::size_t> bytes;
    for (std::size_t n = 0; n < frame.value ().shadow_bytes; ++n) CHECK (bytes.insert (n).second);
    for (std::size_t n = 0; n < 512; ++n)
      CHECK (bytes.insert (frame.value ().fx_offset + n).second);
    for (auto &save : frame.value ().saves) {
      CHECK (registers.insert (save.reg).second);
      CHECK (save.offset % 8 == 0);
      for (unsigned n = 0; n < 8; ++n) {
        CHECK (bytes.insert (save.offset + n).second);
        CHECK (save.offset + n < frame.value ().frame_bytes);
      }
    }
    for (unsigned n = 0; n < 16; ++n)
      CHECK (registers.contains (static_cast<R> (n))
             == (n != unsigned (R::SP) && n != unsigned (R::R10) && n != unsigned (R::R11)));
    CHECK (sp + frame.value ().frame_bytes + 8 == original_sp);
  }
  CHECK_FALSE ((codegen::detail::x86_resolver_frame (target::ABI::AArch64Linux)).has_value ());
  CHECK_FALSE ((codegen::detail::x86_resolver_frame (static_cast<target::ABI> (99))).has_value ());
}
namespace {
struct Descriptor {
  std::uintptr_t destination = 0, target = 0;
  unsigned callbacks = 0;
  bool fail = false, change_rounding = false;
};
std::uintptr_t resolve (Descriptor *descriptor) {
  ++descriptor->callbacks;
#if defined(__x86_64__) && !defined(_MSC_VER)
  // Force real clobbers at the native boundary instead of depending on the
  // compiler to happen to use argument XMM registers in this small callback.
  asm volatile (
    "pxor %%xmm0,%%xmm0\npxor %%xmm1,%%xmm1\npxor %%xmm2,%%xmm2\npxor %%xmm3,%%xmm3\n"
    "pxor %%xmm4,%%xmm4\npxor %%xmm5,%%xmm5\npxor %%xmm6,%%xmm6\npxor %%xmm7,%%xmm7\n"
    "pxor %%xmm8,%%xmm8\npxor %%xmm9,%%xmm9\npxor %%xmm10,%%xmm10\npxor %%xmm11,%%xmm11\n"
    "pxor %%xmm12,%%xmm12\npxor %%xmm13,%%xmm13\npxor %%xmm14,%%xmm14\npxor %%xmm15,%%xmm15"
    :
    :
    : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
      "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
  if (descriptor->change_rounding) {
    std::fesetround (FE_DOWNWARD);
    std::feraiseexcept (FE_INEXACT);
  }
  if (descriptor->fail) return 0;
  return descriptor->destination = descriptor->target;
}
std::vector<ExecutableCode> entry (Descriptor &descriptor, unsigned x87 = 0) {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto resolver
    = make_lazy_resolver ((target).value (), reinterpret_cast<std::uintptr_t> (&resolve));
  REQUIRE ((resolver).has_value ());
  auto gate = make_lazy_entry ((target).value (),
                               {reinterpret_cast<std::uintptr_t> (&descriptor.destination),
                                reinterpret_cast<std::uintptr_t> (&descriptor), 4, x87});
  REQUIRE ((gate).has_value ());
  CHECK (gate.value ().size () == 64);
  CHECK (resolver.value ().size () < 1024);
  const std::array sizes{resolver.value ().size (), gate.value ().size ()};
  auto reserved = CodeMemory ().reserve (sizes);
  REQUIRE ((reserved).has_value ());
  auto address = reserved.value ().address (0);
  REQUIRE ((address).has_value ());
  gate
    = make_lazy_entry ((target).value (),
                       {reinterpret_cast<std::uintptr_t> (&descriptor.destination),
                        reinterpret_cast<std::uintptr_t> (&descriptor), (address).value (), x87});
  REQUIRE ((gate).has_value ());
  const std::array<std::span<const std::uint8_t>, 2> bytes{(resolver).value (), (gate).value ()};
  auto code = reserved.value ().publish (bytes);
  REQUIRE ((code).has_value ());
  return std::move ((code).value ());
}
std::int64_t integers (std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d,
                       std::int64_t e, std::int64_t f, std::int64_t g, std::int64_t h,
                       std::int64_t i, std::int64_t j) {
  return a + b + c + d + e + f + g + h + i + j;
}
double doubles (double a, double b, double c, double d, double e, double f, double g, double h,
                double i, double j) {
  return a + b + c + d + e + f + g + h + i + j;
}
double variadic (int count, ...) {
  va_list list;
  va_start (list, count);
  double result = 0;
  for (int n = 0; n < count; ++n) result += va_arg (list, double);
  va_end (list);
  return result;
}
int rounding () { return std::fegetround (); }
long double extended () { return 2.5L; }
}  // namespace
TEST_CASE (
  "native immutable gates preserve integer floating stack and variadic arguments through cold and "
  "warm calls") {
  Descriptor descriptor;
  descriptor.target = reinterpret_cast<std::uintptr_t> (&integers);
  auto code = entry (descriptor);
  auto integer = reinterpret_cast<decltype (&integers)> (code[1].address ());
  for (unsigned n = 0; n < 2; ++n) CHECK (integer (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
  CHECK (descriptor.callbacks == 1);
  descriptor = {};
  descriptor.target = reinterpret_cast<std::uintptr_t> (&doubles);
  auto real = reinterpret_cast<decltype (&doubles)> (code[1].address ());
  for (unsigned n = 0; n < 2; ++n) CHECK (real (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
  CHECK (descriptor.callbacks == 1);
  descriptor = {};
  descriptor.target = reinterpret_cast<std::uintptr_t> (&variadic);
  auto vararg = reinterpret_cast<decltype (&variadic)> (code[1].address ());
  for (unsigned n = 0; n < 2; ++n)
    CHECK (vararg (10, 1., 2., 3., 4., 5., 6., 7., 8., 9., 10.) == 55);
  CHECK (descriptor.callbacks == 1);
}
TEST_CASE (
  "native resolver failures keep the entry cold and retry with deterministic scalar results") {
  Descriptor descriptor;
  descriptor.target = reinterpret_cast<std::uintptr_t> (&integers);
  descriptor.fail = true;
  auto code = entry (descriptor);
  auto call = reinterpret_cast<decltype (&integers)> (code[1].address ());
  CHECK (call (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 0);
  CHECK (descriptor.destination == 0);
  descriptor.fail = false;
  CHECK (call (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
  CHECK (descriptor.callbacks == 2);
  descriptor = {};
  descriptor.target = reinterpret_cast<std::uintptr_t> (&doubles);
  descriptor.fail = true;
  auto real = reinterpret_cast<decltype (&doubles)> (code[1].address ());
  CHECK (real (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 0);
  descriptor.fail = false;
  CHECK (real (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
  CHECK (descriptor.callbacks == 2);
}
TEST_CASE (
  "native resolvers restore floating control and x86 returns precisely one x87 zero after "
  "failure") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  struct Restore {
    std::fenv_t environment;
    Restore () { std::fegetenv (&environment); }
    ~Restore () { std::fesetenv (&environment); }
  } restore;
  REQUIRE (std::fesetround (FE_TONEAREST) == 0);
  REQUIRE (std::feclearexcept (FE_ALL_EXCEPT) == 0);
  Descriptor descriptor;
  descriptor.target = reinterpret_cast<std::uintptr_t> (&rounding);
  descriptor.change_rounding = true;
  auto code = entry (descriptor);
  CHECK (reinterpret_cast<int (*) ()> (code[1].address ()) () == FE_TONEAREST);
  CHECK (std::fegetround () == FE_TONEAREST);
  CHECK (std::fetestexcept (FE_INEXACT) == 0);
  descriptor.destination = 0;
  descriptor.fail = true;
  CHECK (reinterpret_cast<int (*) ()> (code[1].address ()) () == 0);
  CHECK (descriptor.destination == 0);
  CHECK (std::fegetround () == FE_TONEAREST);
  CHECK (std::fetestexcept (FE_INEXACT) == 0);
  descriptor.fail = false;
  CHECK (reinterpret_cast<int (*) ()> (code[1].address ()) () == FE_TONEAREST);
  CHECK (descriptor.callbacks == 3);
  if (target.value ().architecture () != target::Architecture::X86_64
      || sizeof (long double) <= sizeof (double))
    return;
  descriptor = {};
  descriptor.target = reinterpret_cast<std::uintptr_t> (&extended);
  descriptor.fail = true;
  code = entry (descriptor, 1);
  auto extended_call = reinterpret_cast<long double (*) ()> (code[1].address ());
  // More than eight failed calls would overflow x87 if the resolver returned
  // an extra value. Each host call consumes exactly its one declared result.
  for (unsigned n = 0; n < 20; ++n) CHECK (extended_call () == 0.L);
  CHECK (descriptor.callbacks == 20);
  descriptor.fail = false;
  CHECK (extended_call () == 2.5L);
  CHECK (descriptor.callbacks == 21);
}
TEST_CASE ("native gate validation rejects invalid slots descriptors resolvers and x87 counts") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  CHECK_FALSE ((make_lazy_resolver ((target).value (), 0)).has_value ());
  for (auto addresses :
       {LazyEntryAddresses{0, 8, 4}, LazyEntryAddresses{1, 8, 4}, LazyEntryAddresses{8, 0, 4},
        LazyEntryAddresses{8, 8, 0}, LazyEntryAddresses{8, 8, 4, 3}})
    CHECK_FALSE ((make_lazy_entry ((target).value (), addresses)).has_value ());
  if (target.value ().architecture () != target::Architecture::X86_64
      || target.value ().abi () == target::ABI::X86Windows)
    CHECK_FALSE ((make_lazy_entry ((target).value (), {8, 8, 4, 1})).has_value ());
}
