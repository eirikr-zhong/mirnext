/* Licensed under the MIT License. Derived from MIR SSA dead code elimination. */
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/IR/SSA.hpp"
#include <array>
namespace mirnext::transforms {
namespace {
using namespace ir;
bool overflow (Opcode op) {
  return op == Opcode::Addo || op == Opcode::Addos || op == Opcode::Subo || op == Opcode::Subos
         || op == Opcode::Mulo || op == Opcode::Mulos || op == Opcode::UMulo
         || op == Opcode::UMulos;
}
struct State {
  struct Node {
    Instruction *instruction;
    SSAPhi *phi;
    std::size_t next = SIZE_MAX;
    bool alive = true;
  };
  Function &function;
  SSAForm &form;
  SSARewriter rewriter;
  std::vector<Node> nodes;
  std::unordered_map<const Instruction *, std::size_t> instructions;
  std::unordered_map<const SSAPhi *, std::size_t> phis;
  std::unordered_map<const SSAValue *, std::size_t> detached;
  std::array<std::pair<const SSAValue *, std::size_t>, 8> small_detached{};
  std::size_t small_count = 0;
  bool wide_inputs = false;
  std::vector<std::size_t> pending;
  std::vector<bool> queued;
  std::size_t changes = 0;
  State (Function &f, SSAForm &s) : function (f), form (s), rewriter (s) {
    auto count = f.instruction_count (),
         phi_count = static_cast<std::size_t> (std::ranges::distance (form.phis ()));
    nodes.reserve (count + phi_count);
    pending.reserve (count + phi_count);
    instructions.reserve (count);
    phis.reserve (phi_count);
    for (auto &block : f.blocks ()) {
      auto first = nodes.size ();
      std::unordered_map<const Instruction *, std::vector<SSAPhi *>> anchored;
      for (auto phi : form.phis (block)) anchored[phi->before ()].push_back (phi);
      auto add_phis = [&] (const Instruction *before) {
        auto found = anchored.find (before);
        if (found == anchored.end ()) return;
        for (auto phi : found->second) {
          phis.emplace (phi, nodes.size ());
          nodes.push_back ({nullptr, phi});
        }
      };
      for (auto &i : block.instructions ()) {
        add_phis (&i);
        instructions.emplace (&i, nodes.size ());
        nodes.push_back ({&i, nullptr});
      }
      add_phis (nullptr);
      for (auto p = first; p + 1 < nodes.size (); ++p) nodes[p].next = p + 1;
    }
    queued.resize (nodes.size ());
  }
  std::size_t next_live (std::size_t p) {
    auto result = p;
    while (result != SIZE_MAX && !nodes[result].alive) result = nodes[result].next;
    while (p != result) {
      auto next = nodes[p].next;
      nodes[p].next = result;
      p = next;
    }
    return result;
  }
  bool used (const SSAValue &value) const {
    if (!wide_inputs) {
      for (std::size_t p = 0; p < small_count; ++p)
        if (small_detached[p].first == &value)
          return value.uses ().size () != small_detached[p].second;
      return !value.uses ().empty ();
    }
    auto found = detached.find (&value);
    return value.uses ().size () != (found == detached.end () ? 0 : found->second);
  }
  bool dead (std::size_t p) {
    auto &node = nodes[p];
    if (!node.alive) return false;
    if (node.phi) return !used (node.phi->output ());
    auto &i = *node.instruction;
    auto op = i.opcode ();
    if (instruction_descriptor (op)->count == OperandCount::Call || op == Opcode::Alloca
        || op == Opcode::BStart || op == Opcode::VaStart || op == Opcode::VaArg)
      return false;
    auto defs = form.definitions (i);
    if (defs.empty ()) return false;
    for (auto value : defs)
      if (used (*value)) return false;
    if (overflow (op))
      for (auto n = next_live (node.next); n != SIZE_MAX; n = next_live (nodes[n].next)) {
        if (nodes[n].phi) break;
        auto following = nodes[n].instruction->opcode ();
        if (following == Opcode::Bo || following == Opcode::UBo || following == Opcode::Bno
            || following == Opcode::UBno)
          return false;
        if (following != Opcode::Mov && following != Opcode::Ext32 && following != Opcode::UExt32)
          break;
      }
    return true;
  }
  void enqueue (std::size_t p) {
    if (!queued[p] && dead (p)) {
      queued[p] = true;
      pending.push_back (p);
    }
  }
  Result<void> run () {
    for (std::size_t p = 0; p < nodes.size (); ++p) enqueue (p);
    while (!pending.empty ()) {
      auto p = pending.back ();
      pending.pop_back ();
      auto &node = nodes[p];
      auto inputs = node.phi ? node.phi->inputs () : form.inputs (*node.instruction);
      // Evaluate definitions after each logical edge removal, before erasing the
      // instruction so overflow flag barriers still constrain definition removal.
      detached.clear ();
      small_count = 0;
      wide_inputs = inputs.size () > small_detached.size ();
      for (auto use : inputs) {
        auto &value = use->value ();
        if (wide_inputs)
          ++detached[&value];
        else {
          std::size_t n = 0;
          while (n < small_count && small_detached[n].first != &value) ++n;
          if (n == small_count) small_detached[small_count++] = {&value, 0};
          ++small_detached[n].second;
        }
        if (value.instruction ())
          enqueue (instructions.at (value.instruction ()));
        else if (value.phi ())
          enqueue (phis.at (value.phi ()));
      }
      detached.clear ();
      small_count = 0;
      wide_inputs = false;
      auto result = node.phi ? rewriter.erase_phi (*node.phi)
                             : rewriter.erase_instruction (*node.instruction);
      if (!result) return result;
      node.alive = false;
      ++changes;
    }
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> SSADeadCodePass::run (ir::Function &function,
                                         analysis::FunctionAnalysisManager &analyses) {
  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (function.symbol ().name ()) + ": ssa-dce: " + reason};
  };
  if (&analyses.function () != &function) return invalid ("analysis owner mismatch");
  auto checked = function.verify ();
  if (!checked) return checked.error ();
  auto form = function.ssa_form ();
  if (!form || !form->renamed () || form->conventional ())
    return invalid ("requires renamed nonconventional SSA");
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!graph) return graph.error ();
  auto retained = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!retained) return retained.error ();
  State state (function, *const_cast<ir::SSAForm *> (form));
  checked = state.run ();
  if (!checked) return checked.error ();
  checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move (**retained));
  if (!checked) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{state.changes, preserved};
}
}  // namespace mirnext::transforms
