/* Licensed under the MIT License. MIR loop motion and SSA pressure relief. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
namespace mirnext::transforms {
class LICMPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "licm"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;
};
class PressureReliefPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "pressure-relief"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
