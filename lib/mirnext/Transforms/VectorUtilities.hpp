/* Licensed under the MIT License. Shared SSA access for vector transformations. */
#pragma once
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/IR/Vector.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
namespace mirnext::transforms::detail {
inline ir::SSAValue *vector_input (const ir::SSAForm &form, const ir::Instruction &i, std::size_t p,
                                   ir::SSAOperandPart part = ir::SSAOperandPart::Register) {
  for (auto &use : form.inputs (i))
    if (auto site = std::get_if<ir::SSAInstructionUse> (&use.site ());
        site && site->operand == p && site->part == part)
      return const_cast<ir::SSAValue *> (&use.value ());
  return nullptr;
}
inline ir::SSAValue *vector_output (const ir::SSAForm &form, const ir::Instruction &i) {
  auto definitions = form.definitions (i);
  return definitions.empty () ? nullptr : const_cast<ir::SSAValue *> (&definitions.front ());
}
inline analysis::PreservedAnalyses vector_preserved (std::size_t changes) {
  if (!changes) return analysis::PreservedAnalyses::all ();
  // Neither vector pass changes block identities, terminators or CFG edges.
  // Keep topology analyses while invalidating register/SSA-dependent caches.
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::ControlFlowAnalysis> ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  preserved.preserve<analysis::DominatorAnalysis> ();
  preserved.preserve<analysis::LoopAnalysis> ();
  return preserved;
}
inline Result<ir::SSAForm &> vector_form (ir::MIRBuilder &edit,
                                          analysis::FunctionAnalysisManager &analyses,
                                          std::string_view pass) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  auto function = edit.function ();
  if (!function || &analyses.function () != function)
    return Error{ErrorCode::InvalidArgument, std::string (pass) + ": analysis owner mismatch"};
  auto checked = function->verify ();
  if (!checked.has_value ()) return checked.error ();
  auto form = function->ssa_form ().get ();
  if (!form || !form->renamed () || form->conventional ())
    return Error{ErrorCode::InvalidArgument, std::string (function->symbol ().name ()) + ": "
                                               + std::string (pass)
                                               + ": requires renamed nonconventional SSA"};
  return *const_cast<ir::SSAForm *> (form);
}
}  // namespace mirnext::transforms::detail
