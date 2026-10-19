/* Licensed under the MIT License. Whole-function compilation on owning IR. */
#pragma once
#include "mirnext/CodeGen/CompiledFunction.hpp"
namespace mirnext::codegen {
enum class CompilationStage {
  PrepareMachineIR,
  Machinize,
  LegalizeOperands,
  Coalesce,
  Liveness,
  LiveRanges,
  Allocate,
  Rewrite,
  EdgeSpills,
  StripProperties,
  PostLiveness,
  Combine,
  DeadCode,
  Frame,
  Encode
};
std::string_view stage_name (CompilationStage);
struct CompilationSnapshot {
  CompilationStage stage;
  // All views are valid only during the observer call.
  const MachineFunction &function;
  const CompiledFunction *compiled = nullptr;
  std::string dump () const { return function.dump (); }
  std::size_t instruction_count () const { return function.instruction_count (); }
};
struct CompilationOptions {
  unsigned optimization_level = 2;
  bool preserve_host_registers = false;
  bool no_red_zone = false;
  MachineEncodingOptions encoding;
  std::function<Result<void> (const CompilationSnapshot &)> observer;
};
enum class MachinePreparationKind { WholeFunction, BlockVersioning };
// Owns the final machine body and the frame facts that borrow its stable nodes.
// Preparation retains no source IR or observer. Moving the owner keeps all
// machine identities and frame/CFG references valid.
class PreparedMachineFunction {
 public:
  PreparedMachineFunction (PreparedMachineFunction &&) noexcept = default;
  PreparedMachineFunction &operator= (PreparedMachineFunction &&) noexcept = default;
  PreparedMachineFunction (const PreparedMachineFunction &) = delete;
  PreparedMachineFunction &operator= (const PreparedMachineFunction &) = delete;
  bool valid () const noexcept { return bool (machine_); }
  // Accessors require a valid, non-moved-from owner.
  const MachineFunction &function () const { return *machine_; }
  const MachineFrameResult &frame () const { return frame_; }
  MachinePreparationKind kind () const { return kind_; }
  Result<void> verify () const;

 private:
  friend Result<PreparedMachineFunction> prepare_machine_function (const ir::Function &,
                                                                   const target::TargetMachine &,
                                                                   CompilationOptions,
                                                                   MachinePreparationKind);
  PreparedMachineFunction (std::unique_ptr<MachineFunction> machine, MachineFrameResult frame,
                           MachinePreparationKind kind)
    : machine_ (std::move (machine)), frame_ (std::move (frame)), kind_ (kind) {}
  std::unique_ptr<MachineFunction> machine_;
  MachineFrameResult frame_;
  MachinePreparationKind kind_;
};
// Runs the canonical pipeline through frame formation. BBV preserves property
// operations for subsequent specialization; no code is encoded or published.
Result<PreparedMachineFunction> prepare_machine_function (const ir::Function &,
                                                          const target::TargetMachine &,
                                                          CompilationOptions = {},
                                                          MachinePreparationKind
                                                          = MachinePreparationKind::WholeFunction);
// Input is verified, materialized, non-SSA register IR, normally from optimize.
// Does not mutate it, optimize it again, retain it or publish executable memory.
Result<CompiledFunction> compile_function (const ir::Function &, const target::TargetMachine &,
                                           CompilationOptions = {});
}  // namespace mirnext::codegen
