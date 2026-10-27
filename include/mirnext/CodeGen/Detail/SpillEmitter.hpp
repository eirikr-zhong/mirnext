/* Licensed under the MIT License. Shared typed allocation spill service. */
#pragma once
#include "mirnext/CodeGen/RegisterRewrite.hpp"
namespace mirnext::codegen::detail {
struct MachineSpillEmitter {
  MachineFunction &f;
  MachineRewriter rw;
  MachineRewriteResult &result;
  SparseBitSet &used;
  std::string_view stage;
  MachineSpillEmitter (MachineFunction &f, MachineRewriteResult &r, SparseBitSet &used,
                       std::string_view stage)
    : f (f), rw (f), result (r), used (used), stage (stage) {}
  Error invalid (std::string) const;
  Result<std::int64_t> offset (StackLocation, ir::Type, std::size_t extra = 0) const;
  static bool valid_offset (target::Architecture, ir::Type, std::int64_t);
  target::PhysicalRegister base () const;
  std::optional<target::PhysicalRegister> temporary (ir::Type, bool first) const;
  Result<void> access (StackLocation, ir::Type, target::PhysicalRegister data,
                       target::PhysicalRegister address, MachineBasicBlock &,
                       MachineInstruction *anchor, bool after, bool store);
  Result<void> preserve (const MachineVirtualRegister *, MachineBasicBlock &,
                         MachineInstruction *anchor, bool after, bool restore);
};
}  // namespace mirnext::codegen::detail
