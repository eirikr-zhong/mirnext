/* Licensed under the MIT License. MIR maximal SSA, minimization and conventional SSA. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/IR/SSA.hpp"

namespace mirnext::transforms {
class BuildSSAPass final : public FunctionPass {
 public:
  explicit BuildSSAPass (bool rename = true) : rename_ (rename) {}
  std::string_view name () const noexcept override { return "build-ssa"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;

 private:
  bool rename_;
};
class ConventionalSSAPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "conventional-ssa"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
class DestroySSAPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "destroy-ssa"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
