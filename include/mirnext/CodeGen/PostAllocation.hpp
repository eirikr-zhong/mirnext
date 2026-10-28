/* Licensed under the MIT License. Post-allocation passes over owning nodes. */
#pragma once
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include <functional>
namespace mirnext::codegen {
enum class MachinePostStage { StripProperties, Liveness, Combine, DeadCode };
std::string_view stage_name (MachinePostStage);
struct MachinePostStatistics {
  std::size_t scans = 0, deleted = 0, substitutions = 0, extensions = 0;
  std::size_t moves = 0, swaps = 0, properties = 0, dead_instructions = 0, no_op_anchors = 0;
};
class MachinePostView {
 public:
  const MachineFunction &function () const { return function_; }
  const MachinePostStatistics &statistics () const { return statistics_; }
  // Frozen input topology, conservatively retained for original liveness/DCE
  // decisions after property removal. It is not the current body's CFG.
  const MachineControlFlow &input_cfg () const { return cfg_; }
  std::span<const MachineBlockLiveness> boundary_liveness () const { return boundary_; }
  std::span<const MachineRegister> deaths (const MachineInstruction &i) const {
    auto at = deaths_.find (&i);
    return at == deaths_.end () ? std::span<const MachineRegister>{} : at->second;
  }

 private:
  friend struct detail::PostAllocationEngine;
  MachinePostView (
    const MachineFunction &f, const MachineControlFlow &cfg,
    std::span<const MachineBlockLiveness> boundary,
    const std::unordered_map<const MachineInstruction *, std::vector<MachineRegister>> &deaths,
    const MachinePostStatistics &statistics)
    : function_ (f), cfg_ (cfg), boundary_ (boundary), deaths_ (deaths), statistics_ (statistics) {}
  const MachineFunction &function_;
  const MachineControlFlow &cfg_;
  std::span<const MachineBlockLiveness> boundary_;
  const std::unordered_map<const MachineInstruction *, std::vector<MachineRegister>> &deaths_;
  const MachinePostStatistics &statistics_;
};
struct MachinePostOptions {
  // False retains BBV properties; true is the whole-function JIT convention.
  bool remove_properties = true;
  // Views and references are valid only during this callback.
  std::function<Result<void> (MachinePostStage, const MachinePostView &)> observer;
};
class MachinePostResult {
 public:
  bool current (const MachineFunction &f) const { return spill_.current (f); }
  const MachineAllocation &allocation () const { return spill_.allocation (); }
  const MachineRewriteResult &rewrite_result () const { return spill_.rewrite_result (); }
  const MachineControlFlow &cfg () const { return spill_.cfg (); }
  const MachinePostStatistics &statistics () const { return statistics_; }

 private:
  friend struct detail::PostAllocationEngine;
  explicit MachinePostResult (MachineSpillResult spill) : spill_ (std::move (spill)) {}
  MachineSpillResult spill_;
  MachinePostStatistics statistics_;
};
// O0 strips properties for whole-function codegen; O1 additionally combines and
// removes dead definitions. O2/O3 remove properties during combine, then DCE.
// Operates directly on allocated machine instructions.

}  // namespace mirnext::codegen
