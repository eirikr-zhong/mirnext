/* Licensed under the MIT License. Natural C++ counterparts, compiled with Release optimizations. */
#include "JITExecutionReference.hpp"
#include <array>
#include <utility>
namespace mirnext::benchmark {
namespace {
std::uint64_t tiny (std::uint64_t x) { return x + 1; }
std::uint64_t checksum_loop (std::uint64_t n) {
  std::uint64_t r = 42;
  for (std::uint64_t i = 0; i < n; ++i) r = (r * 3) ^ i;
  return r;
}
std::uint64_t sieve (std::uint64_t rounds) {
  std::array<std::uint8_t, 8190> flags;
  std::uint64_t count = 0;
  for (std::uint64_t iter = 0; iter < rounds; ++iter) {
    flags.fill (1);
    count = 0;
    for (std::uint64_t i = 2; i < flags.size (); ++i)
      if (flags[i]) {
        auto prime = i + 1;
        for (auto k = i + prime; k < flags.size (); k += prime) flags[k] = 0;
        ++count;
      }
  }
  return count;
}
template <std::size_t... N>
std::uint64_t arithmetic (std::uint64_t x, std::index_sequence<N...>) {
  auto r = x;
  ((r = ((r * 3) ^ x) + (N + 1)), ...);
  return r;
}
std::uint64_t arithmetic64 (std::uint64_t x) {
  return arithmetic (x, std::make_index_sequence<64>{});
}
std::uint64_t arithmetic256 (std::uint64_t x) {
  return arithmetic (x, std::make_index_sequence<256>{});
}
template <std::size_t... N>
std::uint64_t branches (std::uint64_t x, std::index_sequence<N...>) {
  std::uint64_t r = 0;
  ((r += (x & (UINT64_C (1) << (N % 8))) ? 2 : 1), ...);
  return r;
}
std::uint64_t branches32 (std::uint64_t x) { return branches (x, std::make_index_sequence<32>{}); }
std::uint64_t memory_sum (std::uint64_t address) {
  auto p = reinterpret_cast<const std::uint64_t *> (static_cast<std::uintptr_t> (address));
  std::uint64_t r = 0;
  for (unsigned n = 0; n < 4096; ++n) r += p[n];
  return r;
}
}  // namespace
NativeFunction reference_function (RuntimeWorkload workload) {
  switch (workload) {
  case RuntimeWorkload::Tiny: return tiny;
  case RuntimeWorkload::ChecksumLoop: return checksum_loop;
  case RuntimeWorkload::Sieve: return sieve;
  case RuntimeWorkload::Arithmetic64: return arithmetic64;
  case RuntimeWorkload::Arithmetic256: return arithmetic256;
  case RuntimeWorkload::Branches32: return branches32;
  case RuntimeWorkload::MemorySum: return memory_sum;
  }
  return nullptr;
}
}  // namespace mirnext::benchmark
