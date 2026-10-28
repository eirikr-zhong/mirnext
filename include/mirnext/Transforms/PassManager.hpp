/* Licensed under the MIT License. Native pass execution and observation. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"
#include "mirnext/IR/Builder.hpp"
#include <concepts>
#include <functional>

namespace mirnext::transforms {
struct PassResult {
  std::size_t transformations = 0;
  analysis::PreservedAnalyses preserved;
};
class FunctionPass {
 public:
  virtual ~FunctionPass () = default;
  virtual std::string_view name () const noexcept = 0;
  virtual Result<void> initialize (ir::Program &) { return {}; }
  virtual void finalize () noexcept {}
  virtual Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) = 0;
};
struct PassEvent {
  enum class Phase { Before, After };
  const ir::Function &function;
  std::string_view pass;
  Phase phase;
  std::size_t instructions_before, instructions_after, transformations;
};
using PassObserver = std::function<Result<void> (const PassEvent &)>;
class FunctionPassManager {
 public:
  template <class Pass, class... Args>
    requires std::derived_from<Pass, FunctionPass>
  void add (Args &&...args) {
    passes_.push_back (std::make_unique<Pass> (std::forward<Args> (args)...));
  }
  Result<void> run (ir::Function &, analysis::FunctionAnalysisManager &, PassObserver = {}) const;
  // Run each pass over the whole scope before advancing to the next pass.
  Result<void> run (ir::Program &, PassObserver = {}) const;
  Result<void> run (ir::Module &, PassObserver = {}) const;

 private:
  std::vector<std::unique_ptr<FunctionPass>> passes_;
};

struct ModulePassEvent {
  const ir::Module &module;
  std::string_view pass;
  PassEvent::Phase phase;
  std::size_t instructions_before, instructions_after, transformations;
};
using ModulePassObserver = std::function<Result<void> (const ModulePassEvent &)>;
struct ModulePassContext {
  analysis::ModuleAnalysisManager &analyses;
  ir::MIRBuilder &builder;
  // Nested function passes share the same instrumentation as direct runs.
  const PassObserver &function_observer;
};
class ModulePass {
 public:
  virtual ~ModulePass () = default;
  virtual std::string_view name () const noexcept = 0;
  virtual Result<void> initialize (ir::Program &) { return {}; }
  virtual void finalize () noexcept {}
  virtual Result<PassResult> run (ir::Module &, ModulePassContext &) = 0;
};
class FunctionPassAdaptor final : public ModulePass {
 public:
  explicit FunctionPassAdaptor (std::unique_ptr<FunctionPass> pass)
    : owned_ (std::move (pass)), pass_ (owned_.get ()) {}
  explicit FunctionPassAdaptor (FunctionPass &pass) : pass_ (&pass) {}
  std::string_view name () const noexcept override {
    return pass_ ? pass_->name () : "empty-function-pass";
  }
  Result<void> initialize (ir::Program &context) override {
    if (!pass_) return Error{ErrorCode::InvalidArgument, "function pass adaptor requires a pass"};
    return pass_->initialize (context);
  }
  void finalize () noexcept override {
    if (pass_) pass_->finalize ();
  }
  Result<PassResult> run (ir::Module &, ModulePassContext &) override;

 private:
  std::unique_ptr<FunctionPass> owned_;
  FunctionPass *pass_;
};
class ModulePassManager {
 public:
  template <class Pass, class... Args>
    requires std::derived_from<Pass, ModulePass>
  void add (Args &&...args) {
    passes_.push_back (std::make_unique<Pass> (std::forward<Args> (args)...));
  }
  template <class Pass, class... Args>
    requires std::derived_from<Pass, FunctionPass>
  void add_function (Args &&...args) {
    add<FunctionPassAdaptor> (std::make_unique<Pass> (std::forward<Args> (args)...));
  }
  Result<void> run (ir::Module &, analysis::ModuleAnalysisManager &, ModulePassObserver = {},
                    PassObserver = {}) const;
  // Phase-major order: finish a pass over every module before starting the next.
  Result<void> run (ir::Program &, ModulePassObserver = {}, PassObserver = {}) const;

 private:
  Result<void> run_scope (ir::Program &, std::span<ir::Module *const>,
                          std::span<analysis::ModuleAnalysisManager *const>,
                          const ModulePassObserver &, const PassObserver &) const;
  std::vector<std::unique_ptr<ModulePass>> passes_;
};
}  // namespace mirnext::transforms
