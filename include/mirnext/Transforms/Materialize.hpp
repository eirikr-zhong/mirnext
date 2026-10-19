/* Licensed under the MIT License. Explicit auxiliary IR materialization. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class MaterializeStackScopesPass final : public FunctionPass {
public:
  std::string_view name() const noexcept override { return "materialize-stack-scopes"; }
  Result<PassResult> run(ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}
