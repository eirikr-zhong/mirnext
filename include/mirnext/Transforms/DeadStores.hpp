/* Licensed under the MIT License. MIR memory liveness and dead store elimination. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class DeadStoresPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "dead-stores"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
