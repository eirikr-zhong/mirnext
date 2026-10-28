/* Licensed under the MIT License. Derived from MIR memory liveness and DSE. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/Transforms/AnalysisAccess.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <array>
namespace mirnext::transforms {
namespace {
using namespace ir;
bool move (Opcode op) {
  return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov;
}
bool store (const Instruction &i) {
  return move (i.opcode ()) && ir::detail::operand_access (i.operands ()[0]).get<Memory> ();
}
bool load (const Instruction &i) {
  return move (i.opcode ()) && ir::detail::operand_access (i.operands ()[1]).get<Memory> ();
}
std::size_t size (Type type) {
  switch (type.kind ()) {
  case Type::Kind::I8:
  case Type::Kind::U8: return 1;
  case Type::Kind::I16:
  case Type::Kind::U16: return 2;
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::F32: return 4;
  case Type::Kind::Pointer: return sizeof (std::uintptr_t);
  case Type::Kind::LongDouble: return sizeof (long double);
  default: return 8;
  }
}
struct State {
  Function &function;
  SSAForm &form;
  SSAUpdater rewriter;
  const analysis::FunctionGraph &graph;
  GVNResult info;
  std::vector<SparseBitSet> in, out, generated, killed;
  std::vector<std::array<std::optional<SparseBitSet>, 2>> reads;
  SparseBitSet all, nonstack;
  std::size_t changes = 0;
  State (Function &f, SSAForm &s, const analysis::FunctionGraph &g, GVNResult metadata)
    : function (f),
      form (s),
      rewriter (s),
      graph (g),
      info (std::move (metadata)),
      in (g.blocks.size ()),
      out (g.blocks.size ()),
      generated (g.blocks.size ()),
      killed (g.blocks.size ()) {
    reads.resize (info.memory.size ());
    for (std::size_t p = 1; p < info.memory.size (); ++p) {
      all.insert (p);
      if (!info.memory[p].alloca_flags) nonstack.insert (p);
    }
  }
  unsigned flags (const SSAValue &value) {
    auto found = detail::AnalysisAccess::numbers (info).find (&value);
    return found == detail::AnalysisAccess::numbers (info).end () ? 0 : found->second.alloca_flags;
  }
  unsigned address_flags (const Instruction &i) {
    for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i)))
      if (auto site = std::get_if<SSAInstructionUse> (&use->site ());
          site && site->operand == 1 && site->part == SSAOperandPart::MemoryBase)
        return flags (use->value ());
    return 0;
  }
  std::size_t location (const Instruction &i) {
    auto found = detail::AnalysisAccess::locations (info).find (&i);
    return found == detail::AnalysisAccess::locations (info).end () ? 0 : found->second;
  }
  void call_live (SparseBitSet &live, const Instruction &i) {
    bool escape = info.full_escape;
    auto first = ir::detail::NodeAccess::symbol (
                   *(ir::detail::operand_access (i.operands ()[0]).get<SymbolReference> ()))
                   ->signature ()
                   .get ()
                   ->results.size ()
                 + 1;
    for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i)))
      if (auto site = std::get_if<SSAInstructionUse> (&use->site ());
          site && site->operand >= first && flags (use->value ()))
        escape = true;
    live.union_with (escape ? all : nonstack);
  }
  bool intersects (std::size_t a, Type type, std::size_t b) {
    auto &x = info.memory[a];
    auto &y = info.memory[b];
    if (!(x.base).has_value () || !(y.base).has_value () || x.base != y.base) return true;
    // Unsigned distances preserve the intervals without signed disp+size overflow.
    return x.displacement - y.displacement < size (y.type)
           || y.displacement - x.displacement < size (type);
  }
  void read (const Instruction &i, SparseBitSet &live, SparseBitSet *kill) {
    auto nloc = location (i);
    if (!nloc) {
      live.union_with (all);
      return;
    }
    auto &memory = *ir::detail::operand_access (i.operands ()[1]).get<Memory> ();
    bool must = address_flags (i) & 2;
    // Memory classes and stack provenance stay fixed throughout DSE. Share the
    // alias/intersection result between gen/kill setup and the deletion walk.
    auto &needed = reads[nloc][must];
    if (!needed) {
      needed.emplace ();
      needed->insert (nloc);
      all.for_each ([&] (auto p) {
        auto &attr = info.memory[p];
        if ((!memory.alias.empty () && !attr.alias.empty () && memory.alias != attr.alias)
            || (!memory.nonalias.empty () && !attr.nonalias.empty ()
                && memory.nonalias == attr.nonalias))
          return;
        if (must && (attr.alloca_flags & 2) && !intersects (nloc, memory.type, p)) return;
        needed->insert (p);
      });
    }
    live.union_with (*needed);
    if (kill) kill->subtract (*needed);
  }
  Result<void> run () {
    for (auto &block : function.blocks ()) {
      auto b = analysis::detail::NodeAccess::indices (graph).at (&block);
      for (auto it = block.instructions ().end (); it != block.instructions ().begin ();) {
        auto &i = *--it;
        if (instruction_descriptor (i.opcode ()).get ()->count == OperandCount::Call)
          call_live (generated[b], i);
        if (store (i)) {
          if (auto p = location (i)) {
            generated[b].erase (p);
            killed[b].insert (p);
          }
        } else if (load (i))
          read (i, generated[b], &killed[b]);
      }
    }
    all.for_each ([&] (auto p) {
      if (!(info.memory[p].alloca_flags & 2)) {
        in[1].insert (p);
        out[1].insert (p);
      }
    });
    bool changed;
    do {
      changed = false;
      for (auto it = graph.order.rbegin (); it != graph.order.rend (); ++it) {
        auto b = *it;
        for (auto id : graph.outgoing[b])
          changed |= out[b].union_with (in[graph.edges[id].destination]);
        auto next = out[b];
        next.subtract (killed[b]);
        next.union_with (generated[b]);
        changed |= next != in[b];
        in[b] = std::move (next);
      }
    } while (changed);
    for (auto &block : function.blocks ()) {
      auto live = out[analysis::detail::NodeAccess::indices (graph).at (&block)];
      auto it = block.instructions ().end ();
      while (it != block.instructions ().begin ()) {
        auto &i = *--it;
        bool remove = false;
        if (instruction_descriptor (i.opcode ()).get ()->count == OperandCount::Call)
          call_live (live, i);
        if (store (i)) {
          if (auto p = location (i)) {
            remove = !live.contains (p);
            live.erase (p);
          }
        } else if (load (i))
          read (i, live, nullptr);
        if (remove) {
          auto following = it;
          ++following;
          detail::AnalysisAccess::locations (info).erase (&i);
          auto checked = rewriter.erase_instruction (i);
          if (!(checked).has_value ()) return checked;
          ++changes;
          it = following;
        }
      }
    }
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> DeadStoresPass::run (ir::MIRBuilder &edit,
                                        analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (function.symbol ().name ()) + ": dead-stores: " + reason};
  };
  if (&analyses.function () != &function) return invalid ("analysis owner mismatch");
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto form = function.ssa_form ().get ();
  if (!form || !form->renamed () || form->conventional ())
    return invalid ("requires renamed nonconventional SSA");
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  auto info = analyses.take_cached<GVNAnalysis> ();
  if (!(info).has_value ()) return info.error ();
  if (!(info).value ()) return invalid ("requires current GVN memory metadata");
  State state (function, *const_cast<ir::SSAForm *> (form), (graph).value (),
               std::move (*(info).value ()));
  checked = state.run ();
  if (!(checked).has_value ()) return checked.error ();
  checked = analyses.set<GVNAnalysis> (std::move (state.info));
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<GVNAnalysis> ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{state.changes, preserved};
}
}  // namespace mirnext::transforms
