/* Licensed under the MIT License. Original MIR phase order without a shadow function body. */
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Transforms/Materialize.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/Inline.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Transforms/Motion.hpp"
#include "mirnext/Transforms/SSACombine.hpp"
#include "mirnext/Transforms/JumpOptimization.hpp"
#include <array>
namespace mirnext::transforms {
namespace {
constexpr std::array<std::string_view, 18> names{"materialize-stack-scopes",
                                                 "simplify",
                                                 "inline",
                                                 "build-cfg",
                                                 "clone-blocks",
                                                 "build-ssa",
                                                 "transform-addresses",
                                                 "gvn",
                                                 "copy-propagation",
                                                 "dead-stores",
                                                 "ssa-dce",
                                                 "build-loops",
                                                 "licm",
                                                 "pressure-relief",
                                                 "conventional-ssa",
                                                 "ssa-combine",
                                                 "destroy-ssa",
                                                 "jump-opt"};
std::optional<PassId> pass_id (std::string_view name) {
  for (std::size_t n = 0; n < names.size (); ++n)
    if (names[n] == name) return static_cast<PassId> (n);
  return {};
}
Result<target::Architecture> configuration (const OptimizationOptions &options) {
  if (static_cast<unsigned> (options.level) > 3)
    return Error{ErrorCode::InvalidArgument,
                 "optimize [configuration]: invalid optimization level"};
  auto native = target::AddressingRules::get ();
  if (!(native).has_value ()) return native.error ();
  auto requested = target::AddressingRules::get (options.target);
  if (!(requested).has_value ()) return requested.error ();
  if (requested.value ().architecture () != native.value ().architecture ())
    return Error{ErrorCode::InvalidArgument,
                 "optimize [configuration]: cross-target optimization is not supported"};
  return requested.value ().architecture ();
}
class BuildCFGPass final : public FunctionPass {
  bool prune_;

 public:
  explicit BuildCFGPass (bool prune) : prune_ (prune) {}
  std::string_view name () const noexcept override { return "build-cfg"; }
  Result<PassResult> run (ir::MIRBuilder &edit,
                          analysis::FunctionAnalysisManager &analyses) override {
    if (auto status = edit.status (); !status.has_value ()) return status.error ();
    if (!edit.function ())
      return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
    auto &function = *edit.function ();

    auto prepared = PrepareCFGPass (prune_).run (edit, analyses);
    if (!(prepared).has_value ()) return prepared.error ();
    analyses.invalidate (prepared.value ().preserved);
    auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
    if (!(graph).has_value ()) return graph.error ();
    prepared.value ().preserved.preserve<analysis::FunctionGraphAnalysis> ();
    return std::move ((prepared).value ());
  }
};
class BuildLoopsPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "build-loops"; }
  Result<PassResult> run (ir::MIRBuilder &edit,
                          analysis::FunctionAnalysisManager &analyses) override {
    if (auto status = edit.status (); !status.has_value ()) return status.error ();
    if (!edit.function ())
      return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
    auto &function = *edit.function ();

    auto loops = analyses.get<analysis::LoopAnalysis> ();
    if (!(loops).has_value ()) return loops.error ();
    return PassResult{0, analysis::PreservedAnalyses::all ()};
  }
};
bool addresses (const ir::Function &function) {
  for (auto &block : function.blocks ())
    for (auto &i : block.instructions ())
      if (i.opcode () == ir::Opcode::Addr || i.opcode () == ir::Opcode::Addr8
          || i.opcode () == ir::Opcode::Addr16 || i.opcode () == ir::Opcode::Addr32)
        return true;
  return false;
}
void middle_end (FunctionPassManager &passes, OptimizationLevel level, target::Architecture target,
                 bool address) {
  passes.add<BuildCFGPass> (level != OptimizationLevel::O0);
  if (level < OptimizationLevel::O2) return;
  if (!address) passes.add<BlockCloningPass> ();
  passes.add<BuildSSAPass> (!address);
  if (address) {
    // AddressTransformPass includes unrenamed SSA exit on the canonical body.
    passes.add<AddressTransformPass> ();
    passes.add<BlockCloningPass> ();
    passes.add<BuildSSAPass> ();
  }
  passes.add<GVNPass> ();
  passes.add<CopyPropagationPass> ();
  passes.add<DeadStoresPass> ();
  passes.add<SSADeadCodePass> ();
  passes.add<BuildLoopsPass> ();
  passes.add<LICMPass> ();
  passes.add<PressureReliefPass> ();
  passes.add<ConventionalSSAPass> ();
  passes.add<SSACombinePass> (target);
  passes.add<DestroySSAPass> ();
  passes.add<JumpOptimizationPass> ();
}
}  // namespace
std::string_view pass_name (PassId pass) noexcept {
  auto n = static_cast<unsigned> (pass);
  return n < names.size () ? names[n] : "invalid-pass";
}
struct OptimizedProgram::Impl {
  ir::Program context;
  OptimizationLevel level;
  target::Architecture target;
  ir::StringMap<ir::StringMap<const ir::Function *>> functions;
  std::vector<PassStatistics> statistics;
  Result<void> observe (const PassEvent &event, analysis::FunctionAnalysisManager *analyses,
                        const OptimizationOptions &options) {
    auto pass = pass_id (event.pass);
    if (!pass) return Error{ErrorCode::InvalidArgument, "optimize: unrecognized pipeline stage"};
    if (event.phase == PassEvent::Phase::Before && !options.observer) return {};
    PassStatistics count{*pass,
                         std::string (event.function.module ().name ()),
                         std::string (event.function.symbol ().name ()),
                         event.instructions_before,
                         event.instructions_after,
                         event.transformations};
    const PassStatistics *view = &count;
    if (event.phase == PassEvent::Phase::After) {
      statistics.push_back (std::move (count));
      view = &statistics.back ();
    }
    if (!options.observer) return {};
    return options.observer ({*view, event.phase, event.function,
                              analyses ? analyses->get_cached<analysis::FunctionGraphAnalysis> ()
                                       : OptionalRef<const analysis::FunctionGraph>{},
                              analyses ? analyses->get_cached<analysis::LoopAnalysis> ()
                                       : OptionalRef<const analysis::LoopTree>{}});
  }
  Result<void> run (const OptimizationOptions &options) {
    std::size_t count = 0;
    for (auto &module : context.modules ())
      for (auto &function : module.functions ()) {
        functions[module.name ()].emplace (function.symbol ().name (), &function);
        ++count;
      }
    statistics.reserve (count * (level >= OptimizationLevel::O2 ? 19 : 4));
    ModulePassManager frontend;
    frontend.add_function<MaterializeStackScopesPass> ();
    frontend.add_function<SimplifyPass> ();
    frontend.add_function<InlinePass> ();
    auto checked = frontend.run (context, {}, [&] (const PassEvent &event) {
      return observe (event, nullptr, options);
    });
    if (!(checked).has_value ()) return checked;
    FunctionPassManager scalar;
    middle_end (scalar, level, target, false);
    std::optional<FunctionPassManager> addressed;
    for (auto &module : context.modules ())
      for (auto &function : module.functions ()) {
        // Detect address-taking before removing unreachable blocks.
        bool address = level >= OptimizationLevel::O2 && addresses (function);
        if (address && !addressed) {
          addressed.emplace ();
          middle_end (*addressed, level, target, true);
        }
        analysis::FunctionAnalysisManager analyses (function);
        checked
          = (address ? *addressed : scalar).run (function, analyses, [&] (const PassEvent &event) {
              return observe (event, &analyses, options);
            });
        if (!(checked).has_value ()) return checked;
      }
    return {};
  }
};
OptimizedProgram::OptimizedProgram (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}
OptimizedProgram::OptimizedProgram (OptimizedProgram &&) noexcept = default;
OptimizedProgram &OptimizedProgram::operator= (OptimizedProgram &&) noexcept = default;
OptimizedProgram::~OptimizedProgram () = default;
const ir::Program &OptimizedProgram::program () const noexcept { return impl_->context; }
OptimizationLevel OptimizedProgram::level () const noexcept { return impl_->level; }
target::Architecture OptimizedProgram::target () const noexcept { return impl_->target; }
OptionalRef<const ir::Function> OptimizedProgram::find_function (
  std::string_view module, std::string_view name) const noexcept {
  if (!impl_) return {};
  if (auto found = impl_->functions.find (module); found != impl_->functions.end ())
    if (auto function = found->second.find (name); function != found->second.end ())
      return ::mirnext::detail::borrow_optional (function->second);
  auto owner = impl_->context.find_module (module).get ();
  auto symbol = owner ? owner->find_symbol (name).get () : nullptr;
  return ::mirnext::detail::borrow_optional (symbol ? symbol->function ().get () : nullptr);
}
std::span<const PassStatistics> OptimizedProgram::statistics () const noexcept {
  return impl_ ? std::span<const PassStatistics> (impl_->statistics)
               : std::span<const PassStatistics>{};
}
Result<void> OptimizedProgram::verify () const {
  if (!impl_) return Error{ErrorCode::InvalidArgument, "moved-from optimized program"};
  auto checked = impl_->context.verify ();
  if (!(checked).has_value ()) return checked;
  for (auto &module : impl_->context.modules ())
    for (auto &function : module.functions ()) {
      checked = function.require_register_ir ();
      if (!(checked).has_value ()) return checked;
      if (function.needs_materialization ())
        return Error{ErrorCode::InvalidArgument,
                     "optimized program contains unmaterialized stack scopes"};
    }
  return {};
}
std::string OptimizedProgram::dump () const {
  return impl_ ? ir::dump (impl_->context) : "<moved-from optimized program>\n";
}
Result<std::string> OptimizedProgram::write_text () const {
  auto checked = verify ();
  if (!(checked).has_value ()) return checked.error ();
  return serialization::write_text (impl_->context);
}
Result<std::vector<std::byte>> OptimizedProgram::write_binary (
  serialization::BinaryEncoding encoding) const {
  auto checked = verify ();
  if (!(checked).has_value ()) return checked.error ();
  return serialization::write_binary (impl_->context, encoding);
}
Result<OptimizedProgram> optimize (const ir::Program &source, OptimizationOptions options) {
  auto target = configuration (options);
  if (!(target).has_value ()) return target.error ();
  auto copied = ir::clone (source);
  if (!(copied).has_value ()) return copied.error ();
  auto impl = std::make_unique<OptimizedProgram::Impl> ();
  impl->context = std::move ((copied).value ());
  impl->level = options.level;
  impl->target = (target).value ();
  auto checked = impl->run (options);
  if (!(checked).has_value ()) return checked.error ();
  OptimizedProgram result (std::move (impl));
  checked = result.verify ();
  if (!(checked).has_value ()) return checked.error ();
  return result;
}
Result<OptimizedProgram> optimize (ir::Program &&source, OptimizationOptions options) {
  auto target = configuration (options);
  if (!(target).has_value ()) return target.error ();
  auto checked_source = source.verify ();
  if (!(checked_source).has_value ()) return checked_source.error ();
  auto impl = std::make_unique<OptimizedProgram::Impl> ();
  impl->context = std::move (source);
  impl->level = options.level;
  impl->target = (target).value ();
  auto checked = impl->run (options);
  if (!(checked).has_value ()) return checked.error ();
  OptimizedProgram result (std::move (impl));
  checked = result.verify ();
  if (!(checked).has_value ()) return checked.error ();
  return result;
}
Result<OptimizedProgram> optimize (const ir::Module &source, OptimizationOptions options) {
  auto target = configuration (options);
  if (!(target).has_value ()) return target.error ();
  auto copied = ir::clone (source);
  if (!(copied).has_value ()) return copied.error ();
  auto impl = std::make_unique<OptimizedProgram::Impl> ();
  impl->context = std::move ((copied).value ());
  impl->level = options.level;
  impl->target = (target).value ();
  auto checked = impl->run (options);
  if (!(checked).has_value ()) return checked.error ();
  OptimizedProgram result (std::move (impl));
  checked = result.verify ();
  if (!(checked).has_value ()) return checked.error ();
  return result;
}
}  // namespace mirnext::transforms
