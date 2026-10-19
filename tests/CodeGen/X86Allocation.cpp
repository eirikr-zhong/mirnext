/* Licensed under the MIT License. Native x86 allocation consumers; no C runtime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include "../IR/PublicInstructions.hpp"
#include "X86Fixtures.hpp"
#include "PostAllocationFixtures.hpp"
#include <algorithm>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
namespace {
bool x86() { return target::TargetMachine::native()->architecture() == target::Architecture::X86_64; }
struct Fixture {
  MachineFunction f{"native","x86-allocation",Signature{},*target::TargetMachine::native()};
  MachineRewriter rw{f}; MachineBasicBlock &b = f.create_block();
  Fixture(bool fp = false) {
    auto state = f.state(); state.machinized = state.operands_legalized = true; state.requires_frame_pointer = fp;
    REQUIRE(rw.set_state(state));
  }
  MachineOperand reg(Type type) { auto r = f.create_register(type); REQUIRE(r); return MachineOperand(**r); }
  MachineOperand physical(unsigned n) { return MachineOperand(f.target().registers()[n].reg); }
  MachineInstruction *add(MachineOpcode code, std::vector<MachineOperand> operands = {}) {
    auto i = rw.append(b,code,std::move(operands)); INFO((i ? "" : i.error().message)); REQUIRE(i); return *i;
  }
  void force_stack() {
    std::vector<target::PhysicalRegister> all; for (auto &p : f.target().registers()) if (!p.reserved) all.push_back(p.reg);
    REQUIRE(rw.set_global_registers(all));
  }
};
MachineRewriteResult rewrite(MachineFunction &f,unsigned level) {
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  auto live = MachineLiveness::analyze(f,*graph,{.optimization_level=level}); REQUIRE(live);
  auto ranges = MachineLiveRanges::build(f,*graph,*live); REQUIRE(ranges);
  auto allocation = MachineAllocation::assign(f,*graph,*live,*ranges); REQUIRE(allocation);
  auto result = rewrite_allocated(f,*graph,*live,*ranges,std::move(*allocation)); INFO((result ? "" : result.error().message)); REQUIRE(result);
  REQUIRE(result->current(f)); REQUIRE(f.verify()); return std::move(*result);
}
void finish(MachineFunction &f,unsigned level) {
  auto allocated = rewrite(f,level);
  auto spill = split_allocated_spills(f,std::move(allocated)); INFO((spill ? "" : spill.error().message)); REQUIRE(spill);
  auto post = optimize_allocated_machine(f,std::move(*spill)); INFO((post ? "" : post.error().message)); REQUIRE(post);
  REQUIRE(post->current(f)); REQUIRE(post->cfg().verify(f)); REQUIRE(f.verify());
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
    auto pattern = has_instruction_pattern(i); REQUIRE(pattern); CHECK(*pattern);
    for (auto &o : i.operands()) {
      CHECK_FALSE(o.get<const MachineVirtualRegister *>());
      if (auto m = o.get<MachineMemory>()) {
        if (m->base) CHECK(std::holds_alternative<target::PhysicalRegister>(*m->base));
        if (m->index) CHECK(std::holds_alternative<target::PhysicalRegister>(*m->index));
      }
    }
  }
}
void pipeline(const Context &source,unsigned level,bool optimize) {
  transforms::OptimizationOptions options; options.level = static_cast<transforms::OptimizationLevel>(level);
  auto optimized = transforms::optimize(source,options); REQUIRE(optimized);
  auto &context = optimize ? optimized->context() : source;
  for (auto &module : context.modules()) for (auto &function : module.functions()) {
    auto m = prepare_machine_ir(function,*target::TargetMachine::native()); REQUIRE(m);
    REQUIRE(machinize(**m)); REQUIRE(legalize_operands(**m));
    auto graph = MachineGraphAnalysis::build(**m); REQUIRE(graph); REQUIRE(coalesce_registers(**m,*graph,level));
    finish(**m,level);
  }
}
}
TEST_CASE("owning copied-block dependencies retain analysis identity without virtual machine addresses") {
  Fixture t; auto value = t.reg(Type::i64()); auto dependency = *value.get<const MachineVirtualRegister *>();
  auto proto = t.f.intern_symbol({"native","block-proto",MachineSymbol::Kind::Prototype,Signature{{},{Type::block(24)}},false,{}}); REQUIRE(proto);
  auto ext = t.f.intern_symbol({"native","ext",MachineSymbol::Kind::Import,{},false,{}}); REQUIRE(ext);
  auto block = MachineOperand(MachineMemory{Type::block(24),MachineRegister(dependency),{},24,1,{},{}});
  auto call = t.add(Opcode::Call,{MachineOperand(MachineSymbolReference{*proto}),MachineOperand(MachineSymbolReference{*ext}),block});
  auto operands = std::vector<MachineOperand>(call->operands().begin(),call->operands().end());
  operands[2] = MachineOperand(MachineMemory{Type(Type::Kind::Undef),MachineRegister(dependency),{},24,1,{},{}});
  REQUIRE(t.rw.lower_call(*call,Opcode::Call,std::move(operands)));
  CallArgumentDependency d{2,dependency}; CHECK_FALSE(t.rw.set_call_dependencies(*call,std::span(&d,1))); t.add(Opcode::Ret);
  auto before = t.f.dump(); auto revision = t.f.revision();
  for (auto bad : {CallArgumentDependency{0,dependency},CallArgumentDependency{99,dependency},CallArgumentDependency{2,nullptr}}) {
    CHECK_FALSE(t.rw.set_call_dependencies(*call,std::span(&bad,1))); CHECK(t.f.dump() == before); CHECK(t.f.revision() == revision);
  }
  Fixture foreign; auto other = foreign.reg(Type::i64()); CallArgumentDependency bad{2,*other.get<const MachineVirtualRegister *>()};
  CHECK_FALSE(t.rw.set_call_dependencies(*call,std::span(&bad,1))); CHECK(t.f.dump() == before);
  auto rewritten = rewrite(t.f,2); REQUIRE(call->call_dependencies().size() == 1); CHECK(call->call_dependencies()[0] == d);
  CHECK_FALSE(call->operands()[2].get<MachineMemory>()->base); REQUIRE(t.f.verify());
  before = t.f.dump(); revision = t.f.revision();
  std::array duplicate{d,d}; CHECK_FALSE(t.rw.set_call_dependencies(*call,duplicate)); CHECK_FALSE(t.rw.set_call_dependencies(*call,std::span(&bad,1)));
  CHECK(t.f.dump() == before); CHECK(t.f.revision() == revision);
  auto split = split_allocated_spills(t.f,std::move(rewritten)); REQUIRE(split); bool checked = false;
  auto post = optimize_allocated_machine(t.f,std::move(*split),{.observer=[&](auto stage,const auto &view) -> Result<void> {
    if (stage == MachinePostStage::Liveness) {
      auto dead = view.deaths(*call); CHECK(std::find(dead.begin(),dead.end(),MachineRegister(dependency)) != dead.end()); checked = true;
    } return {};
  }}); INFO((post ? "" : post.error().message)); REQUIRE(post); CHECK(checked); REQUIRE(t.f.verify());
  CHECK(t.f.dump().find("stack-argument 2 depends-on") != std::string::npos);
}
TEST_CASE("native x86 allocation rewrites tied memory destinations with ABI reload registers") {
  if (!x86()) return;
  for (bool fp : {false,true}) for (unsigned level = 0; level < 4; ++level) {
    Fixture t(fp); t.force_stack(); auto a = t.reg(Type::i64()), c = t.reg(Type::i64());
    auto first = t.add(Opcode::Mov,{a,MachineOperand(std::int64_t(42))});
    t.add(Opcode::Mov,{c,a}); auto sum = t.add(Opcode::Add,{c,c,a}); t.add(Opcode::Ret);
    auto result = rewrite(t.f,level); CHECK(result.allocation().stack_bytes() == 16);
    REQUIRE(first->operands()[0].get<MachineMemory>()); REQUIRE(sum->operands()[0].get<MachineMemory>());
    CHECK(sum->operands()[0] == sum->operands()[1]); REQUIRE(sum->operands()[2].get<target::PhysicalRegister>());
    CHECK(sum->operands()[2].get<target::PhysicalRegister>()->encoding() == 10);
    CHECK(first->operands()[0].get<MachineMemory>()->base == MachineRegister(t.f.target().registers()[fp ? 5 : 4].reg));
    CHECK((first->operands()[0].get<MachineMemory>()->displacement < 0) == fp);
    REQUIRE(has_instruction_pattern(*sum)); CHECK(*has_instruction_pattern(*sum));
  }
}
TEST_CASE("owning allocation preserves bound registers across repeated calls with target stack locations") {
  for (unsigned level : {2,3}) for (bool fp : {false,true}) {
    Fixture t(fp); auto physical = t.f.target().registers()[2].reg;
    auto r = t.f.create_register(Type::i64(),"kept",false,false,physical); REQUIRE(r);
    auto proto = t.f.intern_symbol({"native","empty-proto",MachineSymbol::Kind::Prototype,Signature{},false,{}}); REQUIRE(proto);
    auto ext = t.f.intern_symbol({"native","ext",MachineSymbol::Kind::Import,{},false,{}}); REQUIRE(ext);
    t.add(Opcode::Mov,{MachineOperand(**r),MachineOperand(std::int64_t(42))});
    std::vector<MachineInstruction *> calls;
    for (unsigned n = 0; n < 2; ++n) {
      auto call = t.add(Opcode::Call,{MachineOperand(MachineSymbolReference{*proto}),MachineOperand(MachineSymbolReference{*ext})});
      REQUIRE(t.rw.lower_call(*call,call->opcode(),std::vector<MachineOperand>(call->operands().begin(),call->operands().end())));
      REQUIRE(t.rw.set_call_arguments(*call,std::span(&physical,1))); calls.push_back(call);
      t.add(MachinePseudoOpcode::Use,{MachineOperand(**r)});
    }
    t.add(Opcode::Ret); auto rewritten = rewrite(t.f,level); auto slot = rewritten.spill_slot(**r); REQUIRE(slot); REQUIRE(*slot);
    auto displacement = static_cast<std::int64_t>((**slot).offset + 16);
    unsigned base = 29;
    if (x86()) {
      auto value = target::x86_spill_displacement(t.f.target().abi(),fp,false,Type::i64(),(**slot).offset); REQUIRE(value);
      displacement = *value; base = fp ? 5 : 4;
    }
    for (auto call : calls) {
      auto next = t.rw.next(*call); REQUIRE(next); REQUIRE(*next); auto memory = (*next)->operands()[1].get<MachineMemory>(); REQUIRE(memory);
      CHECK(memory->displacement == displacement); CHECK(memory->base == MachineRegister(t.f.target().registers()[base].reg));
    }
    auto split = split_allocated_spills(t.f,std::move(rewritten)); REQUIRE(split);
    auto post = optimize_allocated_machine(t.f,std::move(*split)); REQUIRE(post); REQUIRE(t.f.verify());
  }
}
TEST_CASE("native x86 floating reloads preserve tied operands and separate input temporaries") {
  if (!x86()) return;
  for (bool single : {false,true}) for (unsigned level = 0; level < 4; ++level) {
    Fixture t(true); t.force_stack(); auto type = Type(single ? Type::Kind::F32 : Type::Kind::F64);
    auto a = t.reg(type), c = t.reg(type); auto mov = single ? Opcode::FMov : Opcode::DMov;
    t.add(mov,{a,t.physical(16)}); t.add(mov,{c,t.physical(17)});
    auto sum = t.add(single ? Opcode::FAdd : Opcode::DAdd,{c,c,a});
    t.add(mov,{MachineOperand(MachineMemory{type,MachineRegister(t.f.target().registers()[3].reg),{},0,1,{},{}}),c}); t.add(Opcode::Ret);
    rewrite(t.f,level); auto first = target::x86_reload_register(t.f.target().abi(),type,true), second = target::x86_reload_register(t.f.target().abi(),type,false); REQUIRE(first); REQUIRE(second);
    CHECK(sum->operands()[0] == sum->operands()[1]);
    CHECK(sum->operands()[0].get<target::PhysicalRegister>()->encoding() == static_cast<unsigned>(*first));
    CHECK(sum->operands()[2].get<target::PhysicalRegister>()->encoding() == static_cast<unsigned>(*second));
    CHECK(*has_instruction_pattern(*sum));
  }
}
TEST_CASE("native x86 x87 values stay in two-slot memory without reload registers") {
  if (!x86() || sizeof(long double) == sizeof(double)) return;
  for (bool fp : {false,true}) for (unsigned level = 0; level < 4; ++level) {
    Fixture t(fp); auto ld = Type(Type::Kind::LongDouble); auto a = t.reg(ld), b = t.reg(ld), c = t.reg(ld);
    t.add(Opcode::LDMov,{a,t.physical(32)}); t.add(Opcode::LDMov,{b,t.physical(33)});
    auto sum = t.add(Opcode::LDAdd,{c,a,b}); t.add(Opcode::LDMov,{t.physical(32),c}); t.add(Opcode::Ret);
    auto rewritten = rewrite(t.f,level); CHECK(rewritten.allocation().stack_bytes() >= 32);
    for (auto &o : sum->operands()) {
      auto mem = o.get<MachineMemory>(); REQUIRE(mem); CHECK(mem->type == ld);
      CHECK(mem->base == MachineRegister(t.f.target().registers()[fp ? 5 : 4].reg));
    }
    CHECK(*has_instruction_pattern(*sum));
    auto split = split_allocated_spills(t.f,std::move(rewritten)); REQUIRE(split);
    auto post = optimize_allocated_machine(t.f,std::move(*split)); REQUIRE(post); REQUIRE(t.f.verify());
  }
}
TEST_CASE("native x86 allocation consumers cover public opcodes ABI boundaries and raw combine rules") {
  if (!x86()) return; auto target = target::TargetMachine::native(); REQUIRE(target);
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level);
    for (auto &d : instruction_descriptors()) { CAPTURE(d.name); auto c = test::public_instruction_example(d); pipeline(*c,level,true); }
    for (auto &text : test::x86_lowering_fixtures(target->abi() == target::ABI::X86Windows)) {
      CAPTURE(text); auto c = serialization::parse_text(text); REQUIRE(c); pipeline(**c,level,true);
    }
    for (auto &text : test::post_allocation_fixtures()) { CAPTURE(text); auto c = serialization::parse_text(text); REQUIRE(c); pipeline(**c,level,false); }
  }
}
