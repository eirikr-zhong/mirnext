/* Licensed under the MIT License. MIR v1 file identifiers, independent of IR enums. */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace mirnext::serialization::protocol {
inline constexpr std::uint64_t version = 1;
enum class Type : unsigned {
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F,
  D,
  LD,
  P,
  Block,
  Block1,
  Block2,
  Block3,
  Block4,
  ReturnBlock,
  Undef
};
enum Tag : unsigned {
  U0 = 0,
  U1 = 1,
  U8 = 8,
  I1 = 9,
  I8 = 16,
  F = 17,
  D = 18,
  LD = 19,
  REG1 = 20,
  REG4 = 23,
  NAME1 = 24,
  NAME4 = 27,
  STR1 = 28,
  STR4 = 31,
  LAB1 = 32,
  LAB4 = 35,
  MEM_DISP = 36,
  MEM_LAST = 42,
  TYPE = 43,
  EOI = 61,
  EOFILE = 62,
  ALIAS_DISP = 63,
  ALIAS_LAST = 69
};
#if defined(__x86_64__) && !defined(_WIN32)
inline constexpr std::size_t long_double_bytes = 10;
#else
inline constexpr std::size_t long_double_bytes = sizeof (long double);
#endif
inline std::array<std::byte, 16> floating_bytes (long double value) {
  std::array<std::byte, 16> bytes{};
  static_assert (long_double_bytes <= bytes.size ());
  std::memcpy (bytes.data (), &value, long_double_bytes);
  return bytes;
}
}  // namespace mirnext::serialization::protocol
