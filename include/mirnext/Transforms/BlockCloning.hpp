/* Licensed under the MIT License. Hot-block cloning into cold jump predecessors. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class BlockCloningPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "clone-blocks"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
