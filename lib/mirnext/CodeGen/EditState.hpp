/* Licensed under the MIT License. Private batch state, independent of facade moves. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen::detail {
struct MachineEditState {
  MachineFunction *owner;
  std::optional<Error> error;
  std::size_t pending = 0;
  bool tracks_handles = false;
  std::unordered_map<const void *, std::uint64_t> erased;
};
struct MachineBatch {
  MachineEditState *state;
  bool closed = false;
};
}  // namespace mirnext::codegen::detail
