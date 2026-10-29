/* Licensed under the MIT License. Derived from MIR LICM and pressure relief. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/Motion.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/IR/SSA.hpp"
#include <algorithm>
#include <numeric>
#include <unordered_set>
namespace mirnext::transforms {
namespace {
using namespace ir;
using namespace analysis;
bool move (Opcode op) {
  return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov
         || op == Opcode::VMov;
}
Result<void> validate (Function &f, FunctionAnalysisManager &a, std::string_view pass) {
  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (f.symbol ().name ()) + ": " + std::string (pass) + ": " + reason};
  };
  if (&a.function () != &f) return invalid ("analysis owner mismatch");
  auto checked = f.verify ();
  if (!(checked).has_value ()) return checked;
  auto form = f.ssa_form ().get ();
  if (!form || !form->renamed () || form->conventional ())
    return invalid ("requires renamed nonconventional SSA");
  return {};
}
struct LoopMotion {
  Function &function;
  SSAForm &form;
  SSAUpdater rewriter;
  InstructionRewriter layout;
  FunctionGraph graph;
  LoopTree tree;
  std::vector<std::size_t> preheaders, parents;
  std::unordered_set<const Instruction *> invariant, marked;
  std::size_t changes = 0;
  LoopMotion (Function &f, FunctionGraph g, LoopTree t)
    : function (f),
      form (*const_cast<SSAForm *> (f.ssa_form ().get ())),
      rewriter (form),
      layout (f),
      graph (std::move (g)),
      tree (std::move (t)),
      preheaders (tree.nodes.size (), SIZE_MAX) {
    for (std::size_t b = 0; b < graph.blocks.size (); ++b) parents.push_back (tree.nodes[b].parent);
  }
  bool logical_empty (std::size_t b) {
    auto block = analysis::detail::NodeAccess::ir (graph.blocks[b]);
    return !block ? graph.blocks[b].kind != FunctionGraph::Kind::Prologue
                  : block->size () == 0
                      && ir::detail::ReferenceAccess::pointers (block->labels ()).empty ()
                      && ir::detail::ReferenceAccess::pointers (form.phis (*block)).empty ();
  }
  Result<void> preheader (std::size_t loop) {
    bool nested = false;
    auto children = tree.nodes[loop].children;
    for (auto node : children)
      if (tree.nodes[node].block == SIZE_MAX) {
        nested = true;
        auto checked = preheader (node);
        if (!(checked).has_value ()) return checked;
      }
    if (nested || loop == tree.root) return {};
    auto header = tree.nodes[tree.nodes[loop].entry].block, edge = SIZE_MAX;
    for (auto id : graph.incoming[header])
      if (!graph.back_edges[id]) {
        if (edge != SIZE_MAX) return {};
        edge = id;
      }
    if (edge == SIZE_MAX) return {};
    auto src = graph.edges[edge].source;
    auto from = analysis::detail::NodeAccess::ir (graph.blocks[src]),
         to = analysis::detail::NodeAccess::ir (graph.blocks[header]);
    if (graph.edges[edge].kind != EdgeKind::Fallthrough && from && from->size ()
        && std::prev (from->instructions ().end ())->opcode () != Opcode::Jmp)
      return {};
    if (!logical_empty (src) && !logical_empty (header)
        && (!from || !to || layout.next_block (*from) != to))
      return {};
    const Instruction *last
      = from && from->size () ? &*std::prev (from->instructions ().end ()) : nullptr;
    bool phi_tail
      = from && !ir::detail::ReferenceAccess::pointers (form.phis (*from)).empty ()
        && ir::detail::ReferenceAccess::pointers (form.phis (*from)).back ()->before ().get ()
             == nullptr;
    if (!last || phi_tail || last->opcode () == Opcode::Jmp
        || instruction_descriptor (last->opcode ()).get ()->flow == ControlFlow::Next) {
      preheaders[loop] = src;
      return {};
    }
    if (!from || !to)
      return Error{ErrorCode::InvalidArgument, "licm: missing canonical fallthrough blocks"};
    auto added = rewriter.split_fallthrough (*const_cast<BasicBlock *> (from),
                                             *const_cast<BasicBlock *> (to));
    if (!(added).has_value ()) return added.error ();
    auto b = graph.blocks.size (), parent = tree.nodes[loop].parent;
    analysis::detail::NodeAccess::indices (graph).emplace (&(added).value (), b);
    graph.blocks.push_back ({FunctionGraph::Kind::Body, (added).value ()});
    graph.incoming.emplace_back ();
    graph.outgoing.emplace_back ();
    graph.preorder.push_back (0);
    graph.rpost.push_back (0);
    graph.order.push_back (b);
    auto first = edge, second = graph.edges.size ();
    graph.edges[first] = {src, b, EdgeKind::Fallthrough, SIZE_MAX};
    graph.edges.push_back ({b, header, EdgeKind::Fallthrough, SIZE_MAX});
    graph.back_edges[first] = false;
    graph.back_edges.push_back (false);
    std::erase (graph.outgoing[src], edge);
    graph.outgoing[src].insert (graph.outgoing[src].begin (), first);
    *std::ranges::find (graph.incoming[header], edge) = second;
    graph.incoming[b].push_back (first);
    graph.outgoing[b].push_back (second);
    // Loop nodes and graph blocks have independent identities once new blocks
    // appear. Insert the new leaf without disturbing the original tree order.
    auto leaf = tree.nodes.size ();
    tree.nodes.push_back ({b, b, parent, {}});
    auto &siblings = tree.nodes[parent].children;
    auto source_leaf
      = std::ranges::find_if (siblings, [&] (auto n) { return tree.nodes[n].block == src; });
    if (source_leaf != siblings.end ())
      siblings.insert (std::next (source_leaf), leaf);
    else
      siblings.push_back (leaf);
    parents.push_back (parent);
    preheaders[loop] = b;
    ++changes;
    return {};
  }
  std::size_t parent_of (std::size_t block) const { return parents[block]; }
  std::size_t def_block (const SSAValue &value) const {
    if (value.instruction ().get ())
      return analysis::detail::NodeAccess::indices (graph).at (
        &value.instruction ().get ()->block ());
    if (value.phi ().get ())
      return analysis::detail::NodeAccess::indices (graph).at (&value.phi ().get ()->block ());
    return FunctionGraph::entry;
  }
  bool eligible (std::size_t loop, const Instruction &i, bool use_invariants) {
    auto op = i.opcode ();
    auto descriptor = instruction_descriptor (op).get ();
    if (descriptor->flow != ControlFlow::Next || descriptor->count == OperandCount::Call
        || op == Opcode::Alloca || op == Opcode::BStart || op == Opcode::BEnd
        || op == Opcode::VaStart || op == Opcode::VaArg || op == Opcode::VaBlockArg
        || op == Opcode::VaEnd || op == Opcode::Label || op == Opcode::Div || op == Opcode::Divs
        || op == Opcode::UDiv || op == Opcode::UDivs || op == Opcode::Mod || op == Opcode::Mods
        || op == Opcode::UMod || op == Opcode::UMods)
      return false;
    for (auto &operand : i.operands ())
      if (ir::detail::operand_access (operand).get<Memory> ()
          || (ir::detail::operand_access (operand).get<const Register *> ()
              && !(*ir::detail::operand_access (operand).get<const Register *> ())
                    ->hardware_binding ()
                    .empty ()))
        return false;
    for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i))) {
      auto &value = use->value ();
      if (use_invariants && value.instruction ().get ()
          && invariant.contains (value.instruction ().get ()))
        continue;
      auto parent = parent_of (def_block (value));
      bool outside = false;
      for (auto current = tree.nodes[loop].parent; current != SIZE_MAX;
           current = tree.nodes[current].parent)
        if (current == parent) {
          outside = true;
          break;
        }
      if (!outside) return false;
    }
    return true;
  }
  bool noninvariant_use (const Instruction &i) const {
    for (auto value : ir::detail::ReferenceAccess::pointers (form.definitions (i)))
      for (auto use : ir::detail::ReferenceAccess::pointers (value->uses ())) {
        auto site = std::get_if<SSAInstructionUse> (&use->site ());
        if (!site || !invariant.contains (ir::detail::NodeAccess::instruction (*(site))))
          return true;
      }
    return false;
  }
  void mark (const Instruction &i) {
    std::vector<const Instruction *> pending{&i};
    while (!pending.empty ()) {
      auto current = pending.back ();
      pending.pop_back ();
      if (!marked.insert (current).second || !invariant.contains (current)) continue;
      for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (*current)))
        if (auto definition = use->value ().instruction ().get ()) pending.push_back (definition);
    }
  }
  Result<void> process (std::size_t loop) {
    bool nested = false;
    for (auto node : tree.nodes[loop].children)
      if (tree.nodes[node].block == SIZE_MAX) {
        nested = true;
        auto checked = process (node);
        if (!(checked).has_value ()) return checked;
      }
    if (nested || loop == tree.root || preheaders[loop] == SIZE_MAX) return {};
    invariant.clear ();
    marked.clear ();
    std::vector<const Instruction *> order;
    for (auto node : tree.nodes[loop].children)
      if (auto b = tree.nodes[node].block;
          b != SIZE_MAX && analysis::detail::NodeAccess::ir (graph.blocks[b]))
        for (auto &i : analysis::detail::NodeAccess::ir (graph.blocks[b])->instructions ())
          if (eligible (loop, i, false)) {
            invariant.insert (&i);
            order.push_back (&i);
          }
    for (std::size_t p = 0; p < order.size (); ++p)
      for (auto value : ir::detail::ReferenceAccess::pointers (form.definitions (*order[p])))
        for (auto it = ir::detail::ReferenceAccess::pointers (value->uses ()).begin ();
             it != ir::detail::ReferenceAccess::pointers (value->uses ()).end (); ++it) {
          auto site = std::get_if<SSAInstructionUse> (&(*it)->site ());
          if (site && eligible (loop, *ir::detail::NodeAccess::instruction (*(site)), true)
              && invariant.insert (ir::detail::NodeAccess::instruction (*(site))).second)
            order.push_back (ir::detail::NodeAccess::instruction (*(site)));
        }
    for (auto it = order.rbegin (); it != order.rend (); ++it) {
      auto &i = **it;
      if (marked.contains (&i)) continue;
      if (i.opcode () == Opcode::Mul || i.opcode () == Opcode::Muls) {
        mark (i);
        continue;
      }
      bool profitable = !ir::detail::ReferenceAccess::pointers (form.inputs (i)).empty ();
      for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i)))
        if (auto def = use->value ().instruction ().get ();
            def && invariant.contains (def) && !marked.contains (def) && noninvariant_use (*def)) {
          profitable = false;
          break;
        }
      if (profitable) mark (i);
    }
    auto target = preheaders[loop];
    auto block = analysis::detail::NodeAccess::ir (graph.blocks[target]);
    for (auto i : order)
      if (marked.contains (i)) {
        if (!block)
          return Error{ErrorCode::InvalidArgument,
                       std::string (function.symbol ().name ())
                         + ": licm: invariant has no canonical preheader"};
        auto before = block->terminator ().get ();
        auto checked = rewriter.move_before (*const_cast<Instruction *> (i),
                                             *const_cast<BasicBlock *> (block), before);
        if (!(checked).has_value ()) return checked;
        ++changes;
      }
    return {};
  }
  void layout_indices () {
    std::vector<std::size_t> old{0, 1};
    for (std::size_t b = 2; b < graph.blocks.size (); ++b)
      if (!analysis::detail::NodeAccess::ir (graph.blocks[b])) old.push_back (b);
    for (auto &block : function.blocks ())
      old.push_back (analysis::detail::NodeAccess::indices (graph).at (&block));
    std::vector<std::size_t> indices (old.size ());
    for (std::size_t p = 0; p < old.size (); ++p) indices[old[p]] = p;
    FunctionGraph result;
    result.blocks.reserve (old.size ());
    for (auto b : old) {
      result.blocks.push_back (graph.blocks[b]);
      result.incoming.push_back (std::move (graph.incoming[b]));
      result.outgoing.push_back (std::move (graph.outgoing[b]));
      result.preorder.push_back (graph.preorder[b]);
      result.rpost.push_back (graph.rpost[b]);
      if (analysis::detail::NodeAccess::ir (graph.blocks[b]))
        analysis::detail::NodeAccess::indices (result).emplace (analysis::detail::NodeAccess::ir (
                                                                  graph.blocks[b]),
                                                                result.blocks.size () - 1);
    }
    result.edges = std::move (graph.edges);
    result.back_edges = std::move (graph.back_edges);
    for (auto &e : result.edges) {
      e.source = indices[e.source];
      e.destination = indices[e.destination];
    }
    for (auto b : graph.order) result.order.push_back (indices[b]);
    graph = std::move (result);
  }
};
}  // namespace
Result<PassResult> LICMPass::run (ir::MIRBuilder &edit,
                                  analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto checked = validate (function, analyses, name ());
  if (!(checked).has_value ()) return checked.error ();
  auto cfg = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!(cfg).has_value ()) return cfg.error ();
  auto loops = analyses.get<analysis::LoopAnalysis> ();
  if (!(loops).has_value ()) return loops.error ();
  auto tree = analyses.take_cached<analysis::LoopAnalysis> ();
  if (!(tree).has_value ()) return tree.error ();
  auto graph = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  LoopMotion state (function, std::move (*(graph).value ()), std::move (*(tree).value ()));
  checked = state.preheader (state.tree.root);
  if (!(checked).has_value ()) return checked.error ();
  checked = state.process (state.tree.root);
  if (!(checked).has_value ()) return checked.error ();
  checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  state.layout_indices ();
  analysis::enumerate (state.graph);
  checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move (state.graph));
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{state.changes, preserved};
}
Result<PassResult> PressureReliefPass::run (ir::MIRBuilder &edit,
                                            analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto checked = validate (function, analyses, name ());
  if (!(checked).has_value ()) return checked.error ();
  auto form = function.ssa_form ().get ();
  auto rewriter = SSAUpdater::get (function);
  if (!(rewriter).has_value ()) return rewriter.error ();
  std::size_t changes = 0;
  for (auto &block : function.blocks ())
    for (auto it = block.instructions ().begin (); it != block.instructions ().end ();) {
      auto &i = *it++;
      if (!move (i.opcode ())
          || !ir::detail::operand_access (i.operands ()[0]).get<const Register *> ()
          || ir::detail::operand_access (i.operands ()[1]).get<const Register *> ()
          || ir::detail::operand_access (i.operands ()[1]).get<Memory> ())
        continue;
      auto values = ir::detail::ReferenceAccess::pointers (form->definitions (i));
      if (values.size () != 1
          || ir::detail::ReferenceAccess::pointers (values[0]->uses ()).size () != 1)
        continue;
      auto &site = ir::detail::ReferenceAccess::pointers (values[0]->uses ()).front ()->site ();
      const Instruction *user = nullptr;
      if (auto ordinary = std::get_if<SSAInstructionUse> (&site))
        user = ir::detail::NodeAccess::instruction (*(ordinary));
      else if (auto hardware = std::get_if<SSAHardwareUse> (&site))
        user = ir::detail::NodeAccess::ret (*(hardware));
      if (!user || &user->block () == &block) continue;
      checked
        = rewriter.value ().move_before (i, *const_cast<BasicBlock *> (&user->block ()), user);
      if (!(checked).has_value ()) return checked.error ();
      ++changes;
    }
  checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{changes, preserved};
}
}  // namespace mirnext::transforms
