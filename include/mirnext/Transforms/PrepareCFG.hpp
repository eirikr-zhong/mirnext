/* Licensed under the MIT License. Canonicalize layout before optimization analyses. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class PrepareCFGPass final : public FunctionPass {
 public:
  explicit PrepareCFGPass (bool remove_unreachable = true)
    : remove_unreachable_ (remove_unreachable) {}
  std::string_view name () const noexcept override { return "prepare-cfg"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;

 private:
  bool remove_unreachable_;
};
}  // namespace mirnext::transforms
