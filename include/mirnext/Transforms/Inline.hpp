/* Licensed under the MIT License. Native MIR inlining on stable IR nodes. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"

namespace mirnext::transforms {
class InlinePass final : public FunctionPass {
 public:
  InlinePass ();
  ~InlinePass () override;
  std::string_view name () const noexcept override { return "inline"; }
  Result<void> initialize (ir::Context &) override;
  void finalize () noexcept override;
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;

 private:
  struct Symbols;
  std::unique_ptr<Symbols> symbols_;
};
}  // namespace mirnext::transforms
