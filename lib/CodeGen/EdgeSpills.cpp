#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
/* MIR edge placement, Copyright (C) 2018-2024 Vladimir Makarov. MIT License.
   Instructions remain owned by their machine blocks throughout all edits. */
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/SpillEmitter.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace mirnext::codegen {
namespace {
using ir::Opcode;
using Kind = MachineControlFlow::EdgeKind;
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", edge-spills]: " + std::move (why)};
}
bool branch (const MachineInstruction *i) {
  if (!i) return false;
  auto code = i->opcode ();
  auto p = std::get_if<Opcode> (&code);
  return p && ir::instruction_descriptor (*p).get ()->flow != ir::ControlFlow::Next;
}
MachineInstruction *last (MachineBasicBlock *b) {
  auto body = b->instructions ();
  return body.begin () == body.end () ? nullptr : &*std::prev (body.end ());
}
}  // namespace
namespace detail {
struct MachineEdgeEditor {
  struct Edge;
  struct Node {
    MachineBasicBlock *block;
    std::size_t identity;
    std::list<Edge *> incoming;
    std::vector<Edge *> outgoing;
    SparseBitSet live_in, gen, kill;
  };
  struct Edge {
    Node *source, *destination;
    Kind kind;
    std::size_t operand, identity;
    bool at_source = false, at_destination = false;
    std::list<Edge *>::iterator destination_position;
  };
  MachineFunction &f;
  MachineRewriter rw;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<Edge>> edges;
  std::unordered_map<const MachineBasicBlock *, Node *> by_block;
  std::optional<Error> error;
  MachineEdgeEditor (MachineFunction &f, const MachineControlFlow &cfg) : f (f), rw (f) {
    for (auto b : detail::NodeAccess::blocks (cfg)) {
      auto n = std::make_unique<Node> ();
      n->block = const_cast<MachineBasicBlock *> (b);
      n->identity = nodes.size ();
      if (b) by_block.emplace (b, n.get ());
      nodes.push_back (std::move (n));
    }
    for (auto e : cfg.edges) {
      auto edge = std::make_unique<Edge> (Edge{nodes[e.source].get (), nodes[e.destination].get (),
                                               e.kind, e.operand, edges.size ()});
      edges.push_back (std::move (edge));
    }
    for (std::size_t b = 0; b < nodes.size (); ++b) {
      for (auto e : cfg.incoming[b]) {
        nodes[b]->incoming.push_back (edges[e].get ());
        edges[e]->destination_position = std::prev (nodes[b]->incoming.end ());
      }
      for (auto e : cfg.outgoing[b]) nodes[b]->outgoing.push_back (edges[e].get ());
    }
  }
  void fail (std::string why) {
    if (!error) error = invalid (f, std::move (why));
  }
  template <class T>
  bool check (const Result<T> &r) {
    if (!r.has_value ()) {
      fail (r.error ().message);
      return false;
    }
    return true;
  }
  Node *create (Node &anchor, bool after) {
    auto b = after ? rw.create_block_after (*anchor.block) : rw.create_block_before (*anchor.block);
    if (!check (b)) return &anchor;
    auto n = std::make_unique<Node> ();
    n->block = &(b).value ();
    n->identity = nodes.size ();
    auto p = n.get ();
    by_block.emplace (&(b).value (), p);
    nodes.push_back (std::move (n));
    return p;
  }
  void connect (Node &from, Node &to, Kind kind, std::size_t operand = SIZE_MAX) {
    auto e = std::make_unique<Edge> (Edge{&from, &to, kind, operand, edges.size ()});
    from.outgoing.push_back (e.get ());
    to.incoming.push_back (e.get ());
    e->destination_position = std::prev (to.incoming.end ());
    edges.push_back (std::move (e));
  }
  void redirect (Edge &e, Node &to) {
    // Retain incoming order with constant-time unlink/relink at high fan-in.
    to.incoming.splice (to.incoming.end (), e.destination->incoming, e.destination_position);
    e.destination = &to;
  }
  const MachineLabel *target (Edge &e) {
    auto tail = last (e.source->block);
    if (!tail || e.operand >= tail->operands ().size ()) return nullptr;
    auto label = detail::operand_access (tail->operands ()[e.operand]).get<const MachineLabel *> ();
    return label && &(*label)->block () == e.destination->block ? *label : nullptr;
  }
  Node *split (Edge &e) {
    auto src = e.source, dst = e.destination;
    if (!src->block || !dst->block) {
      fail ("cannot split an entry/exit boundary");
      return src;
    }
    auto tail = last (src->block);
    Node *created;
    if (src->outgoing.size () == 1 || e.kind == Kind::Fallthrough) {
      auto old_kind = e.kind;
      auto old_operand = e.operand;
      if (e.kind == Kind::Fallthrough) {
        created = create (*src, true);
        if (error) return src;
        check (rw.append (*created->block, MachinePseudoOpcode::Use, {}));
      } else if (src->incoming.size () == 1)
        return src;
      else {
        if (!tail
            || (tail->opcode () != MachineOpcode (Opcode::Jmp)
                && tail->opcode () != MachineOpcode (Opcode::Ret)
                && tail->opcode () != MachineOpcode (Opcode::JRet))) {
          fail ("single-destination split requires a jump/return");
          return src;
        }
        created = create (*src, true);
        if (error) return src;
        check (rw.move_to (*tail, *created->block));
      }
      redirect (e, *created);
      connect (*created, *dst, old_kind, old_operand);
      e.kind = Kind::Fallthrough;
      e.operand = SIZE_MAX;
    } else if (dst->incoming.size () == 1) {
      if (ir::detail::ReferenceAccess::pointers (dst->block->labels ()).empty ()) {
        fail ("jump destination lacks an anchor");
        return dst;
      }
      if (ir::detail::ReferenceAccess::pointers (dst->block->labels ()).size () == 1
          && !last (dst->block))
        return dst;
      auto label = target (e);
      if (!label) label = ir::detail::ReferenceAccess::pointers (dst->block->labels ())[0];
      created = create (*dst, false);
      if (error) return dst;
      check (rw.move_label (*const_cast<MachineLabel *> (label), *created->block));
      redirect (e, *created);
      connect (*created, *dst, Kind::Fallthrough);
    } else {
      if (ir::detail::ReferenceAccess::pointers (dst->block->labels ()).empty ()) {
        fail ("critical destination lacks an anchor");
        return src;
      }
      auto label = target (e);
      if (!label) {
        fail ("critical indirect edge cannot be redirected by the retained calling convention");
        return src;
      }
      Edge *incoming = nullptr;
      for (auto edge : dst->incoming)
        if (edge->kind == Kind::Fallthrough) {
          incoming = edge;
          break;
        }
      if (incoming) {
        auto other = incoming->source;
        if (other->outgoing.size () == 1) {
          if (branch (last (other->block))) {
            fail ("single fallthrough source ends in a branch");
            return other;
          }
          check (rw.append (*other->block, Opcode::Jmp,
                            {detail::NodeAccess::make_MachineOperand (*label)}));
          incoming->kind = Kind::Branch;
          incoming->operand = 0;
        } else {
          auto trampoline = create (*other, true);
          if (error) return other;
          check (rw.append (*trampoline->block, Opcode::Jmp,
                            {detail::NodeAccess::make_MachineOperand (*label)}));
          redirect (*incoming, *trampoline);
          connect (*trampoline, *dst, Kind::Branch, 0);
        }
      }
      created = create (*dst, false);
      if (error) return src;
      auto fresh = detail::access (f).create_label (*created->block);
      if (!check (fresh)) return src;
      auto ops = std::vector<MachineOperand> (tail->operands ().begin (), tail->operands ().end ());
      ops[e.operand] = detail::NodeAccess::make_MachineOperand ((fresh).value ());
      check (rw.replace (*tail, tail->opcode (), std::move (ops)));
      redirect (e, *created);
      connect (*created, *dst, Kind::Fallthrough);
    }
    return created;
  }
  Result<MachineControlFlow> finish () {
    if (error) return *error;
    auto canonical = MachineControlFlow::build (f);
    if (!(canonical).has_value ()) return invalid (f, canonical.error ().message);
    MachineControlFlow cfg;
    cfg.owner_ = &f;
    cfg.revision = f.revision ();
    detail::NodeAccess::blocks (cfg) = detail::NodeAccess::blocks (((canonical)).value ());
    std::map<const Node *, std::size_t> indices;
    indices[nodes[0].get ()] = 0;
    indices[nodes[1].get ()] = 1;
    for (std::size_t b = 2; b < detail::NodeAccess::blocks (cfg).size (); ++b)
      indices[by_block.at (detail::NodeAccess::blocks (cfg)[b])] = b;
    cfg.incoming.resize (indices.size ());
    cfg.outgoing.resize (indices.size ());
    for (auto &e : edges)
      cfg.edges.push_back (
        {indices.at (e->source), indices.at (e->destination), e->kind, e->operand});
    for (auto &n : nodes) {
      auto b = indices.at (n.get ());
      for (auto e : n->incoming) cfg.incoming[b].push_back (e->identity);
      for (auto e : n->outgoing) cfg.outgoing[b].push_back (e->identity);
    }
    auto sorted = [] (const MachineControlFlow &g) {
      std::vector<std::tuple<std::size_t, std::size_t, Kind, std::size_t>> v;
      for (auto e : g.edges) v.emplace_back (e.source, e.destination, e.kind, e.operand);
      std::sort (v.begin (), v.end ());
      return v;
    };
    if (sorted (cfg) != sorted ((canonical).value ()))
      return invalid (f, "edited topology disagrees with block-owned control flow");
    return cfg;
  }
};
struct EdgeSpillEngine {
  MachineFunction &f;
  MachineSpillResult result;
  MachineEdgeEditor edit;
  SparseBitSet used;
  MachineSpillEmitter spill;
  struct Element {
    std::size_t reg;
    MachineEdgeEditor::Edge *edge;
    bool store;
  };
  struct Placement {
    std::size_t reg;
    MachineEdgeEditor::Node *block;
    bool end, store;
    friend bool operator== (const Placement &, const Placement &) = default;
  };
  EdgeSpillEngine (MachineFunction &f, MachineRewriteResult r, const MachineControlFlow &cfg)
    : f (f),
      result (std::move (r)),
      edit (f, cfg),
      spill (f, result.rewrite_, used, "edge-spills") {
    for (auto p : result.allocation ().used_registers ()) used.insert (p.encoding ());
    for (std::size_t b = 0; b < edit.nodes.size (); ++b) {
      auto &n = *edit.nodes[b];
      auto &facts = result.rewrite_.blocks_[b];
      n.live_in = facts.live_in;
      n.gen = facts.spill_gen;
      n.kill = facts.spill_kill;
    }
  }
  Result<MachineSpillResult> run () {
    std::vector<Element> elements;
    const auto initial = edit.nodes.size ();
    if (result.allocation ().optimization_level () >= 2)
      for (std::size_t b = 2; b < initial; ++b) {
        auto &bb = *edit.nodes[b];
        for (auto e = bb.outgoing.rbegin (); e != bb.outgoing.rend (); ++e) {
          auto missing = (*e)->destination->gen;
          missing.subtract (bb.gen);
          missing.for_each ([&] (auto r) { elements.push_back ({r, *e, true}); });
        }
        for (auto e = bb.incoming.rbegin (); e != bb.incoming.rend (); ++e) {
          auto missing = (*e)->source->gen;
          missing.subtract (bb.gen);
          missing.for_each ([&] (auto r) {
            if (bb.live_in.contains (r)) elements.push_back ({r, *e, false});
          });
        }
      }
    for (auto el : elements)
      if (el.reg >= result.allocation ().locations_.size ()
          || !std::holds_alternative<target::PhysicalRegister> (
            detail::NodeAccess::storage (result.allocation ().locations_[el.reg])))
        return invalid (f, "edge-spilled value lacks physical assignment");
    std::sort (elements.begin (), elements.end (), [] (auto a, auto b) {
      if (a.edge != b.edge) return a.edge->identity < b.edge->identity;
      if (a.store != b.store) return a.store;
      return a.reg < b.reg;
    });
    if (!elements.empty ()) {
      for (auto &n : edit.nodes)
        if (n->block) {
          n->gen.clear ();
          n->kill.clear ();
        }
      for (auto &el : elements) {
        auto &e = *el.edge;
        e.at_source = el.store || !branch (last (e.source->block))
                      || last (e.source->block)->opcode () == MachineOpcode (Opcode::Jmp);
        e.at_destination = true;
        e.source->kill.insert (el.reg);
        e.destination->gen.insert (el.reg);
      }
      for (std::size_t p = 0; p < elements.size ();) {
        auto e = elements[p].edge;
        SparseBitSet regs;
        do {
          regs.insert (elements[p++].reg);
        } while (p < elements.size () && elements[p].edge == e);
        if (e->at_source) {
          bool common = regs == e->source->kill;
          for (auto edge : e->source->outgoing) common &= edge->at_source;
          if (!common)
            for (auto edge : e->source->outgoing) edge->at_source = false;
        }
        if (e->at_destination) {
          bool common = regs == e->destination->gen;
          for (auto edge : e->destination->incoming) common &= edge->at_destination;
          if (!common)
            for (auto edge : e->destination->incoming) edge->at_destination = false;
        }
      }
      for (auto &el : elements)
        if (el.edge->at_source && el.edge->at_destination) {
          if (el.edge->source->outgoing.size () == 1)
            el.edge->at_source = false;
          else if (el.edge->destination->incoming.size () == 1)
            el.edge->at_destination = false;
        }
    }
    std::vector<Placement> placements;
    for (auto &el : elements) {
      auto e = el.edge;
      auto src = e->source, dst = e->destination;
      MachineEdgeEditor::Node *b;
      bool end = false;
      if (e->at_source) {
        b = src;
        end = true;
      } else if (e->at_destination)
        b = dst;
      else if (src->identity >= initial)
        b = src;
      else if (dst->identity >= initial)
        b = dst;
      else
        b = edit.split (*e);
      if (edit.error) return *edit.error;
      placements.push_back ({el.reg, b, end, el.store});
    }
    std::sort (placements.begin (), placements.end (), [] (auto a, auto b) {
      if (a.block != b.block) return a.block->identity < b.block->identity;
      if (a.end != b.end) return !a.end;
      if (a.store != b.store) return a.end ? a.store : !a.store;
      return a.reg < b.reg;
    });
    placements.erase (std::unique (placements.begin (), placements.end ()), placements.end ());
    for (auto p : placements) {
      auto r = std::get<const MachineVirtualRegister *> (
        detail::NodeAccess::storage (result.allocation ().registers ()[p.reg]));
      auto &b = *p.block->block;
      auto body = b.instructions ();
      auto anchor = p.end ? last (&b) : body.begin () == body.end () ? nullptr : &*body.begin ();
      // Labels are anchors outside the instruction list. With a leading label,
      // each subsequent start insertion precedes the previous generated code.
      bool after = p.end && !branch (anchor);
      auto emitted = spill.preserve (r, b, anchor, after, !p.store);
      if (!(emitted).has_value ()) return emitted.error ();
      result.placements_.push_back (
        detail::NodeAccess::make_MachineSpillPlacement (r, &b, p.end, p.store));
    }
    for (auto p : result.allocation ().global_registers ()) used.erase (p.encoding ());
    auto &a = result.rewrite_.allocation_;
    a.used_registers_.clear ();
    used.for_each ([&] (auto n) { a.used_registers_.push_back (f.target ().registers ()[n].reg); });
    auto state = f.state ();
    state.edge_spills_inserted = true;
    auto changed = edit.rw.set_state (state);
    if (!(changed).has_value ()) return changed.error ();
    auto cfg = edit.finish ();
    if (!(cfg).has_value ()) return cfg.error ();
    result.cfg_ = std::move ((cfg).value ());
    result.rewrite_.blocks_.clear ();
    a.spill_.clear ();
    for (std::size_t b = 0; b < detail::NodeAccess::blocks (result.cfg_).size (); ++b) {
      auto node = b < 2 ? edit.nodes[b].get ()
                        : edit.by_block.at (detail::NodeAccess::blocks (result.cfg_)[b]);
      result.rewrite_.blocks_.push_back (
        {::mirnext::detail::borrow_optional (node->block), node->live_in, node->gen, node->kill});
      a.spill_.push_back ({node->gen, node->kill});
    }
    a.revision_ = f.revision ();
    return std::move (result);
  }
};
}  // namespace detail
Result<MachineEdgeSplit> split_machine_edge (MachineFunction &f, const MachineControlFlow &cfg,
                                             std::size_t edge) {
  if (!cfg.current (f) || edge >= cfg.edges.size ())
    return invalid (f, "stale/foreign CFG or invalid edge");
  auto valid = cfg.verify (f);
  if (!(valid).has_value ()) return invalid (f, valid.error ().message);
  detail::MachineEdgeEditor editor (f, cfg);
  auto b = editor.split (*editor.edges[edge]);
  if (editor.error) return *editor.error;
  auto result = editor.finish ();
  if (!(result).has_value ()) return result.error ();
  return detail::NodeAccess::make_MachineEdgeSplit (b->block, std::move ((result).value ()));
}
Result<MachineSpillResult> split_allocated_spills (MachineFunction &f,
                                                   MachineRewriteResult rewrite) {
  if (f.target ().architecture () != target::Architecture::AArch64
      && f.target ().architecture () != target::Architecture::X86_64)
    return invalid (f, "unsupported target");
  if (!rewrite.current (f) || !f.state ().operands_rewritten || f.state ().edge_spills_inserted)
    return invalid (f, "invalid phase or stale/foreign rewrite result");
  auto cfg = MachineControlFlow::build (f);
  if (!(cfg).has_value ()) return invalid (f, cfg.error ().message);
  if (detail::NodeAccess::blocks (((cfg)).value ()).size () != rewrite.blocks ().size ())
    return invalid (f, "rewrite facts disagree with block layout");
  for (std::size_t b = 0; b < detail::NodeAccess::blocks (((cfg)).value ()).size (); ++b)
    if (detail::NodeAccess::blocks (((cfg)).value ())[b] != rewrite.blocks ()[b].block.get ())
      return invalid (f, "foreign rewrite block");
  detail::EdgeSpillEngine engine (f, std::move (rewrite), (cfg).value ());
  return engine.run ();
}
}  // namespace mirnext::codegen
