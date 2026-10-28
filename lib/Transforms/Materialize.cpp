/* Licensed under the MIT License. Runs directly on canonical IR nodes. */
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Transforms/Materialize.hpp"
namespace mirnext::transforms {
Result<PassResult> MaterializeStackScopesPass::run (ir::MIRBuilder &edit,
                                                    analysis::FunctionAnalysisManager &) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (!function.needs_materialization ()) return PassResult{0, analysis::PreservedAnalyses::all ()};
  auto result = ir::materialize_stack_scopes (function);
  if (!(result).has_value ()) return result.error ();
  return PassResult{result.value ().saves + result.value ().restores, {}};
}
}  // namespace mirnext::transforms
