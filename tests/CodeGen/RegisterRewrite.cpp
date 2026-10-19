/* Licensed under the MIT License. Allocation rewrite on owning machine nodes. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/RegisterRewrite.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool arm() { return target::TargetMachine::native()->architecture() == target::Architecture::AArch64; }
std::unique_ptr<MachineFunction> function(bool variadic = false) {
  auto t = target::TargetMachine::native(); REQUIRE(t);
  auto f = std::make_unique<MachineFunction>("native", "rewrite", Signature{{}, {}, variadic}, *t);
  MachineRewriter rw(*f); auto state = f->state(); state.machinized = state.operands_legalized = true; REQUIRE(rw.set_state(state)); return f;
}
MachineVirtualRegister *reg(MachineFunction &f, Type type = Type::i64(), std::optional<target::PhysicalRegister> binding = {}) {
  auto r = f.create_register(type, {}, false, false, binding); REQUIRE(r); return *r;
}
MachineOperand op(const MachineVirtualRegister *r) { return MachineOperand(*r); }
MachineOperand integer(std::int64_t n) { return MachineOperand(n); }
MachineInstruction *emit(MachineRewriter &rw, MachineBasicBlock &b, MachineOpcode code, std::vector<MachineOperand> operands = {}) {
  auto r = rw.append(b, code, std::move(operands)); INFO((r ? "" : r.error().message)); REQUIRE(r); return *r;
}
void force_stack(MachineFunction &f) {
  std::vector<target::PhysicalRegister> all; for (auto &p : f.target().registers()) if (!p.reserved) all.push_back(p.reg);
  MachineRewriter rw(f); REQUIRE(rw.set_global_registers(all));
}
MachineRewriteResult rewrite(MachineFunction &f, unsigned level = 2) {
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  auto live = MachineLiveness::analyze(f, *graph, {.optimization_level = level}); REQUIRE(live);
  auto ranges = MachineLiveRanges::build(f, *graph, *live); INFO((ranges ? "" : ranges.error().message)); REQUIRE(ranges);
  auto a = MachineAllocation::assign(f, *graph, *live, *ranges); REQUIRE(a);
  auto result = rewrite_allocated(f, *graph, *live, *ranges, std::move(*a)); INFO((result ? "" : result.error().message)); REQUIRE(result);
  REQUIRE(f.verify()); CHECK(result->current(f)); CHECK(f.state().operands_rewritten); CHECK_FALSE(graph->current(f)); CHECK_FALSE(live->current(f)); return std::move(*result);
}
}
TEST_CASE("owning allocation rewrite preserves node identities and replaces virtual operands") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function(); auto &b = f->create_block(); MachineRewriter rw(*f); auto a = reg(*f), c = reg(*f);
    auto first = emit(rw, b, Opcode::Mov, {op(a), integer(42)}); auto serial = first->creation_order();
    auto add = emit(rw, b, Opcode::Add, {op(c), op(a), integer(1)}); emit(rw, b, MachinePseudoOpcode::Use, {op(c)}); emit(rw, b, Opcode::Ret);
    auto result = rewrite(*f, level); CHECK(&first->block() == &b); CHECK(first->creation_order() == serial); CHECK(add->operands()[0].get<target::PhysicalRegister>());
    CHECK(result.allocation().stack_bytes() == 0); CHECK_FALSE(*result.spill_slot(*a));
    auto other = function(); auto foreign = reg(*other); CHECK_FALSE(result.spill_slot(*foreign));
    auto before = f->dump(); CHECK_FALSE(rw.insert_before(*first, Opcode::Mov, {op(a), integer(0)})); CHECK(f->dump() == before);
    reg(*f); CHECK_FALSE(result.spill_slot(*a));
  }
}
TEST_CASE("owning allocation rewrite uses direct stack moves and ordered reloads without C state") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto f = function(); force_stack(*f); auto &b = f->create_block(); MachineRewriter rw(*f); auto a = reg(*f), c = reg(*f);
    auto immediate = emit(rw, b, Opcode::Mov, {op(a), integer(42)});
    auto copied = emit(rw, b, Opcode::Mov, {op(c), op(a)});
    auto add = emit(rw, b, Opcode::Add, {op(c), op(c), op(a)}); emit(rw, b, MachinePseudoOpcode::Use, {op(c)}); emit(rw, b, Opcode::Ret);
    auto result = rewrite(*f, level); CHECK(result.allocation().stack_bytes() == 16);
    REQUIRE(immediate->operands()[0].get<target::PhysicalRegister>()); REQUIRE(copied->operands()[0].get<MachineMemory>());
    CHECK(copied->operands()[0].get<MachineMemory>()->base == MachineRegister(f->target().registers()[29].reg));
    auto before = rw.previous(*add), after = rw.next(*add); REQUIRE(before); REQUIRE(after); REQUIRE(*before); REQUIRE(*after);
    CHECK((*before)->operands()[1].get<MachineMemory>()); CHECK((*after)->operands()[0].get<MachineMemory>());
    CHECK(add->operands()[1].get<target::PhysicalRegister>()->encoding() == 9); CHECK(add->operands()[2].get<target::PhysicalRegister>()->encoding() == 10);
  }
}
TEST_CASE("owning allocation rewrite materializes all local address widths and frame header offsets") {
  if (!arm()) return;
  for (bool extended : {false, true}) for (auto code : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    auto f = function(extended); auto &b = f->create_block(); MachineRewriter rw(*f); auto a = reg(*f), pointer = reg(*f);
    auto address = emit(rw, b, code, {op(pointer), op(a)}); emit(rw, b, MachinePseudoOpcode::Use, {op(pointer)}); emit(rw, b, Opcode::Ret);
    auto result = rewrite(*f); CHECK(address->opcode() == MachineOpcode(Opcode::Mov)); CHECK(std::holds_alternative<StackLocation>(*result.allocation().location(*a)));
    auto add = rw.previous(*address); REQUIRE(add); REQUIRE(*add); CHECK((*add)->opcode() == MachineOpcode(Opcode::Add));
    auto offset = rw.previous(**add); REQUIRE(offset); REQUIRE(*offset); CHECK(*(*offset)->operands()[1].get<std::int64_t>() == (extended ? 32 : 16));
  }
}
TEST_CASE("owning allocation rewrite preloads a fully spilled indexed store before its value") {
  if (!arm()) return;
  for (auto scale : {1u, 2u, 4u, 8u}) {
    auto f = function(); force_stack(*f); auto &b = f->create_block(); MachineRewriter rw(*f); auto a = reg(*f), index = reg(*f), value = reg(*f);
    emit(rw, b, Opcode::Mov, {op(a), integer(4096)}); emit(rw, b, Opcode::Mov, {op(index), integer(2)}); emit(rw, b, Opcode::Mov, {op(value), integer(42)});
    auto store = emit(rw, b, Opcode::Mov, {MachineOperand(MachineMemory{Type::i64(), a, index, 0, scale, "object", "other"}), op(value)}); emit(rw, b, Opcode::Ret);
    rewrite(*f); auto m = store->operands()[0].get<MachineMemory>(); REQUIRE(m); CHECK(m->base == MachineRegister(f->target().registers()[9].reg)); CHECK_FALSE(m->index);
    CHECK(m->alias == "object"); CHECK(m->nonalias == "other"); CHECK(store->operands()[1].get<target::PhysicalRegister>()->encoding() == 10);
    unsigned shifts = 0; for (auto &i : b.instructions()) shifts += i.opcode() == MachineOpcode(Opcode::Lsh); CHECK(shifts == (scale != 1));
  }
}
TEST_CASE("owning allocation rewrite keeps floating reload registers distinct from address temporaries") {
  if (!arm()) return;
  for (bool single : {false, true}) {
    auto f = function(); force_stack(*f); auto &b = f->create_block(); MachineRewriter rw(*f);
    auto t = single ? Type(Type::Kind::F32) : Type(Type::Kind::F64); auto a = reg(*f, t), c = reg(*f, t);
    emit(rw, b, single ? Opcode::FMov : Opcode::DMov, {op(a), single ? MachineOperand(1.0f) : MachineOperand(1.0)});
    auto add = emit(rw, b, single ? Opcode::FAdd : Opcode::DAdd, {op(c), op(a), op(a)});
    emit(rw, b, MachinePseudoOpcode::Use, {op(c)}); emit(rw, b, Opcode::Ret); rewrite(*f);
    CHECK(add->operands()[0].get<target::PhysicalRegister>()->encoding() == 48);
    CHECK(add->operands()[1] == add->operands()[2]); CHECK(add->operands()[1].get<target::PhysicalRegister>()->encoding() == 48);
  }
}
TEST_CASE("owning allocation rewrite expands out-of-range stack offsets in exact instruction order") {
  if (!arm()) return;
  auto f = function(); force_stack(*f); auto &b = f->create_block(); MachineRewriter rw(*f);
  // Addressable locals reserve nonreusable slots even without a live range.
  for (unsigned n = 0; n < 4096; ++n) { auto r = f->create_register(Type::i64(), {}, false, true); REQUIRE(r); }
  auto value = reg(*f), pointer = reg(*f);
  auto mov = emit(rw, b, Opcode::Mov, {op(value), integer(42)}); emit(rw, b, Opcode::Addr, {op(pointer), op(value)});
  emit(rw, b, MachinePseudoOpcode::Use, {op(pointer)}); emit(rw, b, Opcode::Ret); auto result = rewrite(*f);
  auto store = rw.next(*mov); REQUIRE(store); REQUIRE(*store); CHECK((*store)->opcode() == MachineOpcode(Opcode::Mov));
  REQUIRE((*store)->operands()[1].get<std::int64_t>()); CHECK(*(*store)->operands()[1].get<std::int64_t>() >= 32768);
  auto add = rw.next(**store); REQUIRE(add); REQUIRE(*add); CHECK((*add)->opcode() == MachineOpcode(Opcode::Add));
  auto written = rw.next(**add); REQUIRE(written); REQUIRE(*written); REQUIRE((*written)->operands()[0].get<MachineMemory>());
  CHECK((*written)->operands()[0].get<MachineMemory>()->displacement == 0);
  CHECK(result.allocation().reusable_stack_offset() > 32768);
}
TEST_CASE("owning allocation rewrite deletes redundant moves while retaining empty block anchors") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) for (bool labelled : {false, true}) {
    auto f = function(); auto &b = f->create_block(), &exit = f->create_block(); if (labelled) REQUIRE(f->create_label(b));
    MachineRewriter rw(*f); auto a = reg(*f, Type::i64(), f->target().registers()[0].reg);
    emit(rw, b, Opcode::Mov, {op(a), op(a)}); emit(rw, exit, Opcode::Ret); auto result = rewrite(*f, level);
    CHECK(result.deleted_moves() == 1); CHECK(f->instruction_count() == (level && !labelled ? 2 : 1));
    if (level && !labelled) CHECK(b.instructions().begin()->opcode() == MachineOpcode(MachinePseudoOpcode::Use));
  }
}
TEST_CASE("owning allocation rewrite preserves live hardware values across repeated calls") {
  if (!arm()) return;
  for (unsigned level : {2, 3}) {
    auto f = function(); auto &b = f->create_block(); MachineRewriter rw(*f); auto p = f->target().registers()[2].reg;
    auto a = reg(*f, Type::i64(), p); auto proto = f->intern_symbol({"native", "p", MachineSymbol::Kind::Prototype, Signature{}, false, {}}); REQUIRE(proto);
    auto ext = f->intern_symbol({"native", "ext", MachineSymbol::Kind::Import, {}, false, {}}); REQUIRE(ext);
    emit(rw, b, Opcode::Mov, {op(a), integer(42)}); std::vector<MachineInstruction *> calls;
    for (unsigned n = 0; n < 2; ++n) {
      auto call = emit(rw, b, Opcode::Call, {MachineOperand(MachineSymbolReference{*proto}), MachineOperand(MachineSymbolReference{*ext})});
      REQUIRE(rw.lower_call(*call, call->opcode(), std::vector<MachineOperand>(call->operands().begin(), call->operands().end()))); calls.push_back(call);
      REQUIRE(rw.set_call_arguments(*call, std::span(&p, 1)));
      emit(rw, b, MachinePseudoOpcode::Use, {op(a)});
    }
    emit(rw, b, Opcode::Ret); auto result = rewrite(*f, level); auto slot = result.spill_slot(*a); REQUIRE(slot); REQUIRE(*slot);
    CHECK(result.allocation().stack_bytes() == 8);
    for (auto call : calls) { auto next = rw.next(*call); REQUIRE(next); REQUIRE(*next); REQUIRE((*next)->operands()[1].get<MachineMemory>());
      CHECK((*next)->operands()[1].get<MachineMemory>()->displacement == std::int64_t((**slot).offset + 16)); }
  }
}
TEST_CASE("owning allocation rewrite rejects invalid phases stale inputs and foreign allocations") {
  if (!arm()) return;
  auto f = function(); auto &b = f->create_block(); MachineRewriter rw(*f); auto a = reg(*f);
  emit(rw, b, Opcode::Mov, {op(a), integer(42)}); emit(rw, b, MachinePseudoOpcode::Use, {op(a)}); emit(rw, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build(*f); REQUIRE(graph); auto live = MachineLiveness::analyze(*f, *graph); REQUIRE(live);
  auto ranges = MachineLiveRanges::build(*f, *graph, *live); REQUIRE(ranges); auto allocation = MachineAllocation::assign(*f, *graph, *live, *ranges); REQUIRE(allocation);
  auto simple_live = MachineLiveness::analyze(*f, *graph, {.optimization_level = 0}); REQUIRE(simple_live);
  auto simple_ranges = MachineLiveRanges::build(*f, *graph, *simple_live); REQUIRE(simple_ranges);
  auto simple_allocation = MachineAllocation::assign(*f, *graph, *simple_live, *simple_ranges); REQUIRE(simple_allocation);
  auto original = f->dump(); CHECK_FALSE(rewrite_allocated(*f, *graph, *live, *ranges, *simple_allocation)); CHECK(f->dump() == original);
  auto other = function(); auto before = f->dump(); CHECK_FALSE(rewrite_allocated(*other, *graph, *live, *ranges, *allocation)); CHECK(f->dump() == before);
  reg(*f); before = f->dump(); CHECK_FALSE(rewrite_allocated(*f, *graph, *live, *ranges, *allocation)); CHECK(f->dump() == before);
  auto result = rewrite(*f); before = f->dump(); CHECK_FALSE(rewrite_allocated(*f, *graph, *live, *ranges, result.allocation())); CHECK(f->dump() == before);
}
