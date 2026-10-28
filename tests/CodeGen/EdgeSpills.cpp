/* Licensed under the MIT License. Owning machine-edge preservation regressions. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
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
                                            (target::TargetMachine::native ()).value ())),
      rw (*f) {
    for (unsigned b = 0; b < count; ++b) {
      auto &block = mirnext::codegen::detail::access (*(f)).create_block ();
      auto label = mirnext::codegen::detail::access (*(f)).create_label (block);
      REQUIRE ((label).has_value ());
      blocks.push_back (&block);
      labels.push_back (&(label).value ());
    }
  }
  MachineOperand phys (unsigned n = 0) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
      f->target ().registers ()[n].reg);
  }
  MachineOperand label (unsigned n) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (*labels[n]);
  }
  MachineInstruction *emit (unsigned b, MachineOpcode code,
                            std::vector<MachineOperand> operands = {}) {
    auto i = rw.append (*blocks[b], code, std::move (operands));
    INFO (((i).has_value () ? "" : i.error ().message));
    REQUIRE ((i).has_value ());
    return &(i).value ();
  }
  MachineControlFlow cfg () {
    auto g = MachineControlFlow::build (*f);
    REQUIRE ((g).has_value ());
    return std::move ((g).value ());
  }
  std::size_t edge (const MachineControlFlow &g, unsigned from, unsigned to,
                    MachineControlFlow::EdgeKind kind = MachineControlFlow::EdgeKind::Branch) {
    for (std::size_t e = 0; e < g.edges.size (); ++e)
      if (mirnext::codegen::detail::NodeAccess::blocks (g)[g.edges[e].source] == blocks[from]
          && mirnext::codegen::detail::NodeAccess::blocks (g)[g.edges[e].destination] == blocks[to]
          && g.edges[e].kind == kind)
        return e;
    FAIL ("missing fixture edge");
    return 0;
  }
  MachineEdgeSplit split (const MachineControlFlow &g, std::size_t e) {
    auto r = split_machine_edge (*f, g, e);
    INFO (((r).has_value () ? "" : r.error ().message));
    REQUIRE ((r).has_value ());
    REQUIRE ((f->verify ()).has_value ());
    REQUIRE ((r.value ().cfg.verify (*f)).has_value ());
    return std::move ((r).value ());
  }
};
MachineRewriteResult allocated_gap (Fixture &t, unsigned level) {
  auto a = mirnext::codegen::detail::access (*(t.f)).create_register (Type::i64 ()),
       c = mirnext::codegen::detail::access (*(t.f)).create_register (Type::i64 ());
  REQUIRE ((a).has_value ());
  REQUIRE ((c).has_value ());
  std::vector<target::PhysicalRegister> globals;
  for (auto &p : t.f->target ().registers ())
    if (!p.reserved && p.reg.encoding ()) globals.push_back (p.reg);
  REQUIRE ((t.rw.set_global_registers (globals)).has_value ());
  t.emit (0, Opcode::Mov,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((a).value ()),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
  t.emit (0, MachinePseudoOpcode::Use,
          std::vector<MachineOperand> (20,
                                       mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                         (a).value ())));
  t.emit (1, Opcode::Mov,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((c).value ()),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
  for (unsigned n = 0; n < 3; ++n)
    t.emit (1, MachinePseudoOpcode::Use,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((c).value ())});
  t.emit (2, MachinePseudoOpcode::Use,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((a).value ())});
  t.emit (2, Opcode::Ret);
  auto state = t.f->state ();
  state.machinized = state.operands_legalized = true;
  REQUIRE ((t.rw.set_state (state)).has_value ());
  auto g = MachineGraphAnalysis::build (*t.f);
  REQUIRE ((g).has_value ());
  auto l = MachineLiveness::analyze (*t.f, (g).value (), {.optimization_level = level});
  REQUIRE ((l).has_value ());
  auto ranges = MachineLiveRanges::build (*t.f, (g).value (), (l).value ());
  REQUIRE ((ranges).has_value ());
  auto allocation = MachineAllocation::assign (*t.f, (g).value (), (l).value (), (ranges).value ());
  REQUIRE ((allocation).has_value ());
  auto rewritten = rewrite_allocated (*t.f, (g).value (), (l).value (), (ranges).value (),
                                      std::move ((allocation).value ()));
  INFO (((rewritten).has_value () ? "" : rewritten.error ().message));
  REQUIRE ((rewritten).has_value ());
  return std::move ((rewritten).value ());
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
  CHECK (mirnext::codegen::detail::NodeAccess::block (r) == t.blocks[1]);
  CHECK (t.f->dump () == before);
  CHECK (&jump->block () == t.blocks[1]);
}
TEST_CASE ("owning edge edits move jumps and label anchors without replacing their objects") {
  SUBCASE ("moving a jump from a multiple-incoming source") {
    Fixture t;
    t.emit (0, Opcode::Beq,
            {t.label (1), t.phys (),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    auto jump = t.emit (1, Opcode::Jmp, {t.label (2)});
    auto id = jump->creation_order ();
    t.emit (2, MachinePseudoOpcode::Use);
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 1, 2));
    CHECK (mirnext::codegen::detail::NodeAccess::block (r) != t.blocks[1]);
    CHECK (&jump->block () == mirnext::codegen::detail::NodeAccess::block (r));
    CHECK (jump->creation_order () == id);
    CHECK (t.blocks[1]->instructions ().begin () == t.blocks[1]->instructions ().end ());
  }
  SUBCASE ("moving a destination anchor with aliases") {
    Fixture t (5);
    t.emit (0, MachinePseudoOpcode::Use);
    auto extra
      = mirnext::codegen::detail::access (*(t.f)).create_label (*t.blocks[3], "alias", true);
    REQUIRE ((extra).has_value ());
    auto branch
      = t.emit (1, Opcode::Beq,
                {t.label (3), t.phys (),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    t.emit (2, Opcode::Jmp, {t.label (4)});
    t.emit (3, MachinePseudoOpcode::Use);
    t.emit (4, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 1, 3));
    CHECK (&t.labels[3]->block () == mirnext::codegen::detail::NodeAccess::block (r));
    CHECK (&extra.value ().block () == t.blocks[3]);
    CHECK (*mirnext::codegen::detail::operand_access (branch->operands ()[0])
              .get<const MachineLabel *> ()
           == t.labels[3]);
    CHECK (extra.value ().address_taken ());
  }
}
TEST_CASE ("owning edge edits reuse a label-only destination and split only fallthrough") {
  SUBCASE ("label-only destination") {
    Fixture t;
    t.emit (0, Opcode::Beq,
            {t.label (2), t.phys (),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    t.emit (1, Opcode::Jmp, {t.label (3)});
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 0, 2));
    CHECK (mirnext::codegen::detail::NodeAccess::block (r) == t.blocks[2]);
  }
  SUBCASE ("conditional fallthrough") {
    Fixture t;
    auto branch
      = t.emit (0, Opcode::Beq,
                {t.label (2), t.phys (),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    t.emit (1, MachinePseudoOpcode::Use);
    t.emit (2, MachinePseudoOpcode::Use);
    t.emit (3, Opcode::Ret);
    auto g = t.cfg ();
    auto r = t.split (g, t.edge (g, 0, 1, MachineControlFlow::EdgeKind::Fallthrough));
    CHECK (mirnext::codegen::detail::NodeAccess::block (r) != t.blocks[1]);
    CHECK (mirnext::codegen::detail::NodeAccess::block (r)->instructions ().begin ()->opcode ()
           == MachineOpcode (MachinePseudoOpcode::Use));
    CHECK (*mirnext::codegen::detail::operand_access (branch->operands ()[0])
              .get<const MachineLabel *> ()
           == t.labels[2]);
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
          = t.emit (0, Opcode::Beq,
                    {t.label (2), t.phys (),
                     mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
      t.emit (1,
              conditional_fallthrough ? MachineOpcode (Opcode::Beq)
                                      : MachineOpcode (MachinePseudoOpcode::Use),
              conditional_fallthrough
                ? std::vector{t.label (3), t.phys (),
                              mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                std::int64_t (1))}
                : std::vector<MachineOperand>{});
      t.emit (2, MachinePseudoOpcode::Use);
      t.emit (3, Opcode::Ret);
      auto original = t.labels[2];
      auto g = t.cfg ();
      auto r = t.split (g, t.edge (g, 0, 2,
                                   switch_case ? MachineControlFlow::EdgeKind::SwitchCase
                                               : MachineControlFlow::EdgeKind::Branch));
      CHECK (mirnext::codegen::detail::NodeAccess::block (r) != t.blocks[2]);
      CHECK (&original->block () == t.blocks[2]);
      CHECK (mirnext::codegen::detail::NodeAccess::block (r)->labels ().size () == 1);
      CHECK (&(*mirnext::codegen::detail::operand_access (branch->operands ()[switch_case ? 1 : 0])
                  .get<const MachineLabel *> ())
                ->block ()
             == mirnext::codegen::detail::NodeAccess::block (r));
      if (switch_case)
        CHECK (*mirnext::codegen::detail::operand_access (branch->operands ()[3])
                  .get<const MachineLabel *> ()
               == original);
      CHECK (mirnext::codegen::detail::NodeAccess::blocks (r.cfg).size ()
             == mirnext::codegen::detail::NodeAccess::blocks (g).size ()
                  + (conditional_fallthrough ? 2 : 1));
      auto preserved = MachineGraphAnalysis::build (*t.f, r.cfg);
      REQUIRE ((preserved).has_value ());
      CHECK (preserved.value ().current (*t.f));
    }
}
TEST_CASE (
  "owning edge edits reject forged foreign stale and unsupported indirect graphs before editing") {
  Fixture t;
  t.emit (0, Opcode::Beq,
          {t.label (2), t.phys (),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
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
    if (n == 4) mirnext::codegen::detail::NodeAccess::blocks (bad)[2] = nullptr;
    if (n == 5) bad.incoming[4].clear ();
    CHECK_FALSE ((split_machine_edge (*t.f, bad, e)).has_value ());
    CHECK (t.f->dump () == original);
    CHECK_FALSE ((MachineGraphAnalysis::build (*t.f, bad)).has_value ());
  }
  Fixture foreign;
  CHECK_FALSE ((split_machine_edge (*foreign.f, g, e)).has_value ());
  CHECK_FALSE ((split_machine_edge (*t.f, g, SIZE_MAX)).has_value ());
  CHECK_FALSE ((split_machine_edge (*t.f, g, g.outgoing[0][0])).has_value ());
  auto stale = g;
  REQUIRE ((mirnext::codegen::detail::access (*(t.f)).create_label (*t.blocks[2])).has_value ());
  CHECK_FALSE ((split_machine_edge (*t.f, stale, e)).has_value ());
  Fixture indirect;
  REQUIRE ((indirect.rw.move_label (*indirect.labels[2], *indirect.blocks[2])).has_value ());
  auto address = mirnext::codegen::detail::access (*(indirect.f))
                   .create_label (*indirect.blocks[2], "address", true);
  REQUIRE ((address).has_value ());
  auto other = mirnext::codegen::detail::access (*(indirect.f))
                 .create_label (*indirect.blocks[3], "other", true);
  REQUIRE ((other).has_value ());
  indirect.emit (0, Opcode::JmpIndirect, {indirect.phys ()});
  indirect.emit (1, MachinePseudoOpcode::Use);
  indirect.emit (2, MachinePseudoOpcode::Use);
  indirect.emit (3, Opcode::Ret);
  auto ig = indirect.cfg ();
  original = indirect.f->dump ();
  CHECK_FALSE (
    (split_machine_edge (*indirect.f, ig,
                         indirect.edge (ig, 0, 2, MachineControlFlow::EdgeKind::Indirect)))
      .has_value ());
  CHECK (indirect.f->dump () == original);
}
TEST_CASE ("owning edge spills retain range ownership and original O0 through O3 conditions") {
  if (target::TargetMachine::native ().value ().architecture () != target::Architecture::AArch64)
    return;
  for (unsigned level = 0; level < 4; ++level) {
    Fixture t (3);
    auto rewrite = allocated_gap (t, level);
    auto original = t.f->dump ();
    // Every analysis used to construct rewrite has already been destroyed.
    auto result = split_allocated_spills (*t.f, std::move (rewrite));
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    REQUIRE ((t.f->verify ()).has_value ());
    REQUIRE ((result.value ().cfg ().verify (*t.f)).has_value ());
    CHECK (result.value ().current (*t.f));
    CHECK (t.f->state ().edge_spills_inserted);
    CHECK (result.value ().placements ().size () == (level >= 2 ? 2 : 0));
    if (level < 2)
      CHECK (t.f->dump () == original);
    else {
      auto a = &*t.f->registers ().begin ();
      auto slot = result.value ().rewrite_result ().spill_slot (*a);
      REQUIRE ((slot).has_value ());
      REQUIRE ((slot).value ());
      CHECK (result.value ().allocation ().stack_bytes () == 8);
      CHECK (result.value ().placements ()[0]
             == mirnext::codegen::detail::NodeAccess::make_MachineSpillPlacement (a, t.blocks[1],
                                                                                  false, true));
      CHECK (result.value ().placements ()[1]
             == mirnext::codegen::detail::NodeAccess::make_MachineSpillPlacement (a, t.blocks[2],
                                                                                  false, false));
      CHECK (
        (t.blocks[1]->instructions ().begin ()->operands ()[0].get<MachineMemory> ()).has_value ());
      CHECK (
        (t.blocks[2]->instructions ().begin ()->operands ()[1].get<MachineMemory> ()).has_value ());
    }
    CHECK_FALSE ((split_allocated_spills (*t.f, result.value ().rewrite_result ())).has_value ());
    REQUIRE ((mirnext::codegen::detail::access (*(t.f)).create_label (*t.blocks[0])).has_value ());
    CHECK_FALSE (result.value ().current (*t.f));
  }
}
TEST_CASE ("owning edge spill preflight rejects stale and foreign rewrite ownership") {
  if (target::TargetMachine::native ().value ().architecture () != target::Architecture::AArch64)
    return;
  Fixture t (3);
  auto rewrite = allocated_gap (t, 2);
  Fixture foreign;
  auto before = t.f->dump ();
  CHECK_FALSE ((split_allocated_spills (*foreign.f, rewrite)).has_value ());
  CHECK (t.f->dump () == before);
  REQUIRE ((mirnext::codegen::detail::access (*(t.f)).create_label (*t.blocks[0])).has_value ());
  before = t.f->dump ();
  CHECK_FALSE ((split_allocated_spills (*t.f, std::move (rewrite))).has_value ());
  CHECK (t.f->dump () == before);
}
TEST_CASE ("owning allocated critical edge spills run with no reference runtime") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  if (target.value ().architecture () != target::Architecture::AArch64) return;
  for (bool critical : {false, true})
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (critical);
      CAPTURE (level);
      auto source
        = serialization::parse_text (test::edge_spill_fixture ((target).value (), critical));
      REQUIRE ((source).has_value ());
      auto &function = *source.value ().modules ().begin ()->functions ().begin ();
      auto machine = prepare_machine_ir (function, (target).value ());
      REQUIRE ((machine).has_value ());
      REQUIRE ((machinize ((machine).value ())).has_value ());
      REQUIRE ((legalize_operands ((machine).value ())).has_value ());
      auto graph = MachineGraphAnalysis::build ((machine).value ());
      REQUIRE ((graph).has_value ());
      REQUIRE ((coalesce_registers ((machine).value (), (graph).value (), level)).has_value ());
      graph = MachineGraphAnalysis::build ((machine).value ());
      REQUIRE ((graph).has_value ());
      auto live = MachineLiveness::analyze ((machine).value (), (graph).value (),
                                            {.optimization_level = level});
      REQUIRE ((live).has_value ());
      auto ranges
        = MachineLiveRanges::build ((machine).value (), (graph).value (), (live).value ());
      REQUIRE ((ranges).has_value ());
      auto allocation = MachineAllocation::assign ((machine).value (), (graph).value (),
                                                   (live).value (), (ranges).value ());
      REQUIRE ((allocation).has_value ());
      auto rewritten = rewrite_allocated ((machine).value (), (graph).value (), (live).value (),
                                          (ranges).value (), std::move ((allocation).value ()));
      REQUIRE ((rewritten).has_value ());
      auto blocks = rewritten.value ().blocks ().size ();
      auto spills = split_allocated_spills ((machine).value (), std::move ((rewritten).value ()));
      INFO (((spills).has_value () ? "" : spills.error ().message));
      REQUIRE ((spills).has_value ());
      CHECK (spills.value ().placements ().size () == (level < 2 ? 0 : critical ? 3 : 2));
      CHECK (mirnext::codegen::detail::NodeAccess::blocks (spills.value ().cfg ()).size ()
             == blocks + (critical && level >= 2 ? 1 : 0));
      REQUIRE ((spills.value ().cfg ().verify ((machine).value ())).has_value ());
      REQUIRE ((machine.value ().verify ()).has_value ());
    }
}
TEST_CASE ("owning block edits reject foreign nodes without changing either function") {
  Fixture a, b;
  auto i = a.emit (0, MachinePseudoOpcode::Use);
  auto before = a.f->dump (), other = b.f->dump ();
  CHECK_FALSE ((a.rw.move_to (*i, *b.blocks[0])).has_value ());
  CHECK_FALSE ((b.rw.move_label (*a.labels[0], *b.blocks[0])).has_value ());
  CHECK_FALSE ((a.rw.create_block_before (*b.blocks[0])).has_value ());
  CHECK_FALSE ((a.rw.create_block_after (*b.blocks[0])).has_value ());
  CHECK (a.f->dump () == before);
  CHECK (b.f->dump () == other);
}
