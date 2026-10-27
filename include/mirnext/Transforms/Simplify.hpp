/* Licensed under the MIT License. MIR operand and control-flow simplification on canonical nodes.
 */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class SimplifyPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "simplify"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
