/* Licensed under the MIT License. */
#include "mirnext/Transforms/PassManager.hpp"

namespace mirnext::transforms {
namespace {
Result<PassResult> execute(FunctionPass &pass, ir::Function &function, analysis::FunctionAnalysisManager &analyses, const PassObserver &observer) {
    auto contextual = [&](Error error) {
      error.message = std::string(function.module().name()) + "/" + std::string(function.symbol().name())
        + " [" + std::string(pass.name()) + "]: " + error.message;
      return error;
    };
    auto before = function.instruction_count();
    if (observer) {
      auto observed = observer({function, pass.name(), PassEvent::Phase::Before, before, before, 0});
      if (!observed) return contextual(observed.error());
    }
    auto result = pass.run(function, analyses);
    if (!result) { analyses.clear(); return contextual(result.error()); }
    analyses.invalidate(result->preserved);
    auto checked = function.verify(); if (!checked) return contextual(checked.error());
    if (observer) {
      auto observed = observer({function, pass.name(), PassEvent::Phase::After, before, function.instruction_count(), result->transformations});
      if (!observed) return contextual(observed.error());
    }
  return std::move(*result);
}
struct PassScope {
  FunctionPass &pass;
  ~PassScope() { pass.finalize(); }
};
}
Result<void> FunctionPassManager::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses, PassObserver observer) const {
  if (&analyses.function() != &function) return Error{ErrorCode::InvalidArgument, "pass manager: analysis owner mismatch"};
  auto checked = function.verify(); if (!checked) return checked;
  for (auto &pass : passes_) {
    PassScope scope{*pass};
    auto initialized = pass->initialize(function.module().context()); if (!initialized) return initialized;
    auto result = execute(*pass, function, analyses, observer); if (!result) return result.error();
  }
  return {};
}
Result<void> FunctionPassManager::run(ir::Context &context, PassObserver observer) const {
  ModulePassManager pipeline;
  for (auto &pass : passes_) pipeline.add<FunctionPassAdaptor>(*pass);
  return pipeline.run(context, {}, std::move(observer));
}
Result<void> FunctionPassManager::run(ir::Module &module, PassObserver observer) const {
  ModulePassManager pipeline;
  for (auto &pass : passes_) pipeline.add<FunctionPassAdaptor>(*pass);
  analysis::ModuleAnalysisManager analyses(module);
  return pipeline.run(module, analyses, {}, std::move(observer));
}
Result<PassResult> FunctionPassAdaptor::run(ir::Module &module, ModulePassContext &context) {
  if (!pass_) return Error{ErrorCode::InvalidArgument, "function pass adaptor requires a pass"};
  if (&context.analyses.module() != &module) return Error{ErrorCode::InvalidArgument, "function pass adaptor: analysis owner mismatch"};
  std::size_t transformations = 0;
  // A function pass may append helper functions without invalidating traversal.
  // Those helpers enter the next adapted pass, rather than recursively this one.
  std::vector<ir::Function *> functions;
  for (auto &function : module.functions()) functions.push_back(&function);
  for (auto function : functions) {
    auto analyses = context.analyses.function_analyses(*function); if (!analyses) return analyses.error();
    auto result = execute(*pass_, *function, **analyses, context.function_observer); if (!result) return result.error();
    transformations += result->transformations;
  }
  // Function caches were invalidated with each function's preservation set.
  // Module results survive only if their declared revision dependencies did.
  return PassResult{transformations, analysis::PreservedAnalyses::all()};
}
Result<void> ModulePassManager::run_scope(ir::Context &context, std::span<ir::Module *const> modules,
    std::span<analysis::ModuleAnalysisManager *const> analyses, const ModulePassObserver &observer, const PassObserver &function_observer) const {
  auto checked = context.verify(); if (!checked) return checked;
  auto clear = [&] { for (auto manager : analyses) manager->clear(); };
  for (auto &pass : passes_) {
    struct Scope { ModulePass &pass; ~Scope() { pass.finalize(); } } scope{*pass};
    auto contextual = [&](const ir::Module *module, Error error) {
      error.message = (module ? std::string(module->name()) + " " : "") + "[" + std::string(pass->name()) + "]: " + error.message;
      return error;
    };
    auto initialized = pass->initialize(context);
    if (!initialized) { clear(); return contextual(nullptr, initialized.error()); }
    for (std::size_t n = 0; n < modules.size(); ++n) {
      auto &module = *modules[n]; auto &manager = *analyses[n];
      auto count = [&] { std::size_t total = 0; for (auto &function : module.functions()) total += function.instruction_count(); return total; };
      // Only observation needs instruction totals; do not scan bodies otherwise.
      auto before = observer ? count() : 0;
      if (observer) {
        auto observed = observer({module, pass->name(), PassEvent::Phase::Before, before, before, 0});
        if (!observed) { clear(); return contextual(&module, observed.error()); }
      }
      ModulePassContext invocation{manager, function_observer};
      auto result = pass->run(module, invocation);
      if (!result) { clear(); return contextual(&module, result.error()); }
      manager.invalidate(result->preserved);
      checked = module.verify(); if (!checked) { clear(); return contextual(&module, checked.error()); }
      if (observer) {
        auto observed = observer({module, pass->name(), PassEvent::Phase::After, before, count(), result->transformations});
        if (!observed) { clear(); return contextual(&module, observed.error()); }
      }
    }
    // Inlining can also rename/export definitions in another module.
    for (auto manager : analyses) manager->invalidate(analysis::PreservedAnalyses::all());
    checked = context.verify(); if (!checked) { clear(); return contextual(nullptr, checked.error()); }
  }
  return {};
}
Result<void> ModulePassManager::run(ir::Module &module, analysis::ModuleAnalysisManager &analyses,
    ModulePassObserver observer, PassObserver function_observer) const {
  if (&analyses.module() != &module) return Error{ErrorCode::InvalidArgument, "module pass manager: analysis owner mismatch"};
  ir::Module *modules[] = {&module}; analysis::ModuleAnalysisManager *managers[] = {&analyses};
  return run_scope(module.context(), modules, managers, observer, function_observer);
}
Result<void> ModulePassManager::run(ir::Context &context, ModulePassObserver observer, PassObserver function_observer) const {
  std::vector<ir::Module *> modules;
  std::vector<std::unique_ptr<analysis::ModuleAnalysisManager>> owners;
  std::vector<analysis::ModuleAnalysisManager *> managers;
  for (auto &module : context.modules()) {
    modules.push_back(&module); owners.push_back(std::make_unique<analysis::ModuleAnalysisManager>(module)); managers.push_back(owners.back().get());
  }
  return run_scope(context, modules, managers, observer, function_observer);
}
} // namespace mirnext::transforms
