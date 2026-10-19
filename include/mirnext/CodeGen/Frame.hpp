/* Licensed under the MIT License. Frame formation over owning machine nodes. */
#pragma once
#include "mirnext/CodeGen/PostAllocation.hpp"
namespace mirnext::codegen {
struct MachineRegisterSave {
  target::PhysicalRegister reg;
  std::int64_t offset;
  std::size_t size;
  friend bool operator==(const MachineRegisterSave &, const MachineRegisterSave &) = default;
};
struct MachineFrameLayout {
  // Bytes subtracted from SP, excluding the separately allocated aggregate area.
  // Save offsets use the target's frame base (FP or SP); x86 FP offsets are negative.
  std::size_t frame_bytes = 0, aggregate_bytes = 0, variadic_save_bytes = 0, spill_bytes = 0;
  bool saves_previous_stack = false, saves_link = false, saves_frame_pointer = false;
  std::vector<MachineRegisterSave> callee_saves;
};
struct MachineFrameOptions {
  // Host wrappers must preserve the host ABI even when MIR bindings clobber it.
  bool preserve_host_registers = false;
  // x86 environments forbidding stores below SP. Ignored on AArch64.
  bool no_red_zone = false;
};
class MachineFrameResult {
public:
  bool current(const MachineFunction &f) const { return owner_ == &f && revision_ == f.revision() && cfg_.current(f); }
  const MachineControlFlow &cfg() const { return cfg_; }
  const MachineFrameLayout &layout() const { return layout_; }
  std::span<const MachineInstruction *const> inserted_instructions() const { return inserted_; }
  std::span<const MachineLabel *const> aligned_loop_labels() const { return aligned_labels_; }
private:
  friend struct detail::FrameEngine;
  MachineFrameResult() = default;
  const MachineFunction *owner_ = nullptr;
  std::uint64_t revision_ = 0;
  MachineControlFlow cfg_;
  MachineFrameLayout layout_;
  std::vector<const MachineInstruction *> inserted_;
  std::vector<const MachineLabel *> aligned_labels_;
};
// Consumes checked post-allocation state. No C objects or installation protocol.
// The result borrows stable machine identities; its function must outlive it.
Result<MachineFrameResult> form_frame(MachineFunction &, MachinePostResult, MachineFrameOptions = {});
}
