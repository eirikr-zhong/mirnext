/* Licensed under the MIT License. Allocation rewrite on owning machine nodes. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "Navigation.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool arm () {
  return target::TargetMachine::native ().value ().architecture () == target::Architecture::AArch64;
}
std::unique_ptr<MachineFunction> function (bool variadic = false) {
  auto t = target::TargetMachine::native ();
  REQUIRE ((t).has_value ());
  auto f = std::make_unique<MachineFunction> ("native", "rewrite", Signature{{}, {}, variadic},
                                              (t).value ());
  MachineRewriter rw (*f);
  auto state = f->state ();
  state.machinized = state.operands_legalized = true;
  REQUIRE ((rw.set_state (state)).has_value ());
  return f;
}
MachineVirtualRegister *reg (MachineFunction &f, Type type = Type::i64 (),
                             std::optional<target::PhysicalRegister> binding = {}) {
  auto r = mirnext::codegen::detail::access (f).create_register (type, {}, false, false, binding);
  REQUIRE ((r).has_value ());
  return &(r).value ();
}
MachineOperand op (const MachineVirtualRegister *r) {
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (*r);
}
MachineOperand integer (std::int64_t n) {
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (n);
}
MachineInstruction *emit (MachineRewriter &rw, MachineBasicBlock &b, MachineOpcode code,
                          std::vector<MachineOperand> operands = {}) {
  auto r = rw.append (b, code, std::move (operands));
  INFO (((r).has_value () ? "" : r.error ().message));
  REQUIRE ((r).has_value ());
  return &(r).value ();
}
void force_stack (MachineFunction &f) {
  std::vector<target::PhysicalRegister> all;
  for (auto &p : f.target ().registers ())
    if (!p.reserved) all.push_back (p.reg);
  MachineRewriter rw (f);
  REQUIRE ((rw.set_global_registers (all)).has_value ());
}
MachineRewriteResult rewrite (MachineFunction &f, unsigned level = 2) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
  REQUIRE ((live).has_value ());
  auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
  INFO (((ranges).has_value () ? "" : ranges.error ().message));
  REQUIRE ((ranges).has_value ());
  auto a = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
  REQUIRE ((a).has_value ());
  auto result = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                   std::move ((a).value ()));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  CHECK (result.value ().current (f));
  CHECK (f.state ().operands_rewritten);
  CHECK_FALSE (graph.value ().current (f));
  CHECK_FALSE (live.value ().current (f));
  return std::move ((result).value ());
}
}  // namespace
TEST_CASE ("owning allocation rewrite preserves node identities and replaces virtual operands") {
  if (!arm ()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f);
    auto first = emit (rw, b, Opcode::Mov, {op (a), integer (42)});
    auto serial = first->creation_order ();
    auto add = emit (rw, b, Opcode::Add, {op (c), op (a), integer (1)});
    emit (rw, b, MachinePseudoOpcode::Use, {op (c)});
    emit (rw, b, Opcode::Ret);
    auto result = rewrite (*f, level);
    CHECK (&first->block () == &b);
    CHECK (first->creation_order () == serial);
    CHECK ((add->operands ()[0].get<target::PhysicalRegister> ()).has_value ());
    CHECK (result.allocation ().stack_bytes () == 0);
    CHECK_FALSE ((result.spill_slot (*a)).value ());
    auto other = function ();
    auto foreign = reg (*other);
    CHECK_FALSE ((result.spill_slot (*foreign)).has_value ());
    auto before = f->dump ();
    CHECK_FALSE ((rw.insert_before (*first, Opcode::Mov, {op (a), integer (0)})).has_value ());
    CHECK (f->dump () == before);
    reg (*f);
    CHECK_FALSE ((result.spill_slot (*a)).has_value ());
  }
}
TEST_CASE ("owning allocation rewrite uses direct stack moves and ordered reloads") {
  if (!arm ()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function ();
    force_stack (*f);
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f);
    auto immediate = emit (rw, b, Opcode::Mov, {op (a), integer (42)});
    auto copied = emit (rw, b, Opcode::Mov, {op (c), op (a)});
    auto add = emit (rw, b, Opcode::Add, {op (c), op (c), op (a)});
    emit (rw, b, MachinePseudoOpcode::Use, {op (c)});
    emit (rw, b, Opcode::Ret);
    auto result = rewrite (*f, level);
    CHECK (result.allocation ().stack_bytes () == 16);
    REQUIRE ((immediate->operands ()[0].get<target::PhysicalRegister> ()).has_value ());
    REQUIRE ((copied->operands ()[0].get<MachineMemory> ()).has_value ());
    CHECK (copied->operands ()[0].get<MachineMemory> ().value ().base
           == mirnext::codegen::detail::NodeAccess::make_MachineRegister (
             f->target ().registers ()[29].reg));
    auto before = mirnext::test::neighbor (rw.previous (*add)),
         after = mirnext::test::neighbor (rw.next (*add));
    REQUIRE ((before).has_value ());
    REQUIRE ((after).has_value ());
    REQUIRE (&(before).value ());
    REQUIRE (&(after).value ());
    CHECK ((before.value ().operands ()[1].get<MachineMemory> ()).has_value ());
    CHECK ((after.value ().operands ()[0].get<MachineMemory> ()).has_value ());
    CHECK (add->operands ()[1].get<target::PhysicalRegister> ().value ().encoding () == 9);
    CHECK (add->operands ()[2].get<target::PhysicalRegister> ().value ().encoding () == 10);
  }
}
TEST_CASE (
  "owning allocation rewrite materializes all local address widths and frame header offsets") {
  if (!arm ()) return;
  for (bool extended : {false, true})
    for (auto code : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
      auto f = function (extended);
      auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
      MachineRewriter rw (*f);
      auto a = reg (*f), pointer = reg (*f);
      auto address = emit (rw, b, code, {op (pointer), op (a)});
      emit (rw, b, MachinePseudoOpcode::Use, {op (pointer)});
      emit (rw, b, Opcode::Ret);
      auto result = rewrite (*f);
      CHECK (address->opcode () == MachineOpcode (Opcode::Mov));
      CHECK (std::holds_alternative<StackLocation> ((result.allocation ().location (*a)).value ()));
      auto add = mirnext::test::neighbor (rw.previous (*address));
      REQUIRE ((add).has_value ());
      REQUIRE (&(add).value ());
      CHECK (add.value ().opcode () == MachineOpcode (Opcode::Add));
      auto offset = mirnext::test::neighbor (rw.previous ((add).value ()));
      REQUIRE ((offset).has_value ());
      REQUIRE (&(offset).value ());
      CHECK ((offset.value ().operands ()[1].get<std::int64_t> ()).value ()
             == (extended ? 32 : 16));
    }
}
TEST_CASE ("owning allocation rewrite preloads a fully spilled indexed store before its value") {
  if (!arm ()) return;
  for (auto scale : {1u, 2u, 4u, 8u}) {
    auto f = function ();
    force_stack (*f);
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), index = reg (*f), value = reg (*f);
    emit (rw, b, Opcode::Mov, {op (a), integer (4096)});
    emit (rw, b, Opcode::Mov, {op (index), integer (2)});
    emit (rw, b, Opcode::Mov, {op (value), integer (42)});
    auto store
      = emit (rw, b, Opcode::Mov,
              {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                 mirnext::codegen::detail::NodeAccess::make_MachineMemory (Type::i64 (), a, index,
                                                                           0, scale, "object",
                                                                           "other")),
               op (value)});
    emit (rw, b, Opcode::Ret);
    rewrite (*f);
    auto m = mirnext::codegen::detail::operand_access (store->operands ()[0]).get<MachineMemory> ();
    REQUIRE (m);
    CHECK (m->base
           == mirnext::codegen::detail::NodeAccess::make_MachineRegister (
             f->target ().registers ()[9].reg));
    CHECK_FALSE (m->index);
    CHECK (m->alias == "object");
    CHECK (m->nonalias == "other");
    CHECK (store->operands ()[1].get<target::PhysicalRegister> ().value ().encoding () == 10);
    unsigned shifts = 0;
    for (auto &i : b.instructions ()) shifts += i.opcode () == MachineOpcode (Opcode::Lsh);
    CHECK (shifts == (scale != 1));
  }
}
TEST_CASE (
  "owning allocation rewrite keeps floating reload registers distinct from address temporaries") {
  if (!arm ()) return;
  for (bool single : {false, true}) {
    auto f = function ();
    force_stack (*f);
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto t = single ? Type (Type::Kind::F32) : Type (Type::Kind::F64);
    auto a = reg (*f, t), c = reg (*f, t);
    emit (rw, b, single ? Opcode::FMov : Opcode::DMov,
          {op (a), single ? mirnext::codegen::detail::NodeAccess::make_MachineOperand (1.0f)
                          : mirnext::codegen::detail::NodeAccess::make_MachineOperand (1.0)});
    auto add = emit (rw, b, single ? Opcode::FAdd : Opcode::DAdd, {op (c), op (a), op (a)});
    emit (rw, b, MachinePseudoOpcode::Use, {op (c)});
    emit (rw, b, Opcode::Ret);
    rewrite (*f);
    CHECK (add->operands ()[0].get<target::PhysicalRegister> ().value ().encoding () == 48);
    CHECK (add->operands ()[1] == add->operands ()[2]);
    CHECK (add->operands ()[1].get<target::PhysicalRegister> ().value ().encoding () == 48);
  }
}
TEST_CASE (
  "owning allocation rewrite expands out-of-range stack offsets in exact instruction order") {
  if (!arm ()) return;
  auto f = function ();
  force_stack (*f);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  // Addressable locals reserve nonreusable slots even without a live range.
  for (unsigned n = 0; n < 4096; ++n) {
    auto r
      = mirnext::codegen::detail::access (*(f)).create_register (Type::i64 (), {}, false, true);
    REQUIRE ((r).has_value ());
  }
  auto value = reg (*f), pointer = reg (*f);
  auto mov = emit (rw, b, Opcode::Mov, {op (value), integer (42)});
  emit (rw, b, Opcode::Addr, {op (pointer), op (value)});
  emit (rw, b, MachinePseudoOpcode::Use, {op (pointer)});
  emit (rw, b, Opcode::Ret);
  auto result = rewrite (*f);
  auto store = mirnext::test::neighbor (rw.next (*mov));
  REQUIRE ((store).has_value ());
  REQUIRE (&(store).value ());
  CHECK (store.value ().opcode () == MachineOpcode (Opcode::Mov));
  REQUIRE ((store.value ().operands ()[1].get<std::int64_t> ()).has_value ());
  CHECK ((store.value ().operands ()[1].get<std::int64_t> ()).value () >= 32768);
  auto add = mirnext::test::neighbor (rw.next ((store).value ()));
  REQUIRE ((add).has_value ());
  REQUIRE (&(add).value ());
  CHECK (add.value ().opcode () == MachineOpcode (Opcode::Add));
  auto written = mirnext::test::neighbor (rw.next ((add).value ()));
  REQUIRE ((written).has_value ());
  REQUIRE (&(written).value ());
  REQUIRE ((written.value ().operands ()[0].get<MachineMemory> ()).has_value ());
  CHECK (written.value ().operands ()[0].get<MachineMemory> ().value ().displacement == 0);
  CHECK (result.allocation ().reusable_stack_offset () > 32768);
}
TEST_CASE (
  "owning allocation rewrite deletes redundant moves while retaining empty block anchors") {
  if (!arm ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (bool labelled : {false, true}) {
      auto f = function ();
      auto &b = mirnext::codegen::detail::access (*(f)).create_block (),
           &exit = mirnext::codegen::detail::access (*(f)).create_block ();
      if (labelled)
        REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (b)).has_value ());
      MachineRewriter rw (*f);
      auto a = reg (*f, Type::i64 (), f->target ().registers ()[0].reg);
      emit (rw, b, Opcode::Mov, {op (a), op (a)});
      emit (rw, exit, Opcode::Ret);
      auto result = rewrite (*f, level);
      CHECK (result.deleted_moves () == 1);
      CHECK (f->instruction_count () == (level && !labelled ? 2 : 1));
      if (level && !labelled)
        CHECK (b.instructions ().begin ()->opcode () == MachineOpcode (MachinePseudoOpcode::Use));
    }
}
TEST_CASE ("owning allocation rewrite preserves live hardware values across repeated calls") {
  if (!arm ()) return;
  for (unsigned level : {2, 3}) {
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto p = f->target ().registers ()[2].reg;
    auto a = reg (*f, Type::i64 (), p);
    auto proto = mirnext::codegen::detail::access (*(f)).intern_symbol (
      {"native", "p", MachineSymbol::Kind::Prototype, Signature{}, false, {}});
    REQUIRE ((proto).has_value ());
    auto ext = mirnext::codegen::detail::access (*(f)).intern_symbol (
      {"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
    REQUIRE ((ext).has_value ());
    emit (rw, b, Opcode::Mov, {op (a), integer (42)});
    std::vector<MachineInstruction *> calls;
    for (unsigned n = 0; n < 2; ++n) {
      auto call = emit (rw, b, Opcode::Call,
                        {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                           mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                             &(proto).value ())),
                         mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                           mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                             &(ext).value ()))});
      REQUIRE ((rw.lower_call (*call, call->opcode (),
                               std::vector<MachineOperand> (call->operands ().begin (),
                                                            call->operands ().end ())))
                 .has_value ());
      calls.push_back (call);
      REQUIRE ((rw.set_call_arguments (*call, std::span (&p, 1))).has_value ());
      emit (rw, b, MachinePseudoOpcode::Use, {op (a)});
    }
    emit (rw, b, Opcode::Ret);
    auto result = rewrite (*f, level);
    auto slot = result.spill_slot (*a);
    REQUIRE ((slot).has_value ());
    REQUIRE ((slot).value ());
    CHECK (result.allocation ().stack_bytes () == 8);
    for (auto call : calls) {
      auto next = mirnext::test::neighbor (rw.next (*call));
      REQUIRE ((next).has_value ());
      REQUIRE (&(next).value ());
      REQUIRE ((next.value ().operands ()[1].get<MachineMemory> ()).has_value ());
      CHECK (next.value ().operands ()[1].get<MachineMemory> ().value ().displacement
             == std::int64_t ((*(slot).value ()).offset + 16));
    }
  }
}
TEST_CASE (
  "owning allocation rewrite rejects invalid phases stale inputs and foreign allocations") {
  if (!arm ()) return;
  auto f = function ();
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, Opcode::Mov, {op (a), integer (42)});
  emit (rw, b, MachinePseudoOpcode::Use, {op (a)});
  emit (rw, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (*f, (graph).value ());
  REQUIRE ((live).has_value ());
  auto ranges = MachineLiveRanges::build (*f, (graph).value (), (live).value ());
  REQUIRE ((ranges).has_value ());
  auto allocation
    = MachineAllocation::assign (*f, (graph).value (), (live).value (), (ranges).value ());
  REQUIRE ((allocation).has_value ());
  auto simple_live = MachineLiveness::analyze (*f, (graph).value (), {.optimization_level = 0});
  REQUIRE ((simple_live).has_value ());
  auto simple_ranges = MachineLiveRanges::build (*f, (graph).value (), (simple_live).value ());
  REQUIRE ((simple_ranges).has_value ());
  auto simple_allocation = MachineAllocation::assign (*f, (graph).value (), (simple_live).value (),
                                                      (simple_ranges).value ());
  REQUIRE ((simple_allocation).has_value ());
  auto original = f->dump ();
  CHECK_FALSE ((rewrite_allocated (*f, (graph).value (), (live).value (), (ranges).value (),
                                   (simple_allocation).value ()))
                 .has_value ());
  CHECK (f->dump () == original);
  auto other = function ();
  auto before = f->dump ();
  CHECK_FALSE ((rewrite_allocated (*other, (graph).value (), (live).value (), (ranges).value (),
                                   (allocation).value ()))
                 .has_value ());
  CHECK (f->dump () == before);
  reg (*f);
  before = f->dump ();
  CHECK_FALSE ((rewrite_allocated (*f, (graph).value (), (live).value (), (ranges).value (),
                                   (allocation).value ()))
                 .has_value ());
  CHECK (f->dump () == before);
  auto result = rewrite (*f);
  before = f->dump ();
  CHECK_FALSE ((rewrite_allocated (*f, (graph).value (), (live).value (), (ranges).value (),
                                   result.allocation ()))
                 .has_value ());
  CHECK (f->dump () == before);
}
