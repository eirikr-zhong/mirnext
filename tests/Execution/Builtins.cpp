/* Licensed under the MIT License. Native helpers and all maintained ABI cursors. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Builtins.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
using namespace mirnext;
using namespace mirnext::execution;
namespace {
struct SysV {
  std::uint32_t gp, fp;
  std::uintptr_t stack, registers;
};
struct Arm {
  std::uintptr_t stack, gr, vr;
  std::int32_t go, vo;
};
struct Pointer {
  std::uintptr_t stack;
};
template <class T>
auto bytes (T &value) {
  return std::span (reinterpret_cast<std::byte *> (&value), sizeof value);
}
template <class T>
std::uintptr_t address (T &value) {
  return reinterpret_cast<std::uintptr_t> (&value);
}
template <class T>
T function (codegen::Builtin id) {
  auto p = builtin_address (id);
  REQUIRE (p);
  return reinterpret_cast<T> (*p);
}
}  // namespace
TEST_CASE ("typed native builtin identities resolve by builtin kind") {
  for (unsigned n = 0; n <= static_cast<unsigned> (codegen::Builtin::LongDoubleGe); ++n) {
    auto p = builtin_address (static_cast<codegen::Builtin> (n));
    REQUIRE (p);
    CHECK (*p != 0);
  }
  CHECK_FALSE (builtin_address (static_cast<codegen::Builtin> (999)));
}
TEST_CASE (
  "typed long double helpers preserve arithmetic comparisons NaN and conversion boundaries") {
  using B = codegen::Builtin;
  using Binary = long double (*) (long double, long double);
  using Compare = std::int64_t (*) (long double, long double);
  CHECK (function<Binary> (B::LongDoubleAdd) (1.25L, 2.5L) == 3.75L);
  CHECK (function<Binary> (B::LongDoubleSub) (4.5L, 1.25L) == 3.25L);
  CHECK (function<Binary> (B::LongDoubleMul) (1.5L, 2.0L) == 3.0L);
  CHECK (function<Binary> (B::LongDoubleDiv) (3.0L, 2.0L) == 1.5L);
  auto neg = function<long double (*) (long double)> (B::LongDoubleNeg);
  CHECK (neg (3.0L) == -3.0L);
  CHECK (std::signbit (neg (0)));
  for (auto id : {B::LongDoubleEq, B::LongDoubleNe, B::LongDoubleLt, B::LongDoubleLe,
                  B::LongDoubleGt, B::LongDoubleGe}) {
    auto f = function<Compare> (id);
    CHECK (f (2, 3) == (id == B::LongDoubleNe || id == B::LongDoubleLt || id == B::LongDoubleLe));
    CHECK (f (std::numeric_limits<long double>::quiet_NaN (), 0) == (id == B::LongDoubleNe));
  }
  CHECK (function<long double (*) (std::uint64_t)> (B::UnsignedToLongDouble) (UINT64_MAX)
         == static_cast<long double> (UINT64_MAX));
  CHECK (function<long double (*) (std::int64_t)> (B::IntegerToLongDouble) (INT64_MIN)
         == static_cast<long double> (INT64_MIN));
  CHECK (function<long double (*) (float)> (B::FloatToLongDouble) (1.25f) == 1.25L);
  CHECK (function<long double (*) (double)> (B::DoubleToLongDouble) (2.5) == 2.5L);
  CHECK (function<float (*) (long double)> (B::LongDoubleToFloat) (1.25L) == 1.25f);
  CHECK (function<double (*) (long double)> (B::LongDoubleToDouble) (2.5L) == 2.5);
  CHECK (function<float (*) (std::uint64_t)> (B::UnsignedToFloat) (UINT64_MAX)
         == static_cast<float> (UINT64_MAX));
  CHECK (function<double (*) (std::uint64_t)> (B::UnsignedToDouble) (UINT64_MAX)
         == static_cast<double> (UINT64_MAX));
  auto integer = function<std::int64_t (*) (long double)> (B::LongDoubleToInteger);
  CHECK (integer (-3.75L) == -3);
#if defined(__x86_64__) || defined(_M_X64)
  CHECK (integer (std::numeric_limits<long double>::quiet_NaN ()) == INT64_MIN);
  CHECK (integer (std::ldexp (1.0L, 63)) == INT64_MIN);
#else
  CHECK (integer (std::numeric_limits<long double>::quiet_NaN ()) == 0);
  CHECK (integer (std::ldexp (1.0L, 63)) == INT64_MAX);
#endif
}
TEST_CASE ("typed block helpers retain forward word copying and first-error nested scopes") {
  auto copy = function<void (*) (std::byte *, const std::byte *, std::uint64_t)> (
    codegen::Builtin::CopyBlock);
  std::array<std::uint64_t, 3> words{1, 2, 3};
  copy (bytes (words).data () + 8, bytes (words).data (), 2);
  CHECK (words == std::array<std::uint64_t, 3>{1, 1, 1});
  auto argument = function<std::byte *(*) (std::byte *, const std::byte *, std::size_t)> (
    codegen::Builtin::CopyArgument);
  std::array<std::byte, 7> source{std::byte{42}}, destination{};
  CHECK (argument (destination.data (), source.data (), 7) == destination.data ());
  CHECK (destination == source);
  ExecutionInvocation outer;
  copy (nullptr, nullptr, 1);
  auto first = outer.status ();
  REQUIRE_FALSE (first);
  {
    ExecutionInvocation inner;
    auto scalar
      = function<std::byte *(*) (std::byte *, std::uint64_t)> (codegen::Builtin::VariadicScalar);
    CHECK (scalar (nullptr, 999) == nullptr);
    CHECK_FALSE (inner.status ());
  }
  {
    ExecutionInvocation inner;
    auto scalar
      = function<std::byte *(*) (std::byte *, std::uint64_t)> (codegen::Builtin::VariadicScalar);
    CHECK (scalar (nullptr, 0) == nullptr);
    REQUIRE_FALSE (inner.status ());
    CHECK (inner.status ().error ().message.find ("state or ABI") != std::string::npos);
  }
  {
    ExecutionInvocation inner;
    auto block = function<void (*) (std::byte *, std::byte *, std::size_t, std::uint64_t)> (
      codegen::Builtin::VariadicBlock);
    block (nullptr, nullptr, 8, 0);
    REQUIRE_FALSE (inner.status ());
    CHECK (inner.status ().error ().message.find ("state or ABI") != std::string::npos);
  }
  argument (nullptr, nullptr, 1);
  CHECK (outer.status ().error ().message == first.error ().message);
}
TEST_CASE (
  "typed SysV scalar cursors exhaust independent register banks and align stack long doubles") {
  alignas (16) std::array<std::byte, 176> registers{};
  alignas (16) std::array<std::byte, 64> stack{};
  SysV state{40, 160, address (stack) + 1, address (registers)};
  auto a = advance_variadic_scalar (target::ABI::X86SysV, bytes (state), ir::Type::i64 ());
  REQUIRE (a);
  CHECK (*a == address (registers) + 40);
  CHECK (state.gp == 48);
  auto b
    = advance_variadic_scalar (target::ABI::X86SysV, bytes (state), ir::Type (ir::Type::Kind::F64));
  REQUIRE (b);
  CHECK (*b == address (registers) + 160);
  CHECK (state.fp == 176);
  auto c = advance_variadic_scalar (target::ABI::X86SysV, bytes (state),
                                    ir::Type (ir::Type::Kind::LongDouble), 16);
  REQUIRE (c);
  CHECK (*c == address (stack) + 16);
  CHECK (state.stack == address (stack) + 32);
  auto d = advance_variadic_scalar (target::ABI::X86SysV, bytes (state), ir::Type::i64 ());
  REQUIRE (d);
  CHECK (*d == address (stack) + 32);
  CHECK (state.stack == address (stack) + 40);
}
TEST_CASE ("typed AArch64 scalar cursors preserve saved register offsets and stack alignment") {
  alignas (16) std::array<std::byte, 128> registers{};
  alignas (16) std::array<std::byte, 64> stack{};
  Arm state{address (stack) + 1, address (registers) + 64, address (registers) + 128, -8, -16};
  auto a = advance_variadic_scalar (target::ABI::AArch64Linux, bytes (state), ir::Type::i64 ());
  REQUIRE (a);
  CHECK (*a == address (registers) + 56);
  CHECK (state.go == 0);
  auto b = advance_variadic_scalar (target::ABI::AArch64Linux, bytes (state),
                                    ir::Type (ir::Type::Kind::LongDouble), 16);
  REQUIRE (b);
  CHECK (*b == address (registers) + 112);
  CHECK (state.vo == 0);
  auto c = advance_variadic_scalar (target::ABI::AArch64Linux, bytes (state),
                                    ir::Type (ir::Type::Kind::LongDouble), 16);
  REQUIRE (c);
  CHECK (*c == address (stack) + 16);
  CHECK (state.stack == address (stack) + 32);
}
TEST_CASE ("typed pointer variadic ABIs retain distinct long double increments") {
  for (auto abi : {target::ABI::X86Windows, target::ABI::AArch64MacOS})
    for (unsigned size : {8u, 16u}) {
      Pointer state{0x1000};
      auto a
        = advance_variadic_scalar (abi, bytes (state), ir::Type (ir::Type::Kind::LongDouble), size);
      REQUIRE (a);
      CHECK (*a == 0x1000);
      CHECK (state.stack == 0x1000 + (abi == target::ABI::AArch64MacOS ? size : 8));
    }
}
TEST_CASE (
  "typed SysV block classifications select both words and roll back to stack on bank exhaustion") {
  std::array<std::uint64_t, 22> registers{};
  registers[0] = 11;
  registers[1] = 22;
  registers[6] = 33;
  registers[8] = 44;
  std::array<std::uint64_t, 2> stack{55, 66};
  for (unsigned classification = 0; classification <= 4; ++classification)
    for (bool exhausted : {false, true}) {
      SysV state{exhausted ? 48u : 0u, exhausted ? 176u : 48u, address (stack),
                 address (registers)};
      std::array<std::uint64_t, 2> destination{};
      auto r = advance_variadic_block (target::ABI::X86SysV, bytes (state), bytes (destination), 16,
                                       static_cast<ir::Type::BlockClass> (classification));
      REQUIRE (r);
      auto expected = exhausted || !classification ? stack
                      : classification == 1        ? std::array<std::uint64_t, 2>{11, 22}
                      : classification == 2        ? std::array<std::uint64_t, 2>{33, 44}
                      : classification == 3        ? std::array<std::uint64_t, 2>{11, 33}
                                                   : std::array<std::uint64_t, 2>{33, 11};
      CHECK (destination == expected);
      if (exhausted || !classification)
        CHECK (state.stack == address (stack) + 16);
      else
        CHECK (state.stack == address (stack));
    }
}
TEST_CASE ("typed AArch64 blocks preserve register boundary fallback and indirect storage") {
  std::array<std::uint64_t, 4> registers{11, 22, 33, 44}, stack{55, 66, 77, 88};
  for (std::int32_t offset : {-16, -8, 0}) {
    Arm state{address (stack), address (registers) + 32, 0, offset, 0};
    std::array<std::uint64_t, 2> result{};
    REQUIRE (advance_variadic_block (target::ABI::AArch64Linux, bytes (state), bytes (result), 16));
    CHECK (result
           == (offset == -16 ? std::array<std::uint64_t, 2>{33, 44}
                             : std::array<std::uint64_t, 2>{55, 66}));
  }
  std::uintptr_t pointer = address (stack);
  Arm state{0, address (pointer) + 8, 0, -8, 0};
  std::array<std::uint64_t, 3> result{};
  REQUIRE (advance_variadic_block (target::ABI::AArch64Linux, bytes (state), bytes (result), 24));
  CHECK (result == std::array<std::uint64_t, 3>{55, 66, 77});
  CHECK (state.go == 0);
}
TEST_CASE ("typed pointer ABI blocks use the original direct and indirect size thresholds") {
  std::array<std::uint64_t, 4> source{11, 22, 33, 44};
  for (auto abi : {target::ABI::X86Windows, target::ABI::AArch64MacOS})
    for (std::size_t size : {8u, 16u, 24u}) {
      bool indirect = size > (abi == target::ABI::X86Windows ? 8u : 16u);
      std::uintptr_t pointer = address (source);
      Pointer state{indirect ? address (pointer) : address (source)};
      auto start = state.stack;
      std::array<std::uint64_t, 3> result{};
      REQUIRE (advance_variadic_block (abi, bytes (state), bytes (result), size));
      CHECK (std::memcmp (result.data (), source.data (), size) == 0);
      CHECK (state.stack == start + (indirect ? 8 : size));
    }
}
TEST_CASE (
  "typed variadic validation reports overflow invalid metadata and atomic cursor failures") {
  Pointer state{UINTPTR_MAX - 3};
  auto before = state;
  CHECK_FALSE (advance_variadic_scalar (target::ABI::X86Windows, bytes (state), ir::Type::i64 ()));
  CHECK (state.stack == before.stack);
  std::array<std::byte, 64> storage{}, output{};
  for (auto abi : {target::ABI::X86SysV, target::ABI::X86Windows, target::ABI::AArch64Linux,
                   target::ABI::AArch64MacOS}) {
    CHECK_FALSE (advance_variadic_scalar (abi, {}, ir::Type::i64 ()));
    CHECK_FALSE (advance_variadic_scalar (abi, storage, ir::Type::block (8)));
    CHECK_FALSE (advance_variadic_scalar (abi, storage, ir::Type::i64 (), 7));
    CHECK_FALSE (advance_variadic_block (abi, storage, output, SIZE_MAX));
    CHECK_FALSE (
      advance_variadic_block (abi, storage, output, 8, static_cast<ir::Type::BlockClass> (5)));
    CHECK_FALSE (advance_variadic_block (abi, storage, std::span (output).first (7), 8));
    CHECK_FALSE (initialize_variadic_state (abi, {}, storage));
    CHECK_FALSE (initialize_variadic_state (abi, storage, {}));
  }
  CHECK (variadic_state_size (static_cast<target::ABI> (999)) == 0);
  CHECK_FALSE (advance_variadic_scalar (static_cast<target::ABI> (999), storage, ir::Type::i64 ()));
  SysV sysv{0, 48, 0, 0};
  auto copy = sysv;
  CHECK_FALSE (advance_variadic_scalar (target::ABI::X86SysV, bytes (sysv), ir::Type::i64 ()));
  CHECK (std::memcmp (&sysv, &copy, sizeof sysv) == 0);
}
TEST_CASE ("typed variadic state initialization copies exactly the selected ABI representation") {
  std::array<std::byte, 64> source{}, destination{};
  for (unsigned n = 0; n < source.size (); ++n) source[n] = std::byte (n + 1);
  for (auto abi : {target::ABI::X86SysV, target::ABI::X86Windows, target::ABI::AArch64Linux,
                   target::ABI::AArch64MacOS}) {
    destination.fill (std::byte{});
    REQUIRE (initialize_variadic_state (abi, destination, source));
    auto size = variadic_state_size (abi);
    CHECK (std::equal (source.begin (), source.begin () + size, destination.begin ()));
    CHECK (destination[size] == std::byte{});
  }
}
