/* Licensed under the MIT License. Native post-allocation semantics and lifetime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "PostAllocationFixtures.hpp"
#include <algorithm>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool arm() { return target::TargetMachine::native()->architecture() == target::Architecture::AArch64; }
struct Fixture {
  MachineFunction f{"native","post",Signature{{Type::i64()},{}},*target::TargetMachine::native()};
  MachineRewriter rw{f};
  std::vector<MachineBasicBlock *> blocks;
  Fixture(unsigned count = 1) {
    for (unsigned n = 0; n < count; ++n) blocks.push_back(&f.create_block());
    auto state = f.state(); state.machinized = state.operands_legalized = true; REQUIRE(rw.set_state(state));
  }
  MachineOperand reg(unsigned n) { return MachineOperand(f.target().registers()[n].reg); }
  MachineOperand imm(std::int64_t n) { return MachineOperand(n); }
  MachineOperand mem(unsigned n, std::string alias = {}) { return MachineOperand(MachineMemory{Type::i64(),MachineRegister(f.target().registers()[n].reg),{},0,1,std::move(alias),{}}); }
  MachineOperand label(unsigned b) { if (blocks[b]->labels().empty()) REQUIRE(f.create_label(*blocks[b])); return MachineOperand(*blocks[b]->labels()[0]); }
  MachineInstruction *add(MachineOpcode code, std::vector<MachineOperand> ops = {}, unsigned b = 0) {
    auto r = rw.append(*blocks[b],code,std::move(ops)); INFO((r ? "" : r.error().message)); REQUIRE(r); return *r;
  }
  MachineInstruction *call() {
    auto proto = f.intern_symbol({"native","p",MachineSymbol::Kind::Prototype,Signature{{Type::i64()},{}},false,{}}); REQUIRE(proto);
    auto ext = f.intern_symbol({"native","ext",MachineSymbol::Kind::Import,{},false,{}}); REQUIRE(ext);
    auto i = add(Opcode::Call,{MachineOperand(MachineSymbolReference{*proto}),MachineOperand(MachineSymbolReference{*ext}),reg(1)});
    REQUIRE(rw.lower_call(*i,i->opcode(),std::vector<MachineOperand>(i->operands().begin(),i->operands().end()))); return i;
  }
  bool attached(const MachineInstruction *p) { for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (&i == p) return true; return false; }
  MachineSpillResult spill(unsigned level = 2) {
    auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
    auto live = MachineLiveness::analyze(f,*graph,{.optimization_level=level}); REQUIRE(live);
    auto ranges = MachineLiveRanges::build(f,*graph,*live); REQUIRE(ranges);
    auto allocation = MachineAllocation::assign(f,*graph,*live,*ranges); REQUIRE(allocation);
    auto rewritten = rewrite_allocated(f,*graph,*live,*ranges,std::move(*allocation)); INFO((rewritten ? "" : rewritten.error().message)); REQUIRE(rewritten);
    auto r = split_allocated_spills(f,std::move(*rewritten)); REQUIRE(r); return std::move(*r);
  }
  MachinePostResult run(unsigned level = 2, MachinePostOptions options = {}) {
    auto r = optimize_allocated_machine(f,spill(level),std::move(options)); INFO((r ? "" : r.error().message)); REQUIRE(r);
    REQUIRE(r->current(f)); REQUIRE(f.verify()); REQUIRE(r->cfg().verify(f)); return std::move(*r);
  }
};
}
TEST_CASE("owning post-allocation preserves level conditions observer phases and stable identities") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) {
    Fixture t; auto ret = t.add(Opcode::Ret,{t.reg(1)}); auto identity = ret->creation_order(); std::vector<MachinePostStage> phases;
    auto r = t.run(level,{.observer=[&](auto phase,const auto &view) -> Result<void> { phases.push_back(phase); CHECK(&view.function() == &t.f); return {}; }});
    CHECK(t.f.state().post_allocation_optimized); CHECK(t.attached(ret)); CHECK(ret->creation_order() == identity); CHECK(r.allocation().optimization_level() == level);
    std::vector<MachinePostStage> expected; if (level < 2) expected.push_back(MachinePostStage::StripProperties);
    if (level) expected.insert(expected.end(),{MachinePostStage::Liveness,MachinePostStage::Combine,MachinePostStage::DeadCode}); CHECK(phases == expected);
    REQUIRE(t.f.create_label(*t.blocks[0])); CHECK_FALSE(r.current(t.f));
  }
}
TEST_CASE("owning combine substitutes last-use constants and refuses later uses") {
  if (!arm()) return;
  for (bool later : {false,true}) {
    Fixture t; auto def = t.add(Opcode::Mov,{t.reg(0),t.imm(42)}); auto sum = t.add(Opcode::Add,{t.reg(1),t.reg(1),t.reg(0)});
    if (later) t.add(MachinePseudoOpcode::Use,{t.reg(0)}); t.add(Opcode::Ret,{t.reg(1)});
    auto r = t.run(); CHECK(t.attached(def) == later); CHECK(bool(sum->operands()[2].get<std::int64_t>()) != later); CHECK(r.statistics().substitutions == (later ? 0 : 1));
  }
}
TEST_CASE("owning combine retains memory writes calls and explicit stack or variadic barriers") {
  if (!arm()) return;
  for (unsigned barrier = 0; barrier < 6; ++barrier) {
    Fixture t; auto load = t.add(Opcode::Mov,{t.reg(19),t.mem(6,"source")});
    if (barrier == 1) t.add(Opcode::Mov,{t.mem(7,"disjoint"),t.reg(2)});
    if (barrier == 2) t.call();
    if (barrier == 3) t.add(Opcode::BStart,{t.reg(7)});
    if (barrier == 4) t.add(Opcode::BEnd,{t.reg(7)});
    if (barrier == 5) t.add(Opcode::VaBlockArg,{t.reg(7),t.reg(2),t.imm(8),t.imm(0)});
    auto copy = t.add(Opcode::Mov,{t.reg(1),t.reg(19)}); t.add(Opcode::Ret,{t.reg(1)});
    t.run(2,{.observer=[&](auto phase,const auto &) -> Result<void> {
      if (phase == MachinePostStage::Combine) { CHECK(t.attached(load) == (barrier != 0)); REQUIRE(t.attached(copy)); CHECK(bool(copy->operands()[1].get<MachineMemory>()) == (barrier == 0)); } return {};
    }});
  }
}
TEST_CASE("owning combine refuses obsolete register and memory inputs and unencodable constants") {
  if (!arm()) return;
  for (unsigned kind = 0; kind < 3; ++kind) {
    Fixture t; MachineInstruction *def;
    if (!kind) { def = t.add(Opcode::Mov,{t.reg(0),t.reg(2)}); t.add(Opcode::Mov,{t.reg(2),t.imm(42)}); }
    else if (kind == 1) { def = t.add(Opcode::Mov,{t.reg(0),t.mem(6)}); t.add(Opcode::Mov,{t.reg(6),t.imm(42)}); }
    else def = t.add(Opcode::Mov,{t.reg(0),t.imm(INT64_MAX)});
    auto use = kind == 2 ? t.add(Opcode::Add,{t.reg(1),t.reg(1),t.reg(0)}) : t.add(Opcode::Mov,{t.reg(1),t.reg(0)});
    t.add(Opcode::Ret,{t.reg(1)});
    t.run(2,{.observer=[&](auto phase,const auto &) -> Result<void> { if (phase == MachinePostStage::Combine) { CHECK(t.attached(def)); REQUIRE(t.attached(use)); CHECK(use->operands().back().get<target::PhysicalRegister>()); } return {}; }});
  }
}
TEST_CASE("owning combine retargets producers on stable nodes and honors early clobbers and output addresses") {
  if (!arm()) return;
  for (unsigned kind = 0; kind < 3; ++kind) {
    Fixture t; auto def = t.add(kind == 1 ? Opcode::UMulo : Opcode::Lt,{t.reg(0),t.reg(1),t.reg(2)}); auto identity = def->creation_order();
    auto between = t.add(Opcode::Mov,{t.reg(6),t.imm(17)}); auto copy = t.add(Opcode::Mov,{kind == 2 ? t.mem(0) : t.reg(3),t.reg(0)}); t.add(Opcode::Ret,{t.reg(3)});
    auto r = t.run(2,{.observer=[&](auto phase,const auto &) -> Result<void> {
      if (phase == MachinePostStage::Combine) { CHECK(t.attached(copy) == (kind != 0)); REQUIRE(t.attached(def)); CHECK(def->creation_order() == identity);
        if (!kind) { CHECK(def->operands()[0] == t.reg(3)); CHECK(*t.rw.next(*between) == def); }
      } return {};
    }}); CHECK(r.statistics().moves == (kind ? 0 : 1));
  }
}
TEST_CASE("owning combine handles all extension pairs and transfers filtered death markers") {
  if (!arm()) return;
  std::array codes{Opcode::Ext8,Opcode::UExt8,Opcode::Ext16,Opcode::UExt16,Opcode::Ext32,Opcode::UExt32};
  for (std::size_t a = 0; a < codes.size(); ++a) for (std::size_t b = 0; b < codes.size(); ++b) {
    Fixture t; auto def = t.add(codes[a],{t.reg(0),t.reg(2)}); auto use = t.add(codes[b],{t.reg(1),t.reg(0)}); t.add(Opcode::Ret,{t.reg(1)});
    bool allowed = b/2 <= a/2 || b%2 == 0 || a%2 != 0;
    auto r = t.run(2,{.observer=[&](auto phase,const auto &view) -> Result<void> {
      if (phase == MachinePostStage::Combine) { CHECK(t.attached(def) != allowed); REQUIRE(t.attached(use)); CHECK(use->opcode() == MachineOpcode(allowed && b/2 > a/2 ? codes[a] : codes[b])); CHECK(use->operands()[1] == t.reg(allowed ? 2 : 0));
        if (allowed) { REQUIRE(view.deaths(*use).size() == 1); CHECK(view.deaths(*use)[0] == MachineRegister(t.f.target().registers()[2].reg)); }
      } return {};
    }}); CHECK(r.statistics().extensions == (allowed ? 1 : 0));
  }
}
TEST_CASE("owning combine exchanges integer operands and preserves ordered floating comparisons") {
  if (!arm()) return;
  Fixture t; t.add(Opcode::Mov,{t.reg(0),t.imm(42)}); auto sum = t.add(Opcode::Add,{t.reg(1),t.reg(0),t.reg(1)}); t.add(Opcode::Ret,{t.reg(1)});
  auto r = t.run(); CHECK(r.statistics().swaps == 1); CHECK(sum->operands()[1] == t.reg(1)); CHECK(sum->operands()[2] == t.imm(42));
  for (auto op : {Opcode::FLt,Opcode::FLe,Opcode::FGt,Opcode::FGe,Opcode::DLt,Opcode::DLe,Opcode::DGt,Opcode::DGe}) {
    Fixture f; bool single = op == Opcode::FLt || op == Opcode::FLe || op == Opcode::FGt || op == Opcode::FGe;
    auto def = f.add(single ? Opcode::FMov : Opcode::DMov,{f.reg(32),single ? MachineOperand(0.0f) : MachineOperand(0.0)});
    auto compare = f.add(op,{f.reg(1),f.reg(32),f.reg(33)}); f.add(Opcode::Ret,{f.reg(1)}); auto result = f.run();
    CHECK(f.attached(def)); CHECK(compare->opcode() == MachineOpcode(op)); CHECK(compare->operands()[1] == f.reg(32)); CHECK(result.statistics().swaps == 0);
  }
}
TEST_CASE("owning post-allocation removes properties by original conditions and retains BBV mode") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) for (auto code : {Opcode::PrBeq,Opcode::PrBne}) for (std::int64_t value : {0,1}) for (bool remove : {false,true}) {
    Fixture t(3); auto set = t.add(Opcode::PrSet,{t.reg(0),t.imm(1)}); auto branch = t.add(code,{t.label(2),t.reg(0),t.imm(value)}); auto identity = branch->creation_order();
    t.add(Opcode::Jmp,{t.label(2)},1); t.add(Opcode::Ret,{t.reg(1)},2);
    auto r = t.run(level,{.remove_properties=remove}); CHECK(t.attached(set) != remove);
    bool jump = (code == Opcode::PrBeq) == (value == 0); CHECK(t.attached(branch) == (!remove || jump));
    if (t.attached(branch)) { CHECK(branch->opcode() == (remove ? MachineOpcode(Opcode::Jmp) : MachineOpcode(code))); CHECK(branch->creation_order() == identity); }
    CHECK(r.statistics().properties == (remove ? 2 : 0));
  }
}
TEST_CASE("owning post-allocation DCE retains observable stores stack writes and live producer chains") {
  if (!arm()) return;
  Fixture t; auto dead = t.add(Opcode::Lt,{t.reg(0),t.reg(1),t.reg(2)}); auto def = t.add(Opcode::Add,{t.reg(4),t.reg(5),t.imm(1)});
  auto store = t.add(Opcode::Mov,{t.mem(6),t.reg(4)}); auto fp = t.add(Opcode::Mov,{t.reg(29),t.imm(17)}); auto sp = t.add(Opcode::Mov,{t.reg(31),t.reg(7)});
  auto alloca = t.add(Opcode::Alloca,{t.reg(8),t.imm(16)}); auto start = t.add(Opcode::BStart,{t.reg(10)}); auto end = t.add(Opcode::BEnd,{t.reg(10)}); t.add(Opcode::Ret,{t.reg(3)});
  auto result = t.run(); CHECK_FALSE(t.attached(dead)); CHECK(t.attached(def)); CHECK(t.attached(store)); CHECK(t.attached(fp)); CHECK(t.attached(sp)); CHECK(t.attached(alloca)); CHECK(t.attached(start)); CHECK(t.attached(end)); CHECK(result.statistics().dead_instructions > 0);
}
TEST_CASE("owning post-allocation DCE preserves overflow flags through exactly MOV and EXT32 lookahead") {
  if (!arm()) return;
  for (auto op : {Opcode::Addo,Opcode::Addos,Opcode::Subo,Opcode::Subos,Opcode::Mulo,Opcode::Mulos,Opcode::UMulo,Opcode::UMulos})
    for (auto branch : {Opcode::Bo,Opcode::Bno,Opcode::UBo,Opcode::UBno}) for (bool barrier : {false,true}) {
      Fixture t(3); auto def = t.add(op,{t.reg(3),t.reg(4),t.reg(5)}); t.add(Opcode::Mov,{t.reg(29),t.reg(4)}); t.add(Opcode::Ext32,{t.reg(29),t.reg(5)}); t.add(Opcode::UExt32,{t.reg(31),t.reg(6)});
      if (barrier) t.add(MachinePseudoOpcode::Use); t.add(branch,{t.label(2)}); t.add(Opcode::Jmp,{t.label(2)},1); t.add(Opcode::Ret,{t.reg(7)},2);
      t.run(); CHECK(t.attached(def) != barrier);
    }
  for (bool flags : {false,true}) {
    Fixture t(3); auto first = t.add(Opcode::Addo,{t.reg(1),t.reg(4),t.reg(5)}); auto second = t.add(Opcode::Addo,{t.reg(2),t.reg(4),t.reg(5)}); t.add(Opcode::Mov,{t.reg(29),t.reg(4)});
    t.add(flags ? MachineOpcode(Opcode::Bo) : MachineOpcode(Opcode::Jmp),{t.label(2)}); t.add(Opcode::Jmp,{t.label(2)},1); t.add(Opcode::Ret,{t.reg(7)},2);
    t.run(); CHECK_FALSE(t.attached(first)); CHECK(t.attached(second) == flags);
  }
}
TEST_CASE("owning post-allocation DCE honors call arguments global effects and early clobbers") {
  if (!arm()) return;
  Fixture t; auto global = t.f.target().registers()[19].reg; REQUIRE(t.rw.set_global_registers(std::span(&global,1)));
  auto argument = t.add(Opcode::Mov,{t.reg(0),t.imm(42)}); auto saved = t.add(Opcode::Mov,{t.reg(19),t.imm(42)});
  auto overwritten = t.add(Opcode::Mov,{t.reg(1),t.imm(7)}); auto unused = t.add(Opcode::Mov,{t.reg(2),t.imm(7)}); auto call = t.call(); auto arg = t.f.target().registers()[0].reg; REQUIRE(t.rw.set_call_arguments(*call,std::span(&arg,1))); t.add(Opcode::Ret,{t.reg(1)});
  t.run(); CHECK(t.attached(argument)); CHECK(t.attached(saved)); CHECK_FALSE(t.attached(overwritten)); CHECK_FALSE(t.attached(unused)); CHECK(t.attached(call));
  Fixture clobber; auto early = clobber.add(Opcode::Mov,{clobber.reg(8),clobber.imm(42)}); clobber.add(Opcode::Mod,{clobber.reg(1),clobber.reg(0),clobber.reg(2)}); clobber.add(MachinePseudoOpcode::Use,{clobber.reg(8)}); clobber.add(Opcode::Ret,{clobber.reg(1)}); clobber.run(); CHECK_FALSE(clobber.attached(early));
}
TEST_CASE("owning post-allocation DCE retains singleton anchors without adding redundant labeled nops") {
  if (!arm()) return;
  for (bool labeled : {false,true}) {
    Fixture t; if (labeled) t.label(0); auto def = t.add(Opcode::Lt,{t.reg(0),t.reg(1),t.reg(2)}); auto r = t.run();
    CHECK_FALSE(t.attached(def)); CHECK(r.statistics().no_op_anchors == (labeled ? 0 : 1)); CHECK(t.f.instruction_count() == (labeled ? 0 : 1));
    if (!labeled) CHECK(t.blocks[0]->instructions().begin()->opcode() == MachineOpcode(MachinePseudoOpcode::Use));
  }
}
TEST_CASE("owning post-allocation rejects stale foreign repeated phases and propagates observer errors") {
  if (!arm()) return;
  Fixture t, foreign; t.add(Opcode::Ret,{t.reg(1)}); auto spill = t.spill(); auto before = t.f.dump(); CHECK_FALSE(optimize_allocated_machine(foreign.f,spill)); CHECK(t.f.dump() == before);
  REQUIRE(t.f.create_label(*t.blocks[0])); before = t.f.dump(); CHECK_FALSE(optimize_allocated_machine(t.f,spill)); CHECK(t.f.dump() == before);
  for (auto stage : {MachinePostStage::StripProperties,MachinePostStage::Liveness,MachinePostStage::Combine,MachinePostStage::DeadCode}) {
    Fixture f; f.add(Opcode::Ret,{f.reg(1)}); auto input = f.spill(stage == MachinePostStage::StripProperties ? 0 : 2); bool called = false;
    auto r = optimize_allocated_machine(f.f,input,{.observer=[&](auto current,const auto &) -> Result<void> { if (current == stage) { called = true; return Error{ErrorCode::InvalidArgument,"observer failure"}; } return {}; }});
    CHECK_FALSE(r); REQUIRE(called); CHECK(r.error().code == ErrorCode::InvalidArgument); CHECK(r.error().message.find("native/post") != std::string::npos); CHECK(r.error().message.find(stage_name(stage)) != std::string::npos); CHECK_FALSE(f.f.state().post_allocation_optimized);
    Fixture subsequent; subsequent.add(Opcode::Ret,{subsequent.reg(1)}); subsequent.run();
  }
  Fixture done; done.add(Opcode::Ret,{done.reg(1)}); auto input = done.spill(); auto r = optimize_allocated_machine(done.f,input); REQUIRE(r); before = done.f.dump(); CHECK_FALSE(optimize_allocated_machine(done.f,input)); CHECK(done.f.dump() == before);
}
TEST_CASE("owning post-allocation raw IR coverage requires actual substitutions extensions and dead deletion") {
  if (!arm()) return;
  std::size_t substitutions = 0, extensions = 0, deletions = 0;
  for (auto &text : test::post_allocation_fixtures()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(text); CAPTURE(level); auto source = serialization::parse_text(text); REQUIRE(source);
    auto &function = *(*source)->modules().begin()->functions().begin(); auto target = target::TargetMachine::native(); REQUIRE(target);
    auto machine = prepare_machine_ir(function,*target); REQUIRE(machine); source->reset();
    REQUIRE(machinize(**machine)); REQUIRE(legalize_operands(**machine)); auto graph = MachineGraphAnalysis::build(**machine); REQUIRE(graph);
    REQUIRE(coalesce_registers(**machine,*graph,level)); graph = MachineGraphAnalysis::build(**machine); REQUIRE(graph);
    auto live = MachineLiveness::analyze(**machine,*graph,{.optimization_level=level}); REQUIRE(live);
    auto ranges = MachineLiveRanges::build(**machine,*graph,*live); REQUIRE(ranges); auto allocation = MachineAllocation::assign(**machine,*graph,*live,*ranges); REQUIRE(allocation);
    auto rewrite = rewrite_allocated(**machine,*graph,*live,*ranges,std::move(*allocation)); REQUIRE(rewrite);
    auto spill = split_allocated_spills(**machine,std::move(*rewrite)); REQUIRE(spill);
    auto post = optimize_allocated_machine(**machine,std::move(*spill)); INFO((post ? "" : post.error().message)); REQUIRE(post); REQUIRE(post->current(**machine)); REQUIRE(post->cfg().verify(**machine));
    substitutions += post->statistics().substitutions; extensions += post->statistics().extensions; deletions += post->statistics().dead_instructions;
  }
  CHECK(substitutions > 0); CHECK(extensions > 0); CHECK(deletions > 0);
}
