/* Licensed under the MIT License. MIR SSA dead code elimination. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class SSADeadCodePass final : public FunctionPass {
public:
  std::string_view name() const noexcept override { return "ssa-dce"; }
  Result<PassResult> run(ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}
