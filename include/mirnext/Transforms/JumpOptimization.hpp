/* Licensed under the MIT License. Canonical post-SSA jump cleanup. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class JumpOptimizationPass final : public FunctionPass {
public:
  std::string_view name() const noexcept override { return "jump-opt"; }
  Result<PassResult> run(ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}
