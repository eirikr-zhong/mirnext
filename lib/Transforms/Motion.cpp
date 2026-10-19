/* Licensed under the MIT License. Derived from MIR LICM and pressure relief. */
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
  return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov;
}
Result<void> validate (Function &f, FunctionAnalysisManager &a, std::string_view pass) {
  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (f.symbol ().name ()) + ": " + std::string (pass) + ": " + reason};
  };
  if (&a.function () != &f) return invalid ("analysis owner mismatch");
  auto checked = f.verify ();
  if (!checked) return checked;
  auto form = f.ssa_form ();
  if (!form || !form->renamed () || form->conventional ())
    return invalid ("requires renamed nonconventional SSA");
  return {};
}
struct LoopMotion {
  Function &function;
  SSAForm &form;
  SSARewriter rewriter;
  IRRewriter layout;
  FunctionGraph graph;
  LoopTree tree;
  std::vector<std::size_t> preheaders, parents;
  std::unordered_set<const Instruction *> invariant, marked;
  std::size_t changes = 0;
  LoopMotion (Function &f, FunctionGraph g, LoopTree t)
    : function (f),
      form (*const_cast<SSAForm *> (f.ssa_form ())),
      rewriter (form),
      layout (f),
      graph (std::move (g)),
      tree (std::move (t)),
      preheaders (tree.nodes.size (), SIZE_MAX) {
    for (std::size_t b = 0; b < graph.blocks.size (); ++b) parents.push_back (tree.nodes[b].parent);
  }
  bool logical_empty (std::size_t b) {
    auto block = graph.blocks[b].ir;
    return !block ? graph.blocks[b].kind != FunctionGraph::Kind::Prologue
                  : block->size () == 0 && block->labels ().empty () && form.phis (*block).empty ();
  }
  Result<void> preheader (std::size_t loop) {
    bool nested = false;
    auto children = tree.nodes[loop].children;
    for (auto node : children)
      if (tree.nodes[node].block == SIZE_MAX) {
        nested = true;
        auto checked = preheader (node);
        if (!checked) return checked;
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
    auto from = graph.blocks[src].ir, to = graph.blocks[header].ir;
    if (graph.edges[edge].kind != EdgeKind::Fallthrough && from && from->size ()
        && std::prev (from->instructions ().end ())->opcode () != Opcode::Jmp)
      return {};
    if (!logical_empty (src) && !logical_empty (header)
        && (!from || !to || layout.next_block (*from) != to))
      return {};
    const Instruction *last
      = from && from->size () ? &*std::prev (from->instructions ().end ()) : nullptr;
    bool phi_tail
      = from && !form.phis (*from).empty () && form.phis (*from).back ()->before () == nullptr;
    if (!last || phi_tail || last->opcode () == Opcode::Jmp
        || instruction_descriptor (last->opcode ())->flow == ControlFlow::Next) {
      preheaders[loop] = src;
      return {};
    }
    if (!from || !to)
      return Error{ErrorCode::InvalidArgument, "licm: missing canonical fallthrough blocks"};
    auto added = rewriter.split_fallthrough (*const_cast<BasicBlock *> (from),
                                             *const_cast<BasicBlock *> (to));
    if (!added) return added.error ();
    auto b = graph.blocks.size (), parent = tree.nodes[loop].parent;
    graph.indices.emplace (*added, b);
    graph.blocks.push_back ({FunctionGraph::Kind::Body, *added});
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
    if (value.instruction ()) return graph.indices.at (&value.instruction ()->block ());
    if (value.phi ()) return graph.indices.at (&value.phi ()->block ());
    return FunctionGraph::entry;
  }
  bool eligible (std::size_t loop, const Instruction &i, bool use_invariants) {
    auto op = i.opcode ();
    auto descriptor = instruction_descriptor (op);
    if (descriptor->flow != ControlFlow::Next || descriptor->count == OperandCount::Call
        || op == Opcode::Alloca || op == Opcode::BStart || op == Opcode::BEnd
        || op == Opcode::VaStart || op == Opcode::VaArg || op == Opcode::VaBlockArg
        || op == Opcode::VaEnd || op == Opcode::Label || op == Opcode::Div || op == Opcode::Divs
        || op == Opcode::UDiv || op == Opcode::UDivs || op == Opcode::Mod || op == Opcode::Mods
        || op == Opcode::UMod || op == Opcode::UMods)
      return false;
    for (auto &operand : i.operands ())
      if (operand.get<Memory> ()
          || (operand.get<const Register *> ()
              && !(*operand.get<const Register *> ())->hardware_binding ().empty ()))
        return false;
    for (auto use : form.inputs (i)) {
      auto &value = use->value ();
      if (use_invariants && value.instruction () && invariant.contains (value.instruction ()))
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
    for (auto value : form.definitions (i))
      for (auto use : value->uses ()) {
        auto site = std::get_if<SSAInstructionUse> (&use->site ());
        if (!site || !invariant.contains (site->instruction)) return true;
      }
    return false;
  }
  void mark (const Instruction &i) {
    std::vector<const Instruction *> pending{&i};
    while (!pending.empty ()) {
      auto current = pending.back ();
      pending.pop_back ();
      if (!marked.insert (current).second || !invariant.contains (current)) continue;
      for (auto use : form.inputs (*current))
        if (auto definition = use->value ().instruction ()) pending.push_back (definition);
    }
  }
  Result<void> process (std::size_t loop) {
    bool nested = false;
    for (auto node : tree.nodes[loop].children)
      if (tree.nodes[node].block == SIZE_MAX) {
        nested = true;
        auto checked = process (node);
        if (!checked) return checked;
      }
    if (nested || loop == tree.root || preheaders[loop] == SIZE_MAX) return {};
    invariant.clear ();
    marked.clear ();
    std::vector<const Instruction *> order;
    for (auto node : tree.nodes[loop].children)
      if (auto b = tree.nodes[node].block; b != SIZE_MAX && graph.blocks[b].ir)
        for (auto &i : graph.blocks[b].ir->instructions ())
          if (eligible (loop, i, false)) {
            invariant.insert (&i);
            order.push_back (&i);
          }
    for (std::size_t p = 0; p < order.size (); ++p)
      for (auto value : form.definitions (*order[p]))
        for (auto it = value->uses ().begin (); it != value->uses ().end (); ++it) {
          auto site = std::get_if<SSAInstructionUse> (&(*it)->site ());
          if (site && eligible (loop, *site->instruction, true)
              && invariant.insert (site->instruction).second)
            order.push_back (site->instruction);
        }
    for (auto it = order.rbegin (); it != order.rend (); ++it) {
      auto &i = **it;
      if (marked.contains (&i)) continue;
      if (i.opcode () == Opcode::Mul || i.opcode () == Opcode::Muls) {
        mark (i);
        continue;
      }
      bool profitable = !form.inputs (i).empty ();
      for (auto use : form.inputs (i))
        if (auto def = use->value ().instruction ();
            def && invariant.contains (def) && !marked.contains (def) && noninvariant_use (*def)) {
          profitable = false;
          break;
        }
      if (profitable) mark (i);
    }
    auto target = preheaders[loop];
    auto block = graph.blocks[target].ir;
    for (auto i : order)
      if (marked.contains (i)) {
        if (!block)
          return Error{ErrorCode::InvalidArgument,
                       std::string (function.symbol ().name ())
                         + ": licm: invariant has no canonical preheader"};
        auto before = block->terminator ();
        auto checked = rewriter.move_before (*const_cast<Instruction *> (i),
                                             *const_cast<BasicBlock *> (block), before);
        if (!checked) return checked;
        ++changes;
      }
    return {};
  }
  void layout_indices () {
    std::vector<std::size_t> old{0, 1};
    for (std::size_t b = 2; b < graph.blocks.size (); ++b)
      if (!graph.blocks[b].ir) old.push_back (b);
    for (auto &block : function.blocks ()) old.push_back (graph.indices.at (&block));
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
      if (graph.blocks[b].ir)
        result.indices.emplace (graph.blocks[b].ir, result.blocks.size () - 1);
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
Result<PassResult> LICMPass::run (ir::Function &function,
                                  analysis::FunctionAnalysisManager &analyses) {
  auto checked = validate (function, analyses, name ());
  if (!checked) return checked.error ();
  auto cfg = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!cfg) return cfg.error ();
  auto loops = analyses.get<analysis::LoopAnalysis> ();
  if (!loops) return loops.error ();
  auto tree = analyses.take_cached<analysis::LoopAnalysis> ();
  if (!tree) return tree.error ();
  auto graph = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!graph) return graph.error ();
  LoopMotion state (function, std::move (**graph), std::move (**tree));
  checked = state.preheader (state.tree.root);
  if (!checked) return checked.error ();
  checked = state.process (state.tree.root);
  if (!checked) return checked.error ();
  checked = function.verify ();
  if (!checked) return checked.error ();
  state.layout_indices ();
  analysis::enumerate (state.graph);
  checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move (state.graph));
  if (!checked) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{state.changes, preserved};
}
Result<PassResult> PressureReliefPass::run (ir::Function &function,
                                            analysis::FunctionAnalysisManager &analyses) {
  auto checked = validate (function, analyses, name ());
  if (!checked) return checked.error ();
  auto form = function.ssa_form ();
  auto rewriter = SSARewriter::get (function);
  if (!rewriter) return rewriter.error ();
  std::size_t changes = 0;
  for (auto &block : function.blocks ())
    for (auto it = block.instructions ().begin (); it != block.instructions ().end ();) {
      auto &i = *it++;
      if (!move (i.opcode ()) || !i.operands ()[0].get<const Register *> ()
          || i.operands ()[1].get<const Register *> () || i.operands ()[1].get<Memory> ())
        continue;
      auto values = form->definitions (i);
      if (values.size () != 1 || values[0]->uses ().size () != 1) continue;
      auto &site = values[0]->uses ().front ()->site ();
      const Instruction *user = nullptr;
      if (auto ordinary = std::get_if<SSAInstructionUse> (&site))
        user = ordinary->instruction;
      else if (auto hardware = std::get_if<SSAHardwareUse> (&site))
        user = hardware->ret;
      if (!user || &user->block () == &block) continue;
      checked = rewriter->move_before (i, *const_cast<BasicBlock *> (&user->block ()), user);
      if (!checked) return checked.error ();
      ++changes;
    }
  checked = function.verify ();
  if (!checked) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{changes, preserved};
}
}  // namespace mirnext::transforms
