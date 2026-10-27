/* Licensed under the MIT License. MIR copy propagation and strength reduction on canonical SSA. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class CopyPropagationPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "copy-propagation"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
