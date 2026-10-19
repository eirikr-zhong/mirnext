/* Licensed under the MIT License. Immutable entries for lazy block compilation. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
namespace mirnext::codegen {
inline constexpr std::size_t block_entry_bytes = 64;
inline constexpr std::size_t block_entry_cold_offset = 16;
// The slot starts at entry_address + block_entry_cold_offset, never zero.
// Successful compilation redirects the slot to published code. Storage and
// execution are externally serialized, as for ordinary lazy function entries.
struct BlockEntryAddresses { std::uintptr_t destination,descriptor,resolver; };
Result<std::vector<std::uint8_t>> make_block_entry(const target::TargetMachine &,BlockEntryAddresses);
// Unlike a function entry, a block entry can have live flags, an arbitrary
// dynamic stack and a partially executed frame. Preserve them across callback.
// callback(descriptor) returns a published destination, or zero on failure.
// Zero dispatches to failure_continuation with the ORIGINAL block state; that
// continuation must perform the appropriate recovery. It must never be an
// ordinary return thunk pretending that execution is still at function entry.
Result<std::vector<std::uint8_t>> make_block_resolver(const target::TargetMachine &,
    std::uintptr_t callback,std::uintptr_t failure_continuation);
}
