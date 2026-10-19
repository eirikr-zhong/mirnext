/* Licensed under the MIT License. Runs directly on canonical IR nodes. */
#include "mirnext/Transforms/Materialize.hpp"
namespace mirnext::transforms {
Result<PassResult> MaterializeStackScopesPass::run(ir::Function &function, analysis::FunctionAnalysisManager &) {
  if (!function.needs_materialization()) return PassResult{0, analysis::PreservedAnalyses::all()};
  auto result = ir::materialize_stack_scopes(function); if (!result) return result.error();
  return PassResult{result->saves + result->restores, {}};
}
}
