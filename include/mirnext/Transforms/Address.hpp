/* Licensed under the MIT License. MIR address transformation and its required SSA exit. */
#pragma once
#include "mirnext/Transforms/SSA.hpp"

namespace mirnext::transforms {
// Consumes unrenamed SSA and returns register IR. Rebuild SSA before later SSA
// passes; the reference's partially invalid intermediate form is not published.
class AddressTransformPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "transform-addresses"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
