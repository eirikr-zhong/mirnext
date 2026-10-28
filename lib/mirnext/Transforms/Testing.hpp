/* Licensed under the MIT License. Private direct pass regression entry. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Analysis/Editing.hpp"
namespace mirnext::transforms::detail {
inline Result<void> run_pass (const FunctionPassManager &manager, ir::Program &program) {
  return manager.run (program);
}
inline Result<void> run_pass (const FunctionPassManager &manager, ir::Function &function,
                              analysis::FunctionAnalysisManager &analyses) {
  return manager.run (function, analyses);
}
inline Result<PassResult> run_pass (FunctionPass &pass, ir::Function &function,
                                    analysis::FunctionAnalysisManager &analyses) {
  ir::MIRBuilder batch (function);
  analysis::detail::EditScope active (analyses);
  auto result = pass.run (batch, analyses);
  auto finished = batch.finish ();
  if (!(finished).has_value ()) return finished.error ();
  return result;
}
template <class Pass>
  requires std::derived_from<std::remove_reference_t<Pass>, FunctionPass>
Result<PassResult> run_pass (Pass &&pass, ir::Function &function,
                             analysis::FunctionAnalysisManager &analyses) {
  return run_pass (static_cast<FunctionPass &> (pass), function, analyses);
}
}  // namespace mirnext::transforms::detail
