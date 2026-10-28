/* Licensed under the MIT License. Owning allocation preparation regressions. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/RegisterAllocation.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
std::unique_ptr<MachineFunction> function (bool result = true) {
  auto t = target::TargetMachine::native ();
  REQUIRE ((t).has_value ());
  return std::make_unique<MachineFunction> ("native", "allocation",
                                            Signature{result ? std::vector{Type::i64 ()}
                                                             : std::vector<Type>{},
                                                      {}},
                                            (t).value ());
}
MachineVirtualRegister *reg (MachineFunction &f,
                             std::optional<target::PhysicalRegister> binding = {}) {
  auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 (), {}, false, false,
                                                                 binding);
  REQUIRE ((r).has_value ());
  return &(r).value ();
}
MachineOperand operand (MachineVirtualRegister *r) {
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (*r);
}
MachineInstruction *emit (MachineRewriter &rw, MachineBasicBlock &b, MachineOpcode op,
                          std::vector<MachineOperand> operands) {
  auto i = rw.append (b, op, std::move (operands));
  REQUIRE ((i).has_value ());
  return &(i).value ();
}
MachineLiveRanges ranges (const MachineFunction &f, unsigned level = 2,
                          std::span<const MachineRegister> scan = {}) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value (),
                                        {.optimization_level = level, .scan_registers = scan});
  REQUIRE ((live).has_value ());
  auto result = MachineLiveRanges::build (f, (graph).value (), (live).value ());
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
MachineAllocation allocated (const MachineFunction &f, unsigned level = 2) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
  REQUIRE ((live).has_value ());
  auto lr = MachineLiveRanges::build (f, (graph).value (), (live).value ());
  REQUIRE ((lr).has_value ());
  auto result = MachineAllocation::assign (f, (graph).value (), (live).value (), (lr).value ());
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
void force_stack (MachineFunction &f) {
  std::vector<target::PhysicalRegister> all;
  for (auto &p : f.target ().registers ())
    if (!p.reserved) all.push_back (p.reg);
  MachineRewriter rw (f);
  REQUIRE ((rw.set_global_registers (all)).has_value ());
}
void only_first_register (MachineFunction &f) {
  std::vector<target::PhysicalRegister> all;
  for (auto &p : f.target ().registers ())
    if (!p.reserved && p.reg.encoding ()) all.push_back (p.reg);
  MachineRewriter rw (f);
  REQUIRE ((rw.set_global_registers (all)).has_value ());
}
}  // namespace
TEST_CASE ("owning coalescing retains original O0 O1 conditions and merges O2 O3 copy chains") {
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), d = reg (*f);
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    emit (rw, b, Opcode::Mov, {operand (d), operand (c)});
    auto ret = emit (rw, b, Opcode::Ret, {operand (d)});
    auto serial = ret->creation_order ();
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE ((graph).has_value ());
    auto merged = coalesce_registers (*f, (graph).value (), level);
    REQUIRE ((merged).has_value ());
    CHECK (merged.value ().enabled () == (level >= 2));
    CHECK (merged.value ().coalesced_moves () == (level >= 2 ? 2 : 0));
    CHECK ((merged.value ().representative (
              mirnext::codegen::detail::NodeAccess::make_MachineRegister (d)))
             .value ()
           == mirnext::codegen::detail::NodeAccess::make_MachineRegister (level >= 2 ? a : d));
    CHECK (f->instruction_count () == (level >= 2 ? 2 : 4));
    CHECK (ret->creation_order () == serial);
    REQUIRE ((f->verify ()).has_value ());
    auto lr = ranges (*f, level);
    REQUIRE (
      (lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (level >= 2 ? a : d)))
        .has_value ());
  }
}
TEST_CASE ("owning coalescing retains conflicts and tests all previously merged group members") {
  for (bool chain : {false, true}) {
    auto f = function (false);
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), d = reg (*f);
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    if (chain) emit (rw, b, Opcode::Mov, {operand (d), operand (c)});
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
    emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (chain ? d : c)});
    emit (rw, b, Opcode::Ret, {});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE ((graph).has_value ());
    auto merged = coalesce_registers (*f, (graph).value ());
    REQUIRE ((merged).has_value ());
    CHECK (merged.value ().coalesced_moves () == (chain ? 1 : 0));
    CHECK (
      (merged.value ().conflicts (mirnext::codegen::detail::NodeAccess::make_MachineRegister (a),
                                  mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                    chain ? d : c)))
        .value ());
    CHECK ((merged.value ().representative (
              mirnext::codegen::detail::NodeAccess::make_MachineRegister (chain ? d : c)))
             .value ()
           == mirnext::codegen::detail::NodeAccess::make_MachineRegister (chain ? d : c));
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
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (64))});
    emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
    auto load
      = emit (rw, b, Opcode::Mov,
              {operand (value),
               mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                 mirnext::codegen::detail::NodeAccess::make_MachineMemory (Type::i64 (), c, c, 8, 1,
                                                                           "alias", "nonalias"))});
    emit (rw, b, Opcode::Ret, {operand (value)});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE ((graph).has_value ());
    auto merged = coalesce_registers (*f, (graph).value ());
    REQUIRE ((merged).has_value ());
    auto representative = bound == 0 ? a : c;
    CHECK (merged.value ().coalesced_moves () == (bound == 2 ? 0 : 1));
    CHECK ((merged.value ().representative (
              mirnext::codegen::detail::NodeAccess::make_MachineRegister (c)))
             .value ()
           == mirnext::codegen::detail::NodeAccess::make_MachineRegister (representative));
    CHECK (load->operands ()[1].get<MachineMemory> ().value ().base
           == mirnext::codegen::detail::NodeAccess::optional_register (representative));
    CHECK (load->operands ()[1].get<MachineMemory> ().value ().alias == "alias");
    REQUIRE ((f->verify ()).has_value ());
  }
}
TEST_CASE (
  "owning coalescing preserves empty block anchors and checks thresholds and stale views") {
  for (unsigned limit : {2, 3}) {
    auto f = function (false);
    auto &entry = mirnext::codegen::detail::access (*(f)).create_block (),
         &exit = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f);
    emit (rw, entry, Opcode::Mov, {operand (c), operand (a)});
    emit (rw, exit, Opcode::Ret, {});
    auto graph = MachineGraphAnalysis::build (*f);
    REQUIRE ((graph).has_value ());
    auto result = coalesce_registers (*f, (graph).value (), 2, limit);
    REQUIRE ((result).has_value ());
    CHECK (result.value ().enabled () == (limit == 3));
    if (limit == 3) {
      CHECK_FALSE (entry.labels ().empty ());
      CHECK_FALSE (graph.value ().current (*f));
    }
    auto lr = ranges (*f);
    CHECK (lr.current (*f));
    reg (*f);
    CHECK_FALSE (result.value ().current (*f));
    CHECK_FALSE ((result.value ().representative (
                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (c)))
                   .has_value ());
    CHECK_FALSE (
      (lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (a))).has_value ());
    CHECK_FALSE ((coalesce_registers (*f, (graph).value ())).has_value ());
  }
}
TEST_CASE ("owning live ranges retain unused definitions and compressed neighboring lifetimes") {
  auto f = function (false);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
  emit (rw, b, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
  emit (rw, b, Opcode::Ret, {});
  auto lr = ranges (*f);
  auto p = f->target ().registers ().size ();
  CHECK (lr.uncompressed_ranges ()[p] == std::vector<MachineLiveRange>{{1, 1}, {0, 0}});
  CHECK (lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (a)).value ().data ()
         == lr.all_ranges ()[p].data ());
  CHECK (lr.all_ranges ()[p] == std::vector<MachineLiveRange>{{0, 1}});
  CHECK (lr.point_count () == 2);
  CHECK (lr.uncompressed_point_count () == 2);
}
TEST_CASE ("owning whole-block ranges preserve label anchors and ordered compression") {
  auto f = function ();
  auto &entry = mirnext::codegen::detail::access (*(f)).create_block (),
       &first = mirnext::codegen::detail::access (*(f)).create_block (),
       &second = mirnext::codegen::detail::access (*(f)).create_block (),
       &exit = mirnext::codegen::detail::access (*(f)).create_block ();
  REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (first)).has_value ());
  REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (second)).has_value ());
  auto a = reg (*f);
  MachineRewriter rw (*f);
  emit (rw, entry, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
  emit (rw, exit, Opcode::Ret, {operand (a)});
  auto lr = ranges (*f);
  auto p = f->target ().registers ().size ();
  REQUIRE (lr.all_ranges ()[p].size () == 3);
  CHECK (mirnext::codegen::detail::NodeAccess::blocks (lr.all_ranges ()[p][1])
         == std::vector<const MachineBasicBlock *>{&first, &second});
  CHECK (lr.spill_blocks ()[3].kill.empty ());
  std::array<MachineRegister, 1> scan{
    mirnext::codegen::detail::NodeAccess::make_MachineRegister (a)};
  auto compact = ranges (*f, 2, scan);
  CHECK (compact.is_compact ());
  CHECK (compact.scan_registers ()[0]
         == mirnext::codegen::detail::NodeAccess::make_MachineRegister (a));
}
TEST_CASE ("owning instruction creation order survives layout movement and replacement") {
  auto f = function (false);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f);
  auto first = emit (rw, b, Opcode::Mov, {operand (c), operand (a)});
  auto second = emit (rw, b, Opcode::Mov, {operand (a), operand (c)});
  auto one = first->creation_order (), two = second->creation_order ();
  CHECK (one < two);
  REQUIRE ((rw.move_before (*second, *first)).has_value ());
  REQUIRE ((rw.replace (*first, Opcode::Mov, {operand (c), operand (a)})).has_value ());
  CHECK (first->creation_order () == one);
  CHECK (second->creation_order () == two);
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  auto result = coalesce_registers (*f, (graph).value ());
  REQUIRE ((result).has_value ());
  CHECK (result.value ().coalesced_moves () == 2);
}
TEST_CASE ("owning live ranges distinguish surviving call values and physical argument lifetimes") {
  for (bool survives : {false, true}) {
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f);
    auto proto = mirnext::codegen::detail::access (*(f)).intern_symbol (
      {"native", "p", MachineSymbol::Kind::Prototype, Signature{{}, {Type::i64 ()}}, false, {}});
    REQUIRE ((proto).has_value ());
    auto callee = mirnext::codegen::detail::access (*(f)).intern_symbol (
      {"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
    REQUIRE ((callee).has_value ());
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
    auto call = emit (rw, b, Opcode::Call,
                      {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                         mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                           &(proto).value ())),
                       mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                         mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                           &(callee).value ())),
                       operand (a)});
    auto argument = f->target ().registers ()[1].reg;
    REQUIRE ((rw.set_call_arguments (*call, std::span (&argument, 1))).has_value ());
    emit (rw, b, Opcode::Ret,
          {survives
             ? operand (a)
             : mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    auto lr = ranges (*f);
    CHECK (lr.call_crossed ().contains (f->target ().registers ().size ()) == survives);
    CHECK_FALSE (lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (argument))
                   .value ()
                   .empty ());
    std::array<MachineRegister, 1> scan{
      mirnext::codegen::detail::NodeAccess::make_MachineRegister (a)};
    auto compact = ranges (*f, 2, scan);
    CHECK (compact.call_crossed ().empty ());
    CHECK (compact.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (argument))
             .value ()
             .empty ());
  }
}
TEST_CASE ("owning range construction accounts for early clobbers and rejects stale input") {
  auto f = function ();
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f);
  emit (rw, b, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
  emit (rw, b, Opcode::Mov,
        {operand (c),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (6))});
  emit (rw, b, Opcode::UMulo, {operand (a), operand (a), operand (c)});
  emit (rw, b, Opcode::Ret, {operand (a)});
  auto lr = ranges (*f);
  for (auto r : early_clobbers (f->target (), Opcode::UMulo))
    if (r)
      CHECK_FALSE (lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (*r))
                     .value ()
                     .empty ());
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (*f, (graph).value ());
  REQUIRE ((live).has_value ());
  reg (*f);
  CHECK_FALSE ((MachineLiveRanges::build (*f, (graph).value (), (live).value ())).has_value ());
}
TEST_CASE (
  "owning packed conflicts preserve symmetry across word boundaries and reject foreign queries") {
  auto f = function (false);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  std::vector<MachineVirtualRegister *> regs;
  for (unsigned n = 0; n < 70; ++n) regs.push_back (reg (*f));
  for (unsigned n = 1; n < regs.size (); ++n)
    emit (rw, b, Opcode::Mov, {operand (regs[n]), operand (regs[0])});
  for (unsigned n = 0; n < regs.size (); ++n)
    emit (rw, b, Opcode::Mov,
          {operand (regs[n]),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (n))});
  std::vector<MachineOperand> uses;
  for (auto r : regs) uses.push_back (operand (r));
  emit (rw, b, MachinePseudoOpcode::Use, std::move (uses));
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  CHECK_FALSE ((coalesce_registers (*f, (graph).value (), 4)).has_value ());
  CHECK_FALSE ((coalesce_registers (*f, (graph).value (), 2, 0)).has_value ());
  auto result = coalesce_registers (*f, (graph).value ());
  REQUIRE ((result).has_value ());
  CHECK (result.value ().coalesced_moves () == 0);
  for (auto a : regs)
    for (auto c : regs)
      CHECK (
        (result.value ().conflicts (mirnext::codegen::detail::NodeAccess::make_MachineRegister (a),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (c)))
          .value ()
        == (a != c));
  auto other = function (false);
  auto foreign = reg (*other);
  CHECK_FALSE ((result.value ().representative (
                  mirnext::codegen::detail::NodeAccess::make_MachineRegister (foreign)))
                 .has_value ());
  CHECK_FALSE ((result.value ()
                  .conflicts (mirnext::codegen::detail::NodeAccess::make_MachineRegister (regs[0]),
                              mirnext::codegen::detail::NodeAccess::make_MachineRegister (foreign)))
                 .has_value ());
  auto lr = ranges (*f);
  CHECK_FALSE ((lr.ranges (mirnext::codegen::detail::NodeAccess::make_MachineRegister (foreign)))
                 .has_value ());
}
TEST_CASE (
  "owning assignment preserves declaration identity frequency priority and unused values") {
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function (false);
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = reg (*f), c = reg (*f), unused = reg (*f);
    emit (rw, b, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
    emit (rw, b, Opcode::Mov,
          {operand (c),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
    emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (c), operand (c)});
    emit (rw, b, Opcode::Ret, {});
    auto revision = f->revision ();
    auto allocation = allocated (*f, level);
    CHECK (f->revision () == revision);
    REQUIRE (allocation.order ().size () == 3);
    CHECK (&allocation.order ()[0] == c);
    CHECK (&allocation.order ()[2] == unused);
    CHECK (std::holds_alternative<std::monostate> ((allocation.location (*unused)).value ()));
    auto x = allocation.location (*a), y = allocation.location (*c);
    REQUIRE ((x).has_value ());
    REQUIRE ((y).has_value ());
    CHECK (std::holds_alternative<target::PhysicalRegister> (
      mirnext::codegen::detail::NodeAccess::storage ((x).value ())));
    CHECK ((x).value () != (y).value ());
    CHECK (allocation.stack_bytes () == 0);
    CHECK (allocation.busy ().empty () == (level < 2));
    CHECK (allocation.current (*f));
  }
}
TEST_CASE ("owning assignment reuses byte-addressed spill locations for disjoint lifetimes") {
  auto f = function (false);
  force_stack (*f);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f), d = reg (*f);
  emit (rw, b, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Mov,
        {operand (c),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
  emit (rw, b, Opcode::Mov,
        {operand (d),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (3))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (c), operand (d)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  auto x = std::get<StackLocation> ((result.location (*a)).value ()),
       y = std::get<StackLocation> ((result.location (*c)).value ()),
       z = std::get<StackLocation> ((result.location (*d)).value ());
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
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f), pointer = reg (*f);
  emit (rw, b, Opcode::Addr, {operand (pointer), operand (a)});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (pointer)});
  emit (rw, b, Opcode::Addr, {operand (pointer), operand (c)});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (pointer)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  CHECK (std::get<StackLocation> ((result.location (*a)).value ()).offset == 0);
  CHECK (std::get<StackLocation> ((result.location (*c)).value ()).offset == 8);
  CHECK (std::get<StackLocation> ((result.location (*pointer)).value ()).offset == 16);
  CHECK (result.reusable_stack_offset () == 16);
  CHECK (result.stack_bytes () == 24);
  CHECK (std::none_of (result.order ().begin (), result.order ().end (),
                       [&] (auto &reg) { return &reg == a; }));
}
TEST_CASE (
  "owning assignment aligns long double spill locations and keeps physical and stack domains "
  "separate") {
  auto f = function (false);
  force_stack (*f);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  auto wide
    = mirnext::codegen::detail::access (*(f)).create_register (Type (Type::Kind::LongDouble));
  REQUIRE ((wide).has_value ());
  emit (rw, b, Opcode::Mov,
        {operand (a),
         mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (&(wide).value ())});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  auto s = std::get<StackLocation> ((result.location ((wide).value ())).value ());
  auto units = wide.value ().type ().kind () == Type::Kind::LongDouble ? 2u : 1u;
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
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto first = f->target ().registers ()[0].reg, second = f->target ().registers ()[1].reg;
  auto a = reg (*f, first), c = reg (*f, second), d = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a), operand (c), operand (d), operand (d)});
  emit (rw, b, Opcode::Ret, {});
  auto result = allocated (*f);
  CHECK (&result.order ()[0] == a);
  CHECK (&result.order ()[1] == c);
  CHECK (std::get<target::PhysicalRegister> (
           mirnext::codegen::detail::NodeAccess::storage ((result.location (*a)).value ()))
         == first);
  CHECK (std::get<target::PhysicalRegister> (
           mirnext::codegen::detail::NodeAccess::storage ((result.location (*c)).value ()))
         == second);
  auto selected = std::get<target::PhysicalRegister> (
    mirnext::codegen::detail::NodeAccess::storage ((result.location (*d)).value ()));
  CHECK (selected != first);
  CHECK (selected != second);
  CHECK (std::find (result.used_registers ().begin (), result.used_registers ().end (), first)
         == result.used_registers ().end ());
}
TEST_CASE ("owning assignment rejects compact mixed foreign and stale analyses") {
  auto f = function (false);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (*f, (graph).value ());
  REQUIRE ((live).has_value ());
  auto lr = MachineLiveRanges::build (*f, (graph).value (), (live).value ());
  REQUIRE ((lr).has_value ());
  std::array<MachineRegister, 1> scan{
    mirnext::codegen::detail::NodeAccess::make_MachineRegister (a)};
  auto compact = MachineLiveness::analyze (*f, (graph).value (), {.scan_registers = scan});
  REQUIRE ((compact).has_value ());
  CHECK_FALSE ((MachineAllocation::assign (*f, (graph).value (), (compact).value (), (lr).value ()))
                 .has_value ());
  auto other_level = MachineLiveness::analyze (*f, (graph).value (), {.optimization_level = 0});
  REQUIRE ((other_level).has_value ());
  CHECK_FALSE (
    (MachineAllocation::assign (*f, (graph).value (), (other_level).value (), (lr).value ()))
      .has_value ());
  auto no_frequency
    = MachineLiveness::analyze (*f, (graph).value (), {.calculate_frequencies = false});
  REQUIRE ((no_frequency).has_value ());
  CHECK_FALSE (
    (MachineAllocation::assign (*f, (graph).value (), (no_frequency).value (), (lr).value ()))
      .has_value ());
  auto other = function (false);
  auto foreign = reg (*other);
  auto result = allocated (*f);
  CHECK_FALSE ((result.location (*foreign)).has_value ());
  CHECK_FALSE ((result.live_length (*foreign)).has_value ());
  reg (*f);
  CHECK_FALSE (result.current (*f));
  CHECK_FALSE ((result.location (*a)).has_value ());
  CHECK_FALSE ((MachineAllocation::assign (*f, (graph).value (), (live).value (), (lr).value ()))
                 .has_value ());
}
TEST_CASE ("owning assignment splits another whole-block gap at the exact profitability boundary") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned uses : {2, 3}) {
      auto f = function ();
      only_first_register (*f);
      MachineRewriter rw (*f);
      auto &entry = mirnext::codegen::detail::access (*(f)).create_block (),
           &middle = mirnext::codegen::detail::access (*(f)).create_block (),
           &exit = mirnext::codegen::detail::access (*(f)).create_block ();
      REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (middle)).has_value ());
      REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (exit)).has_value ());
      auto a = reg (*f), c = reg (*f);
      emit (rw, entry, Opcode::Mov,
            {operand (a),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
      emit (rw, entry, MachinePseudoOpcode::Use, std::vector<MachineOperand> (20, operand (a)));
      emit (rw, middle, Opcode::Mov,
            {operand (c),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (2))});
      emit (rw, middle, MachinePseudoOpcode::Use, std::vector<MachineOperand> (uses, operand (c)));
      emit (rw, exit, Opcode::Ret, {operand (a)});
      auto allocation = allocated (*f, level);
      bool split = level >= 2 && uses == 3;
      CHECK (std::holds_alternative<target::PhysicalRegister> (
        mirnext::codegen::detail::NodeAccess::storage ((allocation.location (*a)).value ())));
      CHECK (std::holds_alternative<target::PhysicalRegister> (
               mirnext::codegen::detail::NodeAccess::storage ((allocation.location (*c)).value ()))
             == split);
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
    auto &entry = mirnext::codegen::detail::access (*(f)).create_block (),
         &middle = mirnext::codegen::detail::access (*(f)).create_block (),
         &exit = mirnext::codegen::detail::access (*(f)).create_block ();
    REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (middle)).has_value ());
    REQUIRE ((mirnext::codegen::detail::access (*(f)).create_label (exit)).has_value ());
    auto a = reg (*f);
    emit (rw, entry, Opcode::Mov,
          {operand (a),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
    emit (rw, entry, MachinePseudoOpcode::Use, std::vector<MachineOperand> (8, operand (a)));
    auto p = f->target ().registers ()[0].reg;
    emit (rw, middle, Opcode::Mov,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand (p),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (7))});
    emit (rw, middle, MachinePseudoOpcode::Use,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand (p)});
    emit (rw, exit, Opcode::Ret, {operand (a)});
    auto allocation = allocated (*f, level);
    CHECK (std::holds_alternative<target::PhysicalRegister> (
             mirnext::codegen::detail::NodeAccess::storage ((allocation.location (*a)).value ()))
           == (level >= 2));
    CHECK (allocation.spill_blocks ()[3].gen.contains (f->target ().registers ().size ())
           == (level >= 2));
    CHECK (allocation.gaps ().empty ());
  }
}
TEST_CASE (
  "owning assignment reserves compilation-environment globals and rejects mixed environments") {
  auto f = function (false);
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f);
  emit (rw, b, MachinePseudoOpcode::Use, {operand (a)});
  emit (rw, b, Opcode::Ret, {});
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  auto normal = MachineLiveness::analyze (*f, (graph).value ());
  REQUIRE ((normal).has_value ());
  auto lr = MachineLiveRanges::build (*f, (graph).value (), (normal).value ());
  REQUIRE ((lr).has_value ());
  std::vector<target::PhysicalRegister> globals;
  for (auto &p : f->target ().registers ()) globals.push_back (p.reg);
  auto environment = MachineLiveness::analyze (*f, (graph).value (), {.global_registers = globals});
  REQUIRE ((environment).has_value ());
  CHECK_FALSE (
    (MachineAllocation::assign (*f, (graph).value (), (environment).value (), (lr).value ()))
      .has_value ());
  auto env_ranges = MachineLiveRanges::build (*f, (graph).value (), (environment).value ());
  REQUIRE ((env_ranges).has_value ());
  auto allocation = MachineAllocation::assign (*f, (graph).value (), (environment).value (),
                                               (env_ranges).value ());
  REQUIRE ((allocation).has_value ());
  CHECK (std::holds_alternative<StackLocation> ((allocation.value ().location (*a)).value ()));
  for (auto &p : allocation.value ().used ())
    for (auto r : globals) CHECK (p.physical.contains (r.encoding ()));
}

TEST_CASE ("call-crossing allocation respects the preserved width of SIMD registers") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  if (target.value ().architecture () != target::Architecture::AArch64) return;
  for (unsigned level = 0; level < 4; ++level)
    for (bool crossing : {false, true})
      for (auto type : {Type (Type::Kind::F64), Type (Type::Kind::LongDouble)}) {
        CAPTURE (level);
        CAPTURE (crossing);
        auto f = function (false);
        auto value = mirnext::codegen::detail::access (*(f)).create_register (type);
        REQUIRE ((value).has_value ());
        const bool wide = value.value ().type ().kind () == Type::Kind::LongDouble;
        auto &block = mirnext::codegen::detail::access (*(f)).create_block ();
        MachineRewriter rw (*f);
        auto proto = mirnext::codegen::detail::access (*(f)).intern_symbol (
          {"native", "p", MachineSymbol::Kind::Prototype, Signature{}, false, {}});
        auto callee = mirnext::codegen::detail::access (*(f)).intern_symbol (
          {"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
        REQUIRE ((proto).has_value ());
        REQUIRE ((callee).has_value ());
        auto define = [&] {
          emit (rw, block, wide ? Opcode::LDMov : Opcode::DMov,
                {operand (&(value).value ()),
                 wide ? mirnext::codegen::detail::NodeAccess::make_MachineOperand (1.25L)
                      : mirnext::codegen::detail::NodeAccess::make_MachineOperand (1.25)});
        };
        if (crossing) define ();
        emit (rw, block, Opcode::Call,
              {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                 mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                   &(proto).value ())),
               mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                 mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                   &(callee).value ()))});
        if (!crossing) define ();
        emit (rw, block, MachinePseudoOpcode::Use, {operand (&(value).value ())});
        emit (rw, block, Opcode::Ret, {});
        auto allocation = allocated (*f, level);
        auto location = allocation.location ((value).value ());
        REQUIRE ((location).has_value ());
        CHECK (std::holds_alternative<StackLocation> ((location).value ()) == (wide && crossing));
      }
}
