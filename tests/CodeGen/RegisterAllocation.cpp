/* Licensed under the MIT License. Owning allocation preparation regressions. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/RegisterAllocation.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
std::unique_ptr<MachineFunction> function (bool result = true) {
  auto t = target::TargetMachine::native ();
  REQUIRE (t);
  return std::make_unique<MachineFunction> ("native", "allocation",
                                            Signature{result ? std::vector{Type::i64 ()}
                                                             : std::vector<Type>{},
                                                      {}},
                                            *t);
}
MachineVirtualRegister *reg (MachineFunction &f,
                             std::optional<target::PhysicalRegister> binding = {}) {
  auto r = f.create_register (Type::i64 (), {}, false, false, binding);
  REQUIRE (r);
  return *r;
}
MachineOperand operand (MachineVirtualRegister *r) { return MachineOperand (*r); }
MachineInstruction *emit (MachineRewriter &rw, MachineBasicBlock &b, MachineOpcode op,
                          std::vector<MachineOperand> operands) {
  auto i = rw.append (b, op, std::move (operands));
  REQUIRE (i);
  return *i;
}
MachineLiveRanges ranges (const MachineFunction &f, unsigned level = 2,
                          std::span<const MachineRegister> scan = {}) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE (graph);
  auto live
    = MachineLiveness::analyze (f, *graph, {.optimization_level = level, .scan_registers = scan});
  REQUIRE (live);
  auto result = MachineLiveRanges::build (f, *graph, *live);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
MachineAllocation allocated (const MachineFunction &f, unsigned level = 2) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE (graph);
  auto live = MachineLiveness::analyze (f, *graph, {.optimization_level = level});
  REQUIRE (live);
  auto lr = MachineLiveRanges::build (f, *graph, *live);
  REQUIRE (lr);
  auto result = MachineAllocation::assign (f, *graph, *live, *lr);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
void force_stack (MachineFunction &f) {
  std::vector<target::PhysicalRegister> all;
  for (auto &p : f.target ().registers ())
    if (!p.reserved) all.push_back (p.reg);
  MachineRewriter rw (f);
  REQUIRE (rw.set_global_registers (all));
}
void only_first_register (MachineFunction &f) {
  std::vector<target::PhysicalRegister> all;
  for (auto &p : f.target ().registers ())
    if (!p.reserved && p.reg.encoding ()) all.push_back (p.reg);
  MachineRewriter rw (f);
  REQUIRE (rw.set_global_registers (all));
}
}  // namespace
TEST_CASE ("owning coalescing retains original O0 O1 conditions and merges O2 O3 copy chains") {
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function ();
    auto &b = f->create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), d = reg (*f);
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (42))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    emit (rw, b, Opcode::Mov, {operand (d), operand (c)});
    auto ret = emit (rw, b, Opcode::Ret, {operand (d)});
    auto serial = ret->creation_order ();
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE (graph);
    auto merged = coalesce_registers (*f, *graph, level);
    REQUIRE (merged);
    CHECK (merged->enabled () == (level >= 2));
    CHECK (merged->coalesced_moves () == (level >= 2 ? 2 : 0));
    CHECK (*merged->representative (d) == MachineRegister (level >= 2 ? a : d));
    CHECK (f->instruction_count () == (level >= 2 ? 2 : 4));
    CHECK (ret->creation_order () == serial);
    REQUIRE (f->verify ());
    auto lr = ranges (*f, level);
    REQUIRE (lr.ranges (level >= 2 ? a : d));
  }
}
TEST_CASE ("owning coalescing retains conflicts and tests all previously merged group members") {
  for (bool chain : {false, true}) {
    auto f = function (false);
    auto &b = f->create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), d = reg (*f);
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    if (chain) emit (rw, b, Opcode::Mov, {operand (d), operand (c)});
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (2))});
    emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (chain ? d : c)});
    emit (rw, b, Opcode::Ret, {});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE (graph);
    auto merged = coalesce_registers (*f, *graph);
    REQUIRE (merged);
    CHECK (merged->coalesced_moves () == (chain ? 1 : 0));
    CHECK (*merged->conflicts (a, chain ? d : c));
    CHECK (*merged->representative (chain ? d : c) == MachineRegister (chain ? d : c));
  }
}
TEST_CASE ("owning coalescing preserves hardware identity and rewrites memory address uses") {
  for (unsigned bound = 0; bound < 3; ++bound) {
    auto f = function ();
    std::vector<target::PhysicalRegister> bindings;
    for (auto &p : f->target ().registers ())
      if (!p.reserved && p.bank == target::RegisterBank::Integer) bindings.push_back (p.reg);
    REQUIRE (bindings.size () >= 2);
    auto a = reg (*f, bound == 0 || bound == 2 ? std::optional (bindings[0]) : std::nullopt);
    auto c = reg (*f, bound >= 1 ? std::optional (bindings[1]) : std::nullopt);
    auto value = reg (*f);
    auto &b = f->create_block ();
    MachineRewriter rw (*f);
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (64))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    auto load
      = emit (rw, b, Opcode::Mov,
              {operand (value),
               MachineOperand (MachineMemory{Type::i64 (), c, c, 8, 1, "alias", "nonalias"})});
    emit (rw, b, Opcode::Ret, {operand (value)});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE (graph);
    auto merged = coalesce_registers (*f, *graph);
    REQUIRE (merged);
    auto representative = bound == 0 ? a : c;
    CHECK (merged->coalesced_moves () == (bound == 2 ? 0 : 1));
    CHECK (*merged->representative (c) == MachineRegister (representative));
    CHECK (load->operands ()[1].get<MachineMemory> ()->base
           == std::optional<MachineRegister> (representative));
    CHECK (load->operands ()[1].get<MachineMemory> ()->alias == "alias");
    REQUIRE (f->verify ());
  }
}
TEST_CASE (
  "owning coalescing preserves empty block anchors and checks thresholds and stale views") {
  for (unsigned limit : {2, 3}) {
    auto f = function (false);
    auto &entry = f->create_block (), &exit = f->create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f);
    emit (rw, entry, Opcode::Mov, {operand (c), operand (a)});
    emit (rw, exit, Opcode::Ret, {});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE (graph);
    auto result = coalesce_registers (*f, *graph, 2, limit);
    REQUIRE (result);
    CHECK (result->enabled () == (limit == 3));
    if (limit == 3) {
      CHECK_FALSE (entry.labels ().empty ());
      CHECK_FALSE (graph->current (*f));
    }
    auto lr = ranges (*f);
    CHECK (lr.current (*f));
    reg (*f);
    CHECK_FALSE (result->current (*f));
    CHECK_FALSE (result->representative (c));
    CHECK_FALSE (lr.ranges (a));
    CHECK_FALSE (coalesce_registers (*f, *graph));
  }
}
TEST_CASE ("owning live ranges retain unused definitions and compressed neighboring lifetimes") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
  emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (2))});
  emit (rw, b, Opcode::Ret, {});
  auto lr = ranges (*f);
  auto p = f->target ().registers ().size ();
  CHECK (lr.uncompressed_ranges ()[p] == std::vector<MachineLiveRange>{{1, 1}, {0, 0}});
  CHECK (lr.ranges (a)->data () == lr.all_ranges ()[p].data ());
  CHECK (lr.all_ranges ()[p] == std::vector<MachineLiveRange>{{0, 1}});
  CHECK (lr.point_count () == 2);
  CHECK (lr.uncompressed_point_count () == 2);
}
TEST_CASE ("owning whole-block ranges preserve label anchors and ordered compression") {
  auto f = function ();
  auto &entry = f->create_block (), &first = f->create_block (), &second = f->create_block (),
       &exit = f->create_block ();
  REQUIRE (f->create_label (first));
  REQUIRE (f->create_label (second));
  auto a = reg (*f);
  MachineRewriter rw (*f);
  emit (rw, entry, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (42))});
  emit (rw, exit, Opcode::Ret, {operand (a)});
  auto lr = ranges (*f);
  auto p = f->target ().registers ().size ();
  REQUIRE (lr.all_ranges ()[p].size () == 3);
  CHECK (lr.all_ranges ()[p][1].blocks == std::vector<const MachineBasicBlock *>{&first, &second});
  CHECK (lr.spill_blocks ()[3].kill.empty ());
  std::array<MachineRegister, 1> scan{a};
  auto compact = ranges (*f, 2, scan);
  CHECK (compact.is_compact ());
  CHECK (compact.scan_registers ()[0] == MachineRegister (a));
}
TEST_CASE ("owning instruction creation order survives layout movement and replacement") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f);
  auto first = emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
  auto second = emit (rw, b, Opcode::Mov, {operand (a), operand (c)});
  auto one = first->creation_order (), two = second->creation_order ();
  CHECK (one < two);
  REQUIRE (rw.move_before (*second, *first));
  REQUIRE (rw.replace (*first, Opcode::Mov, {operand (c), operand (a)}));
  CHECK (first->creation_order () == one);
  CHECK (second->creation_order () == two);
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE (graph);
  auto result = coalesce_registers (*f, *graph);
  REQUIRE (result);
  CHECK (result->coalesced_moves () == 2);
}
TEST_CASE ("owning live ranges distinguish surviving call values and physical argument lifetimes") {
  for (bool survives : {false, true}) {
    auto f = function ();
    auto &b = f->create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f);
    auto proto = f->intern_symbol (
      {"native", "p", MachineSymbol::Kind::Prototype, Signature{{}, {Type::i64 ()}}, false, {}});
    REQUIRE (proto);
    auto callee = f->intern_symbol ({"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
    REQUIRE (callee);
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (42))});
    auto call = emit (rw, b, Opcode::Call,
                      {MachineOperand (MachineSymbolReference{*proto}),
                       MachineOperand (MachineSymbolReference{*callee}), operand (a)});
    auto argument = f->target ().registers ()[1].reg;
    REQUIRE (rw.set_call_arguments (*call, std::span (&argument, 1)));
    emit (rw, b, Opcode::Ret, {survives ? operand (a) : MachineOperand (std::int64_t (0))});
    auto lr = ranges (*f);
    CHECK (lr.call_crossed ().contains (f->target ().registers ().size ()) == survives);
    CHECK_FALSE (lr.ranges (argument)->empty ());
    std::array<MachineRegister, 1> scan{a};
    auto compact = ranges (*f, 2, scan);
    CHECK (compact.call_crossed ().empty ());
    CHECK (compact.ranges (argument)->empty ());
  }
}
TEST_CASE ("owning range construction accounts for early clobbers and rejects stale input") {
  auto f = function ();
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f);
  emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (42))});
  emit (rw, b, Opcode::Mov, {operand (c), MachineOperand (std::int64_t (6))});
  emit (rw, b, Opcode::UMulo, {operand (a), operand (a), operand (c)});
  emit (rw, b, Opcode::Ret, {operand (a)});
  auto lr = ranges (*f);
  for (auto r : early_clobbers (f->target (), Opcode::UMulo))
    if (r) CHECK_FALSE (lr.ranges (*r)->empty ());
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE (graph);
  auto live = MachineLiveness::analyze (*f, *graph);
  REQUIRE (live);
  reg (*f);
  CHECK_FALSE (MachineLiveRanges::build (*f, *graph, *live));
}
TEST_CASE (
  "owning packed conflicts preserve symmetry across word boundaries and reject foreign queries") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  std::vector<MachineVirtualRegister *> regs;
  for (unsigned n = 0; n < 70; ++n) regs.push_back (reg (*f));
  for (unsigned n = 1; n < regs.size (); ++n)
    emit (rw, b, Opcode::Mov, {operand (regs[n]), operand (regs[0])});
  for (unsigned n = 0; n < regs.size (); ++n)
    emit (rw, b, Opcode::Mov, {operand (regs[n]), MachineOperand (std::int64_t (n))});
  std::vector<MachineOperand> uses;
  for (auto r : regs) uses.push_back (operand (r));
  emit (rw, b, MachinePseudoOpcode::Use, std::move (uses));
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE (graph);
  CHECK_FALSE (coalesce_registers (*f, *graph, 4));
  CHECK_FALSE (coalesce_registers (*f, *graph, 2, 0));
  auto result = coalesce_registers (*f, *graph);
  REQUIRE (result);
  CHECK (result->coalesced_moves () == 0);
  for (auto a : regs)
    for (auto c : regs) CHECK (*result->conflicts (a, c) == (a != c));
  auto other = function (false);
  auto foreign = reg (*other);
  CHECK_FALSE (result->representative (foreign));
  CHECK_FALSE (result->conflicts (regs[0], foreign));
  auto lr = ranges (*f);
  CHECK_FALSE (lr.ranges (foreign));
}
TEST_CASE (
  "owning assignment preserves declaration identity frequency priority and unused values") {
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function (false);
    auto &b = f->create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), unused = reg (*f);
    emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
    emit (rw, b, Opcode::Mov, {operand (c), MachineOperand (std::int64_t (2))});
    emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (c), operand (c)});
    emit (rw, b, Opcode::Ret, {});
    auto revision = f->revision ();
    auto allocation = allocated (*f, level);
    CHECK (f->revision () == revision);
    REQUIRE (allocation.order ().size () == 3);
    CHECK (allocation.order ()[0] == c);
    CHECK (allocation.order ()[2] == unused);
    CHECK (std::holds_alternative<std::monostate> (*allocation.location (*unused)));
    auto x = allocation.location (*a), y = allocation.location (*c);
    REQUIRE (x);
    REQUIRE (y);
    CHECK (std::holds_alternative<target::PhysicalRegister> (*x));
    CHECK (*x != *y);
    CHECK (allocation.stack_bytes () == 0);
    CHECK (allocation.busy ().empty () == (level < 2));
    CHECK (allocation.current (*f));
  }
}
TEST_CASE ("owning assignment reuses byte-addressed spill locations for disjoint lifetimes") {
  auto f = function (false);
  force_stack (*f);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f), d = reg (*f);
  emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Mov, {operand (c), MachineOperand (std::int64_t (2))});
  emit (rw, b, Opcode::Mov, {operand (d), MachineOperand (std::int64_t (3))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (c), operand (d)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  auto x = std::get<StackLocation> (*result.location (*a)),
       y = std::get<StackLocation> (*result.location (*c)),
       z = std::get<StackLocation> (*result.location (*d));
  CHECK (x.offset == y.offset);
  CHECK (y.offset != z.offset);
  CHECK (x.size == 8);
  CHECK (x.alignment == 8);
  CHECK (result.stack_bytes () == 16);
  CHECK (result.reusable_stack_offset () == 0);
  CHECK (result.used_registers ().empty ());
}
TEST_CASE ("owning address-taken locals reserve unique nonreusable stack storage") {
  auto f = function (false);
  force_stack (*f);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f), pointer = reg (*f);
  emit (rw, b, Opcode::Addr, {operand (pointer), operand (a)});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (pointer)});
  emit (rw, b, Opcode::Addr, {operand (pointer), operand (c)});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (pointer)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  CHECK (std::get<StackLocation> (*result.location (*a)).offset == 0);
  CHECK (std::get<StackLocation> (*result.location (*c)).offset == 8);
  CHECK (std::get<StackLocation> (*result.location (*pointer)).offset == 16);
  CHECK (result.reusable_stack_offset () == 16);
  CHECK (result.stack_bytes () == 24);
  CHECK (std::find (result.order ().begin (), result.order ().end (), a) == result.order ().end ());
}
TEST_CASE (
  "owning assignment aligns long double spill locations and keeps physical and stack domains "
  "separate") {
  auto f = function (false);
  force_stack (*f);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  auto wide = f->create_register (Type (Type::Kind::LongDouble));
  REQUIRE (wide);
  emit (rw, b, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (*wide)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  auto s = std::get<StackLocation> (*result.location (**wide));
  auto units = (*wide)->type ().kind () == Type::Kind::LongDouble ? 2u : 1u;
  CHECK (s.size == units * 8);
  CHECK (s.offset % s.alignment == 0);
  bool found = false;
  for (auto &p : result.used ())
    if (p.stack.contains (s.offset / 8)) {
      CHECK_FALSE (p.physical.empty ());
      found = true;
    }
  CHECK (found);
}
TEST_CASE (
  "owning allocation hardware bindings retain side effects without counting them as selected "
  "registers") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto first = f->target ().registers ()[0].reg, second = f->target ().registers ()[1].reg;
  auto a = reg (*f, first), c = reg (*f, second), d = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (c), operand (d), operand (d)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  CHECK (result.order ()[0] == a);
  CHECK (result.order ()[1] == c);
  CHECK (std::get<target::PhysicalRegister> (*result.location (*a)) == first);
  CHECK (std::get<target::PhysicalRegister> (*result.location (*c)) == second);
  auto selected = std::get<target::PhysicalRegister> (*result.location (*d));
  CHECK (selected != first);
  CHECK (selected != second);
  CHECK (std::find (result.used_registers ().begin (), result.used_registers ().end (), first)
         == result.used_registers ().end ());
}
TEST_CASE ("owning assignment rejects compact mixed foreign and stale analyses") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE (graph);
  auto live = MachineLiveness::analyze (*f, *graph);
  REQUIRE (live);
  auto lr = MachineLiveRanges::build (*f, *graph, *live);
  REQUIRE (lr);
  std::array<MachineRegister, 1> scan{a};
  auto compact = MachineLiveness::analyze (*f, *graph, {.scan_registers = scan});
  REQUIRE (compact);
  CHECK_FALSE (MachineAllocation::assign (*f, *graph, *compact, *lr));
  auto other_level = MachineLiveness::analyze (*f, *graph, {.optimization_level = 0});
  REQUIRE (other_level);
  CHECK_FALSE (MachineAllocation::assign (*f, *graph, *other_level, *lr));
  auto no_frequency = MachineLiveness::analyze (*f, *graph, {.calculate_frequencies = false});
  REQUIRE (no_frequency);
  CHECK_FALSE (MachineAllocation::assign (*f, *graph, *no_frequency, *lr));
  auto other = function (false);
  auto foreign = reg (*other);
  auto result = allocated (*f);
  CHECK_FALSE (result.location (*foreign));
  CHECK_FALSE (result.live_length (*foreign));
  reg (*f);
  CHECK_FALSE (result.current (*f));
  CHECK_FALSE (result.location (*a));
  CHECK_FALSE (MachineAllocation::assign (*f, *graph, *live, *lr));
}
TEST_CASE ("owning assignment splits another whole-block gap at the exact profitability boundary") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned uses : {2, 3}) {
      auto f = function ();
      only_first_register (*f);
      MachineRewriter rw (*f);
      auto &entry = f->create_block (), &middle = f->create_block (), &exit = f->create_block ();
      REQUIRE (f->create_label (middle));
      REQUIRE (f->create_label (exit));
      auto a = reg (*f), c = reg (*f);
      emit (rw, entry, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (1))});
      emit (rw, entry, MachinePseudoOpcode::Use, std::vector<MachineOperand> (20, operand (a)));
      emit (rw, middle, Opcode::Mov, {operand (c), MachineOperand (std::int64_t (2))});
      emit (rw, middle, MachinePseudoOpcode::Use, std::vector<MachineOperand> (uses, operand (c)));
      emit (rw, exit, Opcode::Ret, {operand (a)});
      auto allocation = allocated (*f, level);
      bool split = level >= 2 && uses == 3;
      CHECK (std::holds_alternative<target::PhysicalRegister> (*allocation.location (*a)));
      CHECK (std::holds_alternative<target::PhysicalRegister> (*allocation.location (*c)) == split);
      CHECK (allocation.stack_bytes () == (split ? 0 : 8));
      CHECK (allocation.spill_blocks ()[3].gen.contains (f->target ().registers ().size ())
             == split);
      CHECK (allocation.gaps ().size () == (level >= 2 && !split ? 1 : 0));
    }
}
TEST_CASE ("owning assignment can spill its own whole-block gap around a busy physical register") {
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function ();
    only_first_register (*f);
    MachineRewriter rw (*f);
    auto &entry = f->create_block (), &middle = f->create_block (), &exit = f->create_block ();
    REQUIRE (f->create_label (middle));
    REQUIRE (f->create_label (exit));
    auto a = reg (*f);
    emit (rw, entry, Opcode::Mov, {operand (a), MachineOperand (std::int64_t (42))});
    emit (rw, entry, MachinePseudoOpcode::Use, std::vector<MachineOperand> (8, operand (a)));
    auto p = f->target ().registers ()[0].reg;
    emit (rw, middle, Opcode::Mov, {MachineOperand (p), MachineOperand (std::int64_t (7))});
    emit (rw, middle, MachinePseudoOpcode::Use, {MachineOperand (p)});
    emit (rw, exit, Opcode::Ret, {operand (a)});
    auto allocation = allocated (*f, level);
    CHECK (std::holds_alternative<target::PhysicalRegister> (*allocation.location (*a))
           == (level >= 2));
    CHECK (allocation.spill_blocks ()[3].gen.contains (f->target ().registers ().size ())
           == (level >= 2));
    CHECK (allocation.gaps ().empty ());
  }
}
TEST_CASE (
  "owning assignment reserves compilation-environment globals and rejects mixed environments") {
  auto f = function (false);
  auto &b = f->create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE (graph);
  auto normal = MachineLiveness::analyze (*f, *graph);
  REQUIRE (normal);
  auto lr = MachineLiveRanges::build (*f, *graph, *normal);
  REQUIRE (lr);
  std::vector<target::PhysicalRegister> globals;
  for (auto &p : f->target ().registers ()) globals.push_back (p.reg);
  auto environment = MachineLiveness::analyze (*f, *graph, {.global_registers = globals});
  REQUIRE (environment);
  CHECK_FALSE (MachineAllocation::assign (*f, *graph, *environment, *lr));
  auto env_ranges = MachineLiveRanges::build (*f, *graph, *environment);
  REQUIRE (env_ranges);
  auto allocation = MachineAllocation::assign (*f, *graph, *environment, *env_ranges);
  REQUIRE (allocation);
  CHECK (std::holds_alternative<StackLocation> (*allocation->location (*a)));
  for (auto &p : allocation->used ())
    for (auto r : globals) CHECK (p.physical.contains (r.encoding ()));
}

TEST_CASE ("call-crossing allocation respects the preserved width of SIMD registers") {
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  if (target->architecture () != target::Architecture::AArch64) return;
  for (unsigned level = 0; level < 4; ++level)
    for (bool crossing : {false, true})
      for (auto type : {Type (Type::Kind::F64), Type (Type::Kind::LongDouble)}) {
        CAPTURE (level);
        CAPTURE (crossing);
        auto f = function (false);
        auto value = f->create_register (type);
        REQUIRE (value);
        const bool wide = (*value)->type ().kind () == Type::Kind::LongDouble;
        auto &block = f->create_block ();
        MachineRewriter rw (*f);
        auto proto = f->intern_symbol (
          {"native", "p", MachineSymbol::Kind::Prototype, Signature{}, false, {}});
        auto callee
          = f->intern_symbol ({"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
        REQUIRE (proto);
        REQUIRE (callee);
        auto define = [&] {
          emit (rw, block, wide ? Opcode::LDMov : Opcode::DMov,
                {operand (*value), wide ? MachineOperand (1.25L) : MachineOperand (1.25)});
        };
        if (crossing) define ();
        emit (rw, block, Opcode::Call,
              {MachineOperand (MachineSymbolReference{*proto}),
               MachineOperand (MachineSymbolReference{*callee})});
        if (!crossing) define ();
        emit (rw, block, MachinePseudoOpcode::Use, {operand (*value)});
        emit (rw, block, Opcode::Ret, {});
        auto allocation = allocated (*f, level);
        auto location = allocation.location (**value);
        REQUIRE (location);
        CHECK (std::holds_alternative<StackLocation> (*location) == (wide && crossing));
      }
}
