/* Licensed under the MIT License. Per-compilation state, without a runtime. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/PostState.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
namespace mirnext::codegen {
std::string_view stage_name (CompilationStage stage) {
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
struct MachineCompilation::Impl {
  std::unique_ptr<MachineFunction> machine;
  CompilationOptions options;
  MachinePreparationKind kind;
  CompilationStage stage = CompilationStage::PrepareMachineIR;
  std::optional<CompilationStage> following = CompilationStage::Machinize;
  std::optional<Error> error;
  bool consumed = false;
  std::optional<MachineGraphAnalysis> graph;
  std::optional<MachineLiveness> live;
  std::optional<MachineLiveRanges> ranges;
  std::optional<MachineAllocation> allocation;
  std::optional<MachineRewriteResult> rewritten;
  std::optional<MachineSpillResult> spills;
  std::optional<detail::PostAllocationState> post;
  std::optional<MachineFrameResult> frame;
  std::optional<CompiledFunction> compiled;
  Error diagnostic (Error e) const {
    e.message = std::string (machine->module_name ()) + "/" + std::string (machine->name ()) + " ["
                + std::string (machine->target ().name ()) + "/" + std::string (stage_name (stage))
                + "]: " + e.message;
    return e;
  }
  Result<void> observe () {
    if (!options.observer) return {};
    return options.observer ({stage, *machine,
                              compiled ? OptionalRef<const CompiledFunction> (*compiled)
                                       : OptionalRef<const CompiledFunction>{}});
  }
  Result<void> start_post () {
    if (post) return {};
    auto state
      = detail::PostAllocationState::create (*machine, std::move (*spills),
                                             {.remove_properties
                                              = kind == MachinePreparationKind::WholeFunction});
    spills.reset ();
    if (!(state).has_value ()) return state.error ();
    post.emplace (std::move ((state).value ()));
    return {};
  }
  CompilationStage after_post () const {
    auto next = post->next_stage ();
    if (!next) return CompilationStage::Frame;
    switch (*next) {
    case MachinePostStage::StripProperties: return CompilationStage::StripProperties;
    case MachinePostStage::Liveness: return CompilationStage::PostLiveness;
    case MachinePostStage::Combine: return CompilationStage::Combine;
    case MachinePostStage::DeadCode: return CompilationStage::DeadCode;
    }
    return CompilationStage::Frame;
  }
  Result<void> run_stage () {
    auto &f = *machine;
    switch (stage) {
    case CompilationStage::Machinize: {
      auto r = machinize (f);
      if (!(r).has_value ()) return r;
      following = CompilationStage::LegalizeOperands;
      break;
    }
    case CompilationStage::LegalizeOperands: {
      auto r = legalize_operands (f);
      if (!(r).has_value ()) return r;
      following = CompilationStage::Coalesce;
      break;
    }
    case CompilationStage::Coalesce: {
      auto topology = MachineGraphAnalysis::build (f);
      if (!(topology).has_value ()) return topology.error ();
      auto r = coalesce_registers (f, (topology).value (), options.optimization_level);
      if (!(r).has_value ()) return r.error ();
      following = CompilationStage::Liveness;
      break;
    }
    case CompilationStage::Liveness: {
      auto topology = MachineGraphAnalysis::build (f);
      if (!(topology).has_value ()) return topology.error ();
      graph.emplace (std::move ((topology).value ()));
      auto r
        = MachineLiveness::analyze (f, *graph, {.optimization_level = options.optimization_level});
      if (!(r).has_value ()) return r.error ();
      live.emplace (std::move ((r).value ()));
      following = CompilationStage::LiveRanges;
      break;
    }
    case CompilationStage::LiveRanges: {
      auto r = MachineLiveRanges::build (f, *graph, *live);
      if (!(r).has_value ()) return r.error ();
      ranges.emplace (std::move ((r).value ()));
      following = CompilationStage::Allocate;
      break;
    }
    case CompilationStage::Allocate: {
      auto r = MachineAllocation::assign (f, *graph, *live, *ranges);
      if (!(r).has_value ()) return r.error ();
      allocation.emplace (std::move ((r).value ()));
      following = CompilationStage::Rewrite;
      break;
    }
    case CompilationStage::Rewrite: {
      auto r = rewrite_allocated (f, *graph, *live, *ranges, std::move (*allocation));
      allocation.reset ();
      graph.reset ();
      live.reset ();
      ranges.reset ();
      if (!(r).has_value ()) return r.error ();
      rewritten.emplace (std::move ((r).value ()));
      following = CompilationStage::EdgeSpills;
      break;
    }
    case CompilationStage::EdgeSpills: {
      auto r = split_allocated_spills (f, std::move (*rewritten));
      rewritten.reset ();
      if (!(r).has_value ()) return r.error ();
      spills.emplace (std::move ((r).value ()));
      following = options.optimization_level < 2 && kind == MachinePreparationKind::WholeFunction
                    ? CompilationStage::StripProperties
                  : options.optimization_level ? CompilationStage::PostLiveness
                                               : CompilationStage::Frame;
      break;
    }
    case CompilationStage::StripProperties:
    case CompilationStage::PostLiveness:
    case CompilationStage::Combine:
    case CompilationStage::DeadCode: {
      auto ready = start_post ();
      if (!(ready).has_value ()) return ready;
      auto r = post->step ();
      if (!(r).has_value ()) return r;
      following = after_post ();
      break;
    }
    case CompilationStage::Frame: {
      auto ready = start_post ();
      if (!(ready).has_value ()) return ready;
      auto result = post->finish ();
      post.reset ();
      if (!(result).has_value ()) return result.error ();
      auto r = form_frame (f, std::move ((result).value ()),
                           {.preserve_host_registers = options.preserve_host_registers,
                            .no_red_zone = options.no_red_zone});
      if (!(r).has_value ()) return r.error ();
      frame.emplace (std::move ((r).value ()));
      following = CompilationStage::Encode;
      break;
    }
    case CompilationStage::Encode: {
      auto r = encode_machine (f, *frame, std::move (options.encoding));
      if (!(r).has_value ()) return r.error ();
      compiled.emplace (std::move ((r).value ()));
      following.reset ();
      break;
    }
    case CompilationStage::PrepareMachineIR:
      return Error{ErrorCode::InvalidArgument, "machine preparation already completed"};
    }
    return observe ();
  }
};
MachineCompilation::MachineCompilation (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}
MachineCompilation::MachineCompilation (MachineCompilation &&) noexcept = default;
MachineCompilation &MachineCompilation::operator= (MachineCompilation &&) noexcept = default;
MachineCompilation::~MachineCompilation () = default;
Result<MachineCompilation> MachineCompilation::create (const ir::Function &source,
                                                       const target::TargetMachine &target,
                                                       CompilationOptions options,
                                                       MachinePreparationKind kind) {
  auto invalid = [&] (std::string reason) -> Result<MachineCompilation> {
    return Error{ErrorCode::InvalidArgument, std::string (source.module ().name ()) + "/"
                                               + std::string (source.symbol ().name ()) + " ["
                                               + std::string (target.name ())
                                               + "/prepare-machine-ir]: " + reason};
  };
  if (options.optimization_level > 3) return invalid ("optimization level must be O0 through O3");
  if (kind != MachinePreparationKind::WholeFunction
      && kind != MachinePreparationKind::BlockVersioning)
    return invalid ("invalid machine preparation kind");
  auto machine = prepare_machine_ir (source, target);
  if (!(machine).has_value ()) return invalid (machine.error ().message);
  auto impl = std::make_unique<Impl> ();
  impl->machine = std::make_unique<MachineFunction> (std::move ((machine).value ()));
  impl->options = std::move (options);
  impl->kind = kind;
  auto observed = impl->observe ();
  if (!(observed).has_value ()) return impl->diagnostic (observed.error ());
  return MachineCompilation (std::move (impl));
}
const MachineFunction &MachineCompilation::function () const { return *impl_->machine; }
CompilationStage MachineCompilation::stage () const { return impl_->stage; }
std::optional<CompilationStage> MachineCompilation::next_stage () const {
  return impl_ && !impl_->error && !impl_->consumed ? impl_->following : std::nullopt;
}
OptionalRef<const MachineGraphAnalysis> MachineCompilation::graph () const {
  return impl_ && !impl_->consumed && impl_->graph
           ? OptionalRef<const MachineGraphAnalysis> (*impl_->graph)
           : OptionalRef<const MachineGraphAnalysis>{};
}
OptionalRef<const MachineLiveness> MachineCompilation::liveness () const {
  return impl_ && !impl_->consumed && impl_->live
           ? OptionalRef<const MachineLiveness> (*impl_->live)
           : OptionalRef<const MachineLiveness>{};
}
OptionalRef<const MachineLiveRanges> MachineCompilation::live_ranges () const {
  return impl_ && !impl_->consumed && impl_->ranges
           ? OptionalRef<const MachineLiveRanges> (*impl_->ranges)
           : OptionalRef<const MachineLiveRanges>{};
}
OptionalRef<const MachineAllocation> MachineCompilation::allocation () const {
  return impl_ && !impl_->consumed && impl_->allocation
           ? OptionalRef<const MachineAllocation> (*impl_->allocation)
           : OptionalRef<const MachineAllocation>{};
}
OptionalRef<const MachineFrameResult> MachineCompilation::frame () const {
  return impl_ && !impl_->consumed && impl_->frame
           ? OptionalRef<const MachineFrameResult> (*impl_->frame)
           : OptionalRef<const MachineFrameResult>{};
}
Result<void> MachineCompilation::status () const {
  if (!impl_ || impl_->consumed)
    return Error{ErrorCode::InvalidArgument, "moved-from or consumed machine compilation"};
  if (impl_->error) return *impl_->error;
  return impl_->machine->status ();
}
Result<MachineEditor> MachineCompilation::edit () {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  if (impl_->stage != CompilationStage::PrepareMachineIR)
    return impl_->diagnostic (
      {ErrorCode::InvalidArgument, "machine edits require the preparation stage"});
  return MachineEditor (*impl_->machine);
}
Result<void> MachineCompilation::step () {
  auto valid = status ();
  if (!(valid).has_value ()) return valid;
  if (!impl_->following)
    return Error{ErrorCode::InvalidArgument, "machine compilation has no remaining stages"};
  impl_->stage = *impl_->following;
  auto result = impl_->run_stage ();
  if (!(result).has_value ()) {
    impl_->error = impl_->diagnostic (result.error ());
    return *impl_->error;
  }
  return {};
}
Result<void> MachineCompilation::run_until (CompilationStage requested) {
  auto valid = status ();
  if (!(valid).has_value ()) return valid;
  if (static_cast<unsigned> (requested) > static_cast<unsigned> (CompilationStage::Encode)
      || requested < impl_->stage)
    return Error{ErrorCode::InvalidArgument, "invalid or already completed compilation stage"};
  while (impl_->stage != requested) {
    if (!impl_->following || *impl_->following > requested)
      return Error{ErrorCode::InvalidArgument,
                   "compilation stage is disabled for this configuration"};
    auto result = step ();
    if (!(result).has_value ()) return result;
  }
  return {};
}
Result<PreparedMachineFunction> MachineCompilation::prepare () {
  auto result = run_until (CompilationStage::Frame);
  if (!(result).has_value ()) return result.error ();
  impl_->consumed = true;
  return PreparedMachineFunction (std::move (impl_->machine), std::move (*impl_->frame),
                                  impl_->kind);
}
Result<CompiledFunction> MachineCompilation::finish () {
  auto result = run_until (CompilationStage::Encode);
  if (!(result).has_value ()) return result.error ();
  impl_->consumed = true;
  return std::move (*impl_->compiled);
}
Result<CompiledFunction> compile_function (const ir::Function &f,
                                           const target::TargetMachine &target,
                                           CompilationOptions options) {
  auto compilation = MachineCompilation::create (f, target, std::move (options));
  if (!(compilation).has_value ()) return compilation.error ();
  return compilation.value ().finish ();
}
Result<PreparedMachineFunction> prepare_machine_function (const ir::Function &f,
                                                          const target::TargetMachine &target,
                                                          CompilationOptions options,
                                                          MachinePreparationKind kind) {
  auto compilation = MachineCompilation::create (f, target, std::move (options), kind);
  if (!(compilation).has_value ()) return compilation.error ();
  return compilation.value ().prepare ();
}
Result<void> PreparedMachineFunction::verify () const {
  if (!machine_) return Error{ErrorCode::InvalidArgument, "moved-from prepared machine function"};
  if (!machine_->state ().frame_formed || !frame_.current (*machine_))
    return Error{ErrorCode::InvalidArgument, "invalid prepared machine phase or frame"};
  return machine_->verify ();
}
}  // namespace mirnext::codegen
