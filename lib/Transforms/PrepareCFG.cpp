/* Licensed under the MIT License. Stable list edits preserve every surviving identity. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
namespace mirnext::transforms {
Result<PassResult> PrepareCFGPass::run (ir::MIRBuilder &edit,
                                        analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto phase = function.require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (&analyses.function () != &function)
    return Error{ErrorCode::InvalidArgument, std::string (function.symbol ().name ())
                                               + ": prepare-cfg: analysis owner mismatch"};
  auto lowered = ir::materialize_stack_scopes (function);
  if (!(lowered).has_value ()) return lowered.error ();
  std::size_t changes = lowered.value ().saves + lowered.value ().restores;
  ir::InstructionRewriter rewriter (function);
  for (auto &block : function.blocks ()) {
    auto split = rewriter.separate_label_anchors (block);
    if (!(split).has_value ()) return split.error ();
    changes += (split).value ();
  }
  for (auto block = &*function.blocks ().begin (); block;) {
    auto next = rewriter.next_block (*block);
    if (next && !block->terminator ().get ()
        && ir::detail::ReferenceAccess::pointers (next->labels ()).empty ()
        && block->stack_scope ().get () == next->stack_scope ().get ()) {
      auto merged = rewriter.merge_fallthrough (*block);
      if (!(merged).has_value ()) return merged.error ();
      ++changes;
    } else
      block = next;
  }
  std::vector<ir::BasicBlock *> empty;
  for (auto &block : function.blocks ())
    if (block.size () == 0 && ir::detail::ReferenceAccess::pointers (block.labels ()).empty ())
      empty.push_back (&block);
  auto erased_empty = rewriter.erase_blocks (empty);
  if (!(erased_empty).has_value ()) return erased_empty.error ();
  changes += empty.size ();
  auto order = analysis::ConstructionOrderAnalysis::run (function);
  if (!(order).has_value ()) return order.error ();
  if (remove_unreachable_) {
    auto graph = analyses.get<analysis::ControlFlowAnalysis> ();
    if (!(graph).has_value ()) return graph.error ();
    std::vector<ir::BasicBlock *> dead;
    for (auto &block : function.blocks ())
      if (!graph.value ()
             .reachable[analysis::detail::NodeAccess::indices (((graph)).value ()).at (&block)])
        dead.push_back (&block);
    auto erased = rewriter.erase_blocks (dead);
    if (!(erased).has_value ()) return erased.error ();
    changes += dead.size ();
  }
  auto stored = analyses.set<analysis::ConstructionOrderAnalysis> (std::move ((order).value ()));
  if (!(stored).has_value ()) return stored.error ();
  auto preserved
    = changes ? analysis::PreservedAnalyses::none () : analysis::PreservedAnalyses::all ();
  preserved.preserve<analysis::ConstructionOrderAnalysis> ();
  return PassResult{changes, preserved};
}
}  // namespace mirnext::transforms
