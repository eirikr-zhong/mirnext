/* Licensed under the MIT License. Per-compilation state, without a runtime. */
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
namespace mirnext::codegen {
std::string_view stage_name(CompilationStage stage) {
  switch (stage) {
    case CompilationStage::PrepareMachineIR: return "prepare-machine-ir";
    case CompilationStage::Machinize: return "machinize";
    case CompilationStage::LegalizeOperands: return "legalize-operands";
    case CompilationStage::Coalesce: return "coalesce";
    case CompilationStage::Liveness: return "liveness";
    case CompilationStage::LiveRanges: return "live-ranges";
    case CompilationStage::Allocate: return "allocate";
    case CompilationStage::Rewrite: return "rewrite";
    case CompilationStage::EdgeSpills: return "edge-spills";
    case CompilationStage::StripProperties: return "strip-properties";
    case CompilationStage::PostLiveness: return "post-liveness";
    case CompilationStage::Combine: return "combine";
    case CompilationStage::DeadCode: return "dead-code";
    case CompilationStage::Frame: return "frame";
    case CompilationStage::Encode: return "encode";
  }
  return "invalid-compilation-stage";
}
namespace {
// All mutable state belongs to one invocation. Neither source IR nor a Context
// carries target state, and no intermediate object survives in the artifact.
struct CompilationState {
  const ir::Function &source;
  const target::TargetMachine &target;
  CompilationOptions options;
  CompilationStage stage = CompilationStage::PrepareMachineIR;
  std::unique_ptr<MachineFunction> machine;
  Error diagnostic(Error error) const {
    error.message = std::string(source.module().name()) + '/' + std::string(source.symbol().name())
      + " [" + std::string(target.name()) + '/' + std::string(stage_name(stage)) + "]: " + error.message;
    return error;
  }
  Result<void> observe(CompilationStage next,const CompiledFunction *compiled = nullptr) {
    stage = next;
    if (!options.observer) return {};
    return options.observer({stage,*machine,compiled});
  }
  Result<MachineFrameResult> run(MachinePreparationKind kind) {
    if (options.optimization_level > 3) return Error{ErrorCode::InvalidArgument,"optimization level must be O0 through O3"};
    if (kind != MachinePreparationKind::WholeFunction && kind != MachinePreparationKind::BlockVersioning)
      return Error{ErrorCode::InvalidArgument,"invalid machine preparation kind"};
    auto prepared = prepare_machine_ir(source,target); if (!prepared) return prepared.error(); machine = std::move(*prepared);
    auto &f = *machine;
#define CHECK_STAGE(expression) do { auto checked = (expression); if (!checked) return checked.error(); } while (false)
    CHECK_STAGE(observe(CompilationStage::PrepareMachineIR));
    stage = CompilationStage::Machinize; CHECK_STAGE(machinize(f)); CHECK_STAGE(observe(stage));
    stage = CompilationStage::LegalizeOperands; CHECK_STAGE(legalize_operands(f)); CHECK_STAGE(observe(stage));
    {
      stage = CompilationStage::Coalesce;
      auto graph = MachineGraphAnalysis::build(f); if (!graph) return graph.error();
      auto merged = coalesce_registers(f,*graph,options.optimization_level); if (!merged) return merged.error(); CHECK_STAGE(observe(stage));
    } // Release the conflict matrix before building allocation live ranges.
    auto rewritten = [&]() -> Result<MachineRewriteResult> {
      stage = CompilationStage::Liveness;
      auto graph = MachineGraphAnalysis::build(f); if (!graph) return graph.error();
      auto live = MachineLiveness::analyze(f,*graph,{.optimization_level=options.optimization_level}); if (!live) return live.error(); CHECK_STAGE(observe(stage));
      stage = CompilationStage::LiveRanges;
      auto ranges = MachineLiveRanges::build(f,*graph,*live); if (!ranges) return ranges.error(); CHECK_STAGE(observe(stage));
      stage = CompilationStage::Allocate;
      auto allocation = MachineAllocation::assign(f,*graph,*live,*ranges); if (!allocation) return allocation.error(); CHECK_STAGE(observe(stage));
      stage = CompilationStage::Rewrite;
      auto result = rewrite_allocated(f,*graph,*live,*ranges,std::move(*allocation)); if (!result) return result.error(); CHECK_STAGE(observe(stage));
      return std::move(*result);
    }(); if (!rewritten) return rewritten.error();
    // Input graph, liveness and ranges are now dead. The owning rewrite result
    // retains precisely the allocation facts needed by edge spills and frames.
    stage = CompilationStage::EdgeSpills;
    auto spills = split_allocated_spills(f,std::move(*rewritten)); if (!spills) return spills.error(); CHECK_STAGE(observe(stage));
    auto post = optimize_allocated_machine(f,std::move(*spills),{.remove_properties=kind == MachinePreparationKind::WholeFunction,.observer=[&](auto event,const auto &) -> Result<void> {
      switch (event) {
        case MachinePostStage::StripProperties: return observe(CompilationStage::StripProperties);
        case MachinePostStage::Liveness: return observe(CompilationStage::PostLiveness);
        case MachinePostStage::Combine: return observe(CompilationStage::Combine);
        case MachinePostStage::DeadCode: return observe(CompilationStage::DeadCode);
      }
      return Error{ErrorCode::InvalidArgument,"invalid post-allocation stage"};
    }}); if (!post) return post.error();
    stage = CompilationStage::Frame;
    auto frame = form_frame(f,std::move(*post),{.preserve_host_registers=options.preserve_host_registers,.no_red_zone=options.no_red_zone}); if (!frame) return frame.error(); CHECK_STAGE(observe(stage));
#undef CHECK_STAGE
    return std::move(*frame);
  }
};
}
Result<CompiledFunction> compile_function(const ir::Function &f,const target::TargetMachine &target,CompilationOptions options) {
  CompilationState state{f,target,std::move(options)};
  auto frame = state.run(MachinePreparationKind::WholeFunction); if (!frame) return state.diagnostic(frame.error());
  state.stage = CompilationStage::Encode;
  auto result = encode_machine(*state.machine,*frame,std::move(state.options.encoding)); if (!result) return state.diagnostic(result.error());
  auto observed = state.observe(state.stage,&*result); if (!observed) return state.diagnostic(observed.error());
  return std::move(*result);
}
Result<PreparedMachineFunction> prepare_machine_function(const ir::Function &f,const target::TargetMachine &target,
    CompilationOptions options,MachinePreparationKind kind) {
  CompilationState state{f,target,std::move(options)};
  auto frame = state.run(kind); if (!frame) return state.diagnostic(frame.error());
  return PreparedMachineFunction(std::move(state.machine),std::move(*frame),kind);
}
Result<void> PreparedMachineFunction::verify() const {
  if (!machine_) return Error{ErrorCode::InvalidArgument,"moved-from prepared machine function"};
  if (!machine_->state().frame_formed || !frame_.current(*machine_))
    return Error{ErrorCode::InvalidArgument,"invalid prepared machine phase or frame"};
  return machine_->verify();
}
}
