/* Licensed under the MIT License. Owning machine-edge preservation regressions. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "EdgeSpillFixtures.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
struct Fixture {
  std::unique_ptr<MachineFunction> f;
  MachineRewriter rw;
  std::vector<MachineBasicBlock *> blocks;
  std::vector<MachineLabel *> labels;
  Fixture (unsigned count = 4)
    : f (std::make_unique<MachineFunction> ("native", "edge", Signature{},
                                            *target::TargetMachine::native ())),
      rw (*f) {
    for (unsigned b = 0; b < count; ++b) {
      auto &block = f->create_block ();
      auto label = f->create_label (block);
      REQUIRE (label);
      blocks.push_back (&block);
      labels.push_back (*label);
    }
  }
  MachineOperand phys (unsigned n = 0) { return MachineOperand (f->target ().registers ()[n].reg); }
  MachineOperand label (unsigned n) { return MachineOperand (*labels[n]); }
  MachineInstruction *emit (unsigned b, MachineOpcode code,
                            std::vector<MachineOperand> operands = {}) {
    auto i = rw.append (*blocks[b], code, std::move (operands));
    INFO ((i ? "" : i.error ().message));
    REQUIRE (i);
    return *i;
  }
  MachineControlFlow cfg () {
    auto g = MachineControlFlow::build (*f);
    REQUIRE (g);
    return std::move (*g);
  }
  std::size_t edge (const MachineControlFlow &g, unsigned from, unsigned to,
                    MachineControlFlow::EdgeKind kind = MachineControlFlow::EdgeKind::Branch) {
    for (std::size_t e = 0; e < g.edges.size (); ++e)
      if (g.blocks[g.edges[e].source] == blocks[from]
          && g.blocks[g.edges[e].destination] == blocks[to] && g.edges[e].kind == kind)
        return e;
    FAIL ("missing fixture edge");
    return 0;
  }
  MachineEdgeSplit split (const MachineControlFlow &g, std::size_t e) {
    auto r = split_machine_edge (*f, g, e);
    INFO ((r ? "" : r.error ().message));
    REQUIRE (r);
    REQUIRE (f->verify ());
    REQUIRE (r->cfg.verify (*f));
    return std::move (*r);
  }
};
MachineRewriteResult allocated_gap (Fixture &t, unsigned level) {
  auto a = t.f->create_register (Type::i64 ()), c = t.f->create_register (Type::i64 ());
  REQUIRE (a);
  REQUIRE (c);
  std::vector<target::PhysicalRegister> globals;
  for (auto &p : t.f->target ().registers ())
    if (!p.reserved && p.reg.encoding ()) globals.push_back (p.reg);
  REQUIRE (t.rw.set_global_registers (globals));
  t.emit (0, Opcode::Mov, {MachineOperand (**a), MachineOperand (std::int64_t (1))});
  t.emit (0, MachinePseudoOpcode::Use, std::vector<MachineOperand> (20, MachineOperand (**a)));
  t.emit (1, Opcode::Mov, {MachineOperand (**c), MachineOperand (std::int64_t (2))});
  for (unsigned n = 0; n < 3; ++n) t.emit (1, MachinePseudoOpcode::Use, {MachineOperand (**c)});
  t.emit (2, MachinePseudoOpcode::Use, {MachineOperand (**a)});
  t.emit (2, Opcode::Ret);
  auto state = t.f->state ();
  state.machinized = state.operands_legalized = true;
  REQUIRE (t.rw.set_state (state));
  auto g = MachineGraphAnalysis::build (*t.f);
  REQUIRE (g);
  auto l = MachineLiveness::analyze (*t.f, *g, {.optimization_level = level});
  REQUIRE (l);
  auto ranges = MachineLiveRanges::build (*t.f, *g, *l);
  REQUIRE (ranges);
  auto allocation = MachineAllocation::assign (*t.f, *g, *l, *ranges);
  REQUIRE (allocation);
  auto rewritten = rewrite_allocated (*t.f, *g, *l, *ranges, std::move (*allocation));
  INFO ((rewritten ? "" : rewritten.error ().message));
  REQUIRE (rewritten);
  return std::move (*rewritten);
}
}  // namespace
TEST_CASE ("owning edge edits reuse a single-incoming jump source without altering identity") {
  Fixture t;
  t.emit (0, MachinePseudoOpcode::Use);
  auto jump = t.emit (1, Opcode::Jmp, {t.label (2)});
  t.emit (2, MachinePseudoOpcode::Use);
  t.emit (3, Opcode::Ret);
  auto g = t.cfg ();
  auto before = t.f->dump ();
  auto r = t.split (g, t.edge (g, 1, 2));
  CHECK (r.block == t.blocks[1]);
  CHECK (t.f->dump () == before);
  CHECK (&jump->block () == t.blocks[1]);
}
TEST_CASE ("owning edge edits move jumps and label anchors without replacing their objects") {
  SUBCASE ("moving a jump from a multiple-incoming source") {
    Fixture t;
    t.emit (0, Opcode::Beq, {t.label (1), t.phys (), MachineOperand (std::int64_t (0))});
    auto jump = t.emit (1, Opcode::Jmp, {t.label (2)});
    auto id = jump->creation_order ();
    t.emit (2, MachinePseudoOpcode::Use);
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 1, 2));
    CHECK (r.block != t.blocks[1]);
    CHECK (&jump->block () == r.block);
    CHECK (jump->creation_order () == id);
    CHECK (t.blocks[1]->instructions ().begin () == t.blocks[1]->instructions ().end ());
  }
  SUBCASE ("moving a destination anchor with aliases") {
    Fixture t (5);
    t.emit (0, MachinePseudoOpcode::Use);
    auto extra = t.f->create_label (*t.blocks[3], "alias", true);
    REQUIRE (extra);
    auto branch
      = t.emit (1, Opcode::Beq, {t.label (3), t.phys (), MachineOperand (std::int64_t (0))});
    t.emit (2, Opcode::Jmp, {t.label (4)});
    t.emit (3, MachinePseudoOpcode::Use);
    t.emit (4, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 1, 3));
    CHECK (&t.labels[3]->block () == r.block);
    CHECK (&(*extra)->block () == t.blocks[3]);
    CHECK (*branch->operands ()[0].get<const MachineLabel *> () == t.labels[3]);
    CHECK ((*extra)->address_taken ());
  }
}
TEST_CASE ("owning edge edits reuse a label-only destination and split only fallthrough") {
  SUBCASE ("label-only destination") {
    Fixture t;
    t.emit (0, Opcode::Beq, {t.label (2), t.phys (), MachineOperand (std::int64_t (0))});
    t.emit (1, Opcode::Jmp, {t.label (3)});
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 0, 2));
    CHECK (r.block == t.blocks[2]);
  }
  SUBCASE ("conditional fallthrough") {
    Fixture t;
    auto branch
      = t.emit (0, Opcode::Beq, {t.label (2), t.phys (), MachineOperand (std::int64_t (0))});
    t.emit (1, MachinePseudoOpcode::Use);
    t.emit (2, MachinePseudoOpcode::Use);
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 0, 1, MachineControlFlow::EdgeKind::Fallthrough));
    CHECK (r.block != t.blocks[1]);
    CHECK (r.block->instructions ().begin ()->opcode ()
           == MachineOpcode (MachinePseudoOpcode::Use));
    CHECK (*branch->operands ()[0].get<const MachineLabel *> () == t.labels[2]);
  }
}
TEST_CASE ("owning critical edge edits preserve the other path and duplicate switch targets") {
  for (bool conditional_fallthrough : {false, true})
    for (bool switch_case : {false, true}) {
      Fixture t;
      MachineInstruction *branch;
      if (switch_case)
        branch = t.emit (0, Opcode::Switch, {t.phys (), t.label (2), t.label (3), t.label (2)});
      else
        branch
          = t.emit (0, Opcode::Beq, {t.label (2), t.phys (), MachineOperand (std::int64_t (0))});
      t.emit (1,
              conditional_fallthrough ? MachineOpcode (Opcode::Beq)
                                      : MachineOpcode (MachinePseudoOpcode::Use),
              conditional_fallthrough
                ? std::vector{t.label (3), t.phys (), MachineOperand (std::int64_t (1))}
                : std::vector<MachineOperand>{});
      t.emit (2, MachinePseudoOpcode::Use);
      t.emit (3, Opcode::Ret);
      auto original = t.labels[2];
      auto g = t.cfg ();
      auto r = t.split (g, t.edge (g, 0, 2,
                                   switch_case ? MachineControlFlow::EdgeKind::SwitchCase
                                               : MachineControlFlow::EdgeKind::Branch));
      CHECK (r.block != t.blocks[2]);
      CHECK (&original->block () == t.blocks[2]);
      CHECK (r.block->labels ().size () == 1);
      CHECK (&(*branch->operands ()[switch_case ? 1 : 0].get<const MachineLabel *> ())->block ()
             == r.block);
      if (switch_case) CHECK (*branch->operands ()[3].get<const MachineLabel *> () == original);
      CHECK (r.cfg.blocks.size () == g.blocks.size () + (conditional_fallthrough ? 2 : 1));
      auto preserved = MachineGraphAnalysis::build (*t.f, r.cfg);
      REQUIRE (preserved);
      CHECK (preserved->current (*t.f));
    }
}
TEST_CASE (
  "owning edge edits reject forged foreign stale and unsupported indirect graphs before editing") {
  Fixture t;
  t.emit (0, Opcode::Beq, {t.label (2), t.phys (), MachineOperand (std::int64_t (0))});
  t.emit (1, MachinePseudoOpcode::Use);
  t.emit (2, MachinePseudoOpcode::Use);
  t.emit (3, Opcode::Ret);
  auto g = t.cfg ();
  auto e = t.edge (g, 0, 2);
  auto original = t.f->dump ();
  for (unsigned n = 0; n < 6; ++n) {
    auto bad = g;
    if (!n) bad.edges[e].destination = 99;
    if (n == 1) bad.incoming.clear ();
    if (n == 2) bad.outgoing[2].push_back (e);
    if (n == 3) bad.edges[e].operand = 999;
    if (n == 4) bad.blocks[2] = nullptr;
    if (n == 5) bad.incoming[4].clear ();
    CHECK_FALSE (split_machine_edge (*t.f, bad, e));
    CHECK (t.f->dump () == original);
    CHECK_FALSE (MachineGraphAnalysis::build (*t.f, bad));
  }
  Fixture foreign;
  CHECK_FALSE (split_machine_edge (*foreign.f, g, e));
  CHECK_FALSE (split_machine_edge (*t.f, g, SIZE_MAX));
  CHECK_FALSE (split_machine_edge (*t.f, g, g.outgoing[0][0]));
  auto stale = g;
  REQUIRE (t.f->create_label (*t.blocks[2]));
  CHECK_FALSE (split_machine_edge (*t.f, stale, e));
  Fixture indirect;
  REQUIRE (indirect.rw.move_label (*indirect.labels[2], *indirect.blocks[2]));
  auto address = indirect.f->create_label (*indirect.blocks[2], "address", true);
  REQUIRE (address);
  auto other = indirect.f->create_label (*indirect.blocks[3], "other", true);
  REQUIRE (other);
  indirect.emit (0, Opcode::JmpIndirect, {indirect.phys ()});
  indirect.emit (1, MachinePseudoOpcode::Use);
  indirect.emit (2, MachinePseudoOpcode::Use);
  indirect.emit (3, Opcode::Ret);
  auto ig = indirect.cfg ();
  original = indirect.f->dump ();
  CHECK_FALSE (
    split_machine_edge (*indirect.f, ig,
                        indirect.edge (ig, 0, 2, MachineControlFlow::EdgeKind::Indirect)));
  CHECK (indirect.f->dump () == original);
}
TEST_CASE ("owning edge spills retain range ownership and original O0 through O3 conditions") {
  if (target::TargetMachine::native ()->architecture () != target::Architecture::AArch64) return;
  for (unsigned level = 0; level < 4; ++level) {
    Fixture t (3);
    auto rewrite = allocated_gap (t, level);
    auto original = t.f->dump ();
    // Every analysis used to construct rewrite has already been destroyed.
    auto result = split_allocated_spills (*t.f, std::move (rewrite));
    INFO ((result ? "" : result.error ().message));
    REQUIRE (result);
    REQUIRE (t.f->verify ());
    REQUIRE (result->cfg ().verify (*t.f));
    CHECK (result->current (*t.f));
    CHECK (t.f->state ().edge_spills_inserted);
    CHECK (result->placements ().size () == (level >= 2 ? 2 : 0));
    if (level < 2)
      CHECK (t.f->dump () == original);
    else {
      auto a = &*t.f->registers ().begin ();
      auto slot = result->rewrite_result ().spill_slot (*a);
      REQUIRE (slot);
      REQUIRE (*slot);
      CHECK (result->allocation ().stack_bytes () == 8);
      CHECK (result->placements ()[0] == MachineSpillPlacement{a, t.blocks[1], false, true});
      CHECK (result->placements ()[1] == MachineSpillPlacement{a, t.blocks[2], false, false});
      CHECK (t.blocks[1]->instructions ().begin ()->operands ()[0].get<MachineMemory> ());
      CHECK (t.blocks[2]->instructions ().begin ()->operands ()[1].get<MachineMemory> ());
    }
    CHECK_FALSE (split_allocated_spills (*t.f, result->rewrite_result ()));
    REQUIRE (t.f->create_label (*t.blocks[0]));
    CHECK_FALSE (result->current (*t.f));
  }
}
TEST_CASE ("owning edge spill preflight rejects stale and foreign rewrite ownership") {
  if (target::TargetMachine::native ()->architecture () != target::Architecture::AArch64) return;
  Fixture t (3);
  auto rewrite = allocated_gap (t, 2);
  Fixture foreign;
  auto before = t.f->dump ();
  CHECK_FALSE (split_allocated_spills (*foreign.f, rewrite));
  CHECK (t.f->dump () == before);
  REQUIRE (t.f->create_label (*t.blocks[0]));
  before = t.f->dump ();
  CHECK_FALSE (split_allocated_spills (*t.f, std::move (rewrite)));
  CHECK (t.f->dump () == before);
}
TEST_CASE ("owning allocated critical edge spills run with no reference runtime") {
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  if (target->architecture () != target::Architecture::AArch64) return;
  for (bool critical : {false, true})
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (critical);
      CAPTURE (level);
      auto source = serialization::parse_text (test::edge_spill_fixture (*target, critical));
      REQUIRE (source);
      auto &function = *(*source)->modules ().begin ()->functions ().begin ();
      auto machine = prepare_machine_ir (function, *target);
      REQUIRE (machine);
      REQUIRE (machinize (**machine));
      REQUIRE (legalize_operands (**machine));
      auto graph = MachineGraphAnalysis::build (**machine);
      REQUIRE (graph);
      REQUIRE (coalesce_registers (**machine, *graph, level));
      graph = MachineGraphAnalysis::build (**machine);
      REQUIRE (graph);
      auto live = MachineLiveness::analyze (**machine, *graph, {.optimization_level = level});
      REQUIRE (live);
      auto ranges = MachineLiveRanges::build (**machine, *graph, *live);
      REQUIRE (ranges);
      auto allocation = MachineAllocation::assign (**machine, *graph, *live, *ranges);
      REQUIRE (allocation);
      auto rewritten
        = rewrite_allocated (**machine, *graph, *live, *ranges, std::move (*allocation));
      REQUIRE (rewritten);
      auto blocks = rewritten->blocks ().size ();
      auto spills = split_allocated_spills (**machine, std::move (*rewritten));
      INFO ((spills ? "" : spills.error ().message));
      REQUIRE (spills);
      CHECK (spills->placements ().size () == (level < 2 ? 0 : critical ? 3 : 2));
      CHECK (spills->cfg ().blocks.size () == blocks + (critical && level >= 2 ? 1 : 0));
      REQUIRE (spills->cfg ().verify (**machine));
      REQUIRE ((*machine)->verify ());
    }
}
TEST_CASE ("owning block edits reject foreign nodes without changing either function") {
  Fixture a, b;
  auto i = a.emit (0, MachinePseudoOpcode::Use);
  auto before = a.f->dump (), other = b.f->dump ();
  CHECK_FALSE (a.rw.move_to (*i, *b.blocks[0]));
  CHECK_FALSE (b.rw.move_label (*a.labels[0], *b.blocks[0]));
  CHECK_FALSE (a.rw.create_block_before (*b.blocks[0]));
  CHECK_FALSE (a.rw.create_block_after (*b.blocks[0]));
  CHECK (a.f->dump () == before);
  CHECK (b.f->dump () == other);
}
