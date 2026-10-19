/* Licensed under the MIT License. Separate translation-unit native references. */
#pragma once
#include <cstdint>
namespace mirnext::benchmark {
using NativeFunction = std::uint64_t (*) (std::uint64_t);
enum class RuntimeWorkload {
  Tiny,
  ChecksumLoop,
  Sieve,
  Arithmetic64,
  Arithmetic256,
  Branches32,
  MemorySum
};
NativeFunction reference_function (RuntimeWorkload);
}  // namespace mirnext::benchmark
