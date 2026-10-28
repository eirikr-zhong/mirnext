/* Licensed under the MIT License. Combine comparisons and addresses in conventional SSA. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Target/Addressing.hpp"
namespace mirnext::transforms {
class SSACombinePass final : public FunctionPass {
 public:
  explicit SSACombinePass (target::Architecture architecture = target::Architecture::Native)
    : architecture_ (architecture) {}
  std::string_view name () const noexcept override { return "ssa-combine"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;

 private:
  target::Architecture architecture_;
};
}  // namespace mirnext::transforms
