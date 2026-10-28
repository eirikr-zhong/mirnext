/* Licensed under the MIT License. Private resumable post-allocation state. */
#pragma once
#include "mirnext/CodeGen/PostAllocation.hpp"
namespace mirnext::codegen::detail {
struct PostAllocationEngine;
class PostAllocationState {
 public:
  static Result<PostAllocationState> create (MachineFunction &, MachineSpillResult,
                                             MachinePostOptions = {});
  PostAllocationState (PostAllocationState &&) noexcept;
  PostAllocationState &operator= (PostAllocationState &&) noexcept;
  ~PostAllocationState ();
  std::optional<MachinePostStage> next_stage () const;
  Result<void> step ();
  Result<MachinePostResult> finish ();

 private:
  explicit PostAllocationState (std::unique_ptr<PostAllocationEngine>);
  std::unique_ptr<PostAllocationEngine> state_;
};
}  // namespace mirnext::codegen::detail
