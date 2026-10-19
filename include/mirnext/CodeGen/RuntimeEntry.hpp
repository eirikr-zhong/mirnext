/* Licensed under the MIT License. Immutable native lazy-entry code. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
namespace mirnext::codegen {
inline constexpr std::size_t lazy_entry_bytes = 64;
// Addresses are the native ABI boundary, not IR identities. The destination
// slot and descriptor must have stable storage for the entry's entire lifetime.
// External serialization covers both slot updates and entry execution.
struct LazyEntryAddresses {
  std::uintptr_t destination, descriptor, resolver;
  // x86 SysV may return up to two long doubles on the x87 stack. A failed
  // cold call must produce exactly this many results, including zero for void.
  unsigned x87_results = 0;
};
// resolver in LazyEntryAddresses denotes this shared native resolver entry;
// callback is the C++ ABI function taking the descriptor and returning a target.
Result<std::vector<std::uint8_t>> make_lazy_resolver (const target::TargetMachine &,
                                                      std::uintptr_t callback);
Result<std::vector<std::uint8_t>> make_lazy_entry (const target::TargetMachine &,
                                                   LazyEntryAddresses);
}  // namespace mirnext::codegen
