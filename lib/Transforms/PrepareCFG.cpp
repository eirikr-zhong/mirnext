/* Licensed under the MIT License. Stable list edits preserve every surviving identity. */
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
namespace mirnext::transforms {
Result<PassResult> PrepareCFGPass::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses) {
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  if (&analyses.function() != &function)
    return Error{ErrorCode::InvalidArgument, std::string(function.symbol().name()) + ": prepare-cfg: analysis owner mismatch"};
  auto lowered = ir::materialize_stack_scopes(function); if (!lowered) return lowered.error();
  std::size_t changes = lowered->saves + lowered->restores;
  ir::IRRewriter rewriter(function);
  for (auto &block : function.blocks()) {
    auto split = rewriter.separate_label_anchors(block); if (!split) return split.error(); changes += *split;
  }
  for (auto block = &*function.blocks().begin(); block;) {
    auto next = rewriter.next_block(*block);
    if (next && !block->terminator() && next->labels().empty() && block->stack_scope() == next->stack_scope()) {
      auto merged = rewriter.merge_fallthrough(*block); if (!merged) return merged.error(); ++changes;
    } else block = next;
  }
  std::vector<ir::BasicBlock *> empty;
  for (auto &block : function.blocks()) if (block.size() == 0 && block.labels().empty()) empty.push_back(&block);
  auto erased_empty = rewriter.erase_blocks(empty); if (!erased_empty) return erased_empty.error(); changes += empty.size();
  auto order = analysis::ConstructionOrderAnalysis::run(function); if (!order) return order.error();
  if (remove_unreachable_) {
    auto graph = analyses.get<analysis::ControlFlowAnalysis>(); if (!graph) return graph.error();
    std::vector<ir::BasicBlock *> dead;
    for (auto &block : function.blocks()) if (!(*graph)->reachable[(*graph)->indices.at(&block)]) dead.push_back(&block);
    auto erased = rewriter.erase_blocks(dead); if (!erased) return erased.error(); changes += dead.size();
  }
  auto stored = analyses.set<analysis::ConstructionOrderAnalysis>(std::move(*order)); if (!stored) return stored.error();
  auto preserved = changes ? analysis::PreservedAnalyses::none() : analysis::PreservedAnalyses::all();
  preserved.preserve<analysis::ConstructionOrderAnalysis>();
  return PassResult{changes, preserved};
}
}
