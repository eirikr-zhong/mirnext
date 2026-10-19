/* Licensed under the MIT License. Owning AArch64 frames and phase boundaries. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <climits>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
namespace {
bool arm() { return target::TargetMachine::native()->architecture() == target::Architecture::AArch64; }
struct Fixture {
  MachineFunction f;
  MachineRewriter rw;
  MachineBasicBlock &entry;
  MachineInstruction *ret;
  Fixture(bool variadic = false, bool returns = true, bool jret = false, bool labels = true)
    : f("owned","frame",Signature{jret ? std::vector<Type>{} : std::vector{Type::i64()},{},variadic},*target::TargetMachine::native()),rw(f),entry(f.create_block()),ret(nullptr) {
    if (labels) REQUIRE(f.create_label(entry,"entry"));
    if (returns) { auto r = rw.append(entry,jret ? Opcode::JRet : Opcode::Ret,{reg(0)}); REQUIRE(r); ret = *r; }
    else { auto l = f.create_label(entry); REQUIRE(l); REQUIRE(rw.append(entry,Opcode::Jmp,{MachineOperand(**l)})); }
    auto state = f.state(); state.machinized = state.operands_legalized = true; REQUIRE(rw.set_state(state));
  }
  MachineOperand reg(unsigned n) { return MachineOperand(f.target().registers()[n].reg); }
  void state(unsigned kind, std::size_t area = 0) {
    auto s = f.state(); s.is_leaf = kind != 1; s.uses_alloca = kind == 2; s.has_block_arguments = kind == 3;
    std::get<MachineFunctionState::AArch64>(s.target).aggregate_save_bytes = area; REQUIRE(rw.set_state(s));
  }
  void use(unsigned n) { REQUIRE(rw.insert_before(*ret,MachinePseudoOpcode::Use,{reg(n)})); }
  void slots(std::size_t count) {
    for (std::size_t n = 0; n < count; ++n) { auto r = f.create_register(Type::i64(),{},false,true); REQUIRE(r); }
  }
  MachinePostResult post(unsigned level = 2) {
    auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
    auto live = MachineLiveness::analyze(f,*graph,{.optimization_level=level}); REQUIRE(live);
    auto ranges = MachineLiveRanges::build(f,*graph,*live); REQUIRE(ranges);
    auto allocation = MachineAllocation::assign(f,*graph,*live,*ranges); REQUIRE(allocation);
    auto rewritten = rewrite_allocated(f,*graph,*live,*ranges,std::move(*allocation)); REQUIRE(rewritten);
    auto spilled = split_allocated_spills(f,std::move(*rewritten)); REQUIRE(spilled);
    auto p = optimize_allocated_machine(f,std::move(*spilled)); INFO((p ? "" : p.error().message)); REQUIRE(p); return std::move(*p);
  }
  MachineFrameResult run(unsigned level = 2, MachineFrameOptions options = {}) {
    auto r = form_frame(f,post(level),options); INFO((r ? "" : r.error().message)); REQUIRE(r);
    REQUIRE(r->current(f)); REQUIRE(r->cfg().verify(f)); REQUIRE(f.verify()); return std::move(*r);
  }
};
unsigned memory_moves(const MachineFrameResult &r, unsigned reg, bool restore, Opcode code = Opcode::Mov) {
  unsigned n = 0;
  for (auto i : r.inserted_instructions()) if (i->opcode() == MachineOpcode(code)) {
    auto p = i->operands()[restore ? 0 : 1].get<target::PhysicalRegister>();
    if (p && p->encoding() == reg && i->operands()[restore ? 1 : 0].get<MachineMemory>()) ++n;
  } return n;
}
}
TEST_CASE("owning frame omits empty leaf and volatile-only frames at every level") {
  if (!arm()) return;
  for (unsigned level = 0; level < 4; ++level) {
    Fixture t; t.use(1); auto p = t.post(level); auto before = t.f.dump(); auto r = form_frame(t.f,std::move(p)); REQUIRE(r);
    CHECK(r->inserted_instructions().empty()); CHECK(r->layout().frame_bytes == 0); CHECK(t.f.dump() == before);
    CHECK(t.f.state().frame_formed); REQUIRE(r->current(t.f)); CHECK(&*t.f.blocks().begin() == &t.entry);
  }
}
TEST_CASE("owning frame saves each used nonvolatile register and full vector width") {
  if (!arm()) return;
  Fixture t;
  for (auto &r : t.f.target().registers()) if (!t.f.target().call_clobbers(r.reg,Type(Type::Kind::Undef))) t.use(r.reg.encoding());
  auto result = t.run(); REQUIRE_FALSE(result.layout().callee_saves.empty()); std::size_t offset = 0;
  for (auto &s : result.layout().callee_saves) {
    CAPTURE(s.reg.encoding()); CHECK(s.offset >= offset); CHECK(s.offset % s.size == 0); offset = s.offset + s.size;
    CHECK(memory_moves(result,s.reg.encoding(),false,s.reg.encoding() < 32 ? Opcode::Mov : Opcode::LDMov) >= 1);
    CHECK(memory_moves(result,s.reg.encoding(),true,s.reg.encoding() < 32 ? Opcode::Mov : Opcode::LDMov) >= 1);
  }
}
TEST_CASE("owning frame entry is before labels and backedges bypass prologue") {
  if (!arm()) return;
  Fixture t(false,false); t.state(1); auto original = &t.entry; auto label = t.entry.labels()[0]; auto r = t.run();
  CHECK(&*t.f.blocks().begin() != original); CHECK(&label->block() == original);
  auto &cfg = r.cfg(); auto b = std::distance(cfg.blocks.begin(),std::find(cfg.blocks.begin(),cfg.blocks.end(),original));
  bool self = false; for (auto edge : cfg.outgoing[b]) self |= cfg.edges[edge].destination == std::size_t(b); CHECK(self);
  CHECK(r.layout().frame_bytes == 16); CHECK(memory_moves(r,30,true) == 0);
}
TEST_CASE("owning frame inserts into an unlabeled entry without a redundant block") {
  if (!arm()) return;
  Fixture t(false,true,false,false); t.state(1); auto identity = t.ret->creation_order(); auto r = t.run();
  CHECK(&*t.f.blocks().begin() == &t.entry); CHECK(t.ret->creation_order() == identity);
  CHECK(r.layout().frame_bytes == 16); CHECK(memory_moves(r,30,false) == 1); CHECK(memory_moves(r,30,true) == 1);
}
TEST_CASE("owning frame retains dynamic block variadic and aggregate layout rules") {
  if (!arm()) return;
  for (unsigned kind = 0; kind < 4; ++kind) {
    Fixture t(kind == 0); t.state(kind == 1 ? 2 : kind == 2 ? 3 : 0,kind == 3 ? 17 : 0); auto r = t.run();
    CHECK(r.layout().frame_bytes >= 16); CHECK(r.layout().frame_bytes % 16 == 0);
    CHECK(r.layout().saves_previous_stack == (kind == 0 || kind == 2));
    const auto variadic_bytes = kind == 0 && t.f.target().abi() == target::ABI::AArch64Linux ? 192u : 0u;
    CHECK(r.layout().variadic_save_bytes == variadic_bytes);
    CHECK(std::get<MachineFunctionState::AArch64>(t.f.state().target).variadic_save_bytes == variadic_bytes);
    CHECK(memory_moves(r,9,false) == (kind == 0 || kind == 2 ? 1 : 0));
    if (kind == 3) { CHECK(r.layout().aggregate_bytes == 32); CHECK(std::get<MachineFunctionState::AArch64>(t.f.state().target).aggregate_save_bytes == 32); }
    if (kind == 0 && t.f.target().abi() == target::ABI::AArch64Linux) {
      for (unsigned n = 0; n < 8; ++n) { CHECK(memory_moves(r,n,false) == 1); CHECK(memory_moves(r,32+n,false,Opcode::LDMov) == 1); }
    }
  }
  for (std::size_t area : {1u,15u,16u,17u,4096u}) {
    Fixture t; t.state(0,area); auto r = t.run(); CHECK(r.layout().aggregate_bytes == (area + 15) / 16 * 16);
    CHECK(std::get<MachineFunctionState::AArch64>(t.f.state().target).aggregate_save_bytes == r.layout().aggregate_bytes);
  }
}
TEST_CASE("owning frame honors JRET link handling and restores both early and last returns") {
  if (!arm()) return;
  for (bool jump : {false,true}) {
    Fixture t(false,true,jump); t.state(1);
    auto r = t.run(); CHECK(r.layout().saves_link != jump);
    CHECK(memory_moves(r,30,false) == (jump ? 0 : 1)); CHECK(memory_moves(r,30,true) == (jump ? 0 : 1));
    CHECK(&t.ret->block() == &t.entry); CHECK(t.ret->opcode() == MachineOpcode(jump ? Opcode::JRet : Opcode::Ret));
  }
  Fixture t; t.state(1); auto &last = t.f.create_block(); REQUIRE(t.rw.append(last,Opcode::Ret,{t.reg(0)}));
  auto r = t.run(); CHECK(std::distance(t.entry.instructions().begin(),t.entry.instructions().end()) > 1);
  CHECK(memory_moves(r,30,true) == 2);
  for (auto block : {&t.entry,&last}) {
    const MachineInstruction *previous = nullptr;
    for (auto &i : block->instructions()) {
      if (i.opcode() == MachineOpcode(Opcode::Ret)) {
        auto inserted = r.inserted_instructions(); CHECK(std::find(inserted.begin(),inserted.end(),previous) != inserted.end());
      }
      previous = &i;
    }
  }
}
TEST_CASE("owning frame preserves real spill-area alignment and large offsets") {
  if (!arm()) return;
  for (unsigned count : {1u,2u,3u,509u,510u,511u,512u,4095u,4096u}) {
    CAPTURE(count); Fixture t; t.slots(count); t.use(19); auto r = t.run(0);
    CHECK(r.layout().spill_bytes == count * 8); CHECK(r.layout().frame_bytes % 16 == 0);
    bool indirect = false, address = false;
    for (auto i : r.inserted_instructions()) {
      if (i->opcode() == MachineOpcode(Opcode::Sub)) indirect |= bool(i->operands()[2].get<target::PhysicalRegister>());
      if (i->opcode() == MachineOpcode(Opcode::Add)) address |= i->operands()[0].get<target::PhysicalRegister>()->encoding() == 10;
    }
    CHECK(indirect == (r.layout().frame_bytes >= 4096)); CHECK(address == (r.layout().callee_saves[0].offset >= 32768));
  }
}
TEST_CASE("owning frame host mode preserves nonvolatile hardware independently of MIR bindings") {
  if (!arm()) return;
  Fixture ordinary, host; auto plain = ordinary.run(); auto wrapper = host.run(2,{.preserve_host_registers=true});
  CHECK(plain.layout().callee_saves.empty());
  for (auto &r : host.f.target().registers()) if (!r.reserved && !host.f.target().call_clobbers(r.reg,Type(Type::Kind::Undef)))
    CHECK(std::find_if(wrapper.layout().callee_saves.begin(),wrapper.layout().callee_saves.end(),[&](auto s) { return s.reg == r.reg; }) != wrapper.layout().callee_saves.end());
}
TEST_CASE("owning frame rejects stale foreign repeated and overflowing inputs before mutation") {
  if (!arm()) return;
  for (unsigned bad = 0; bad < 6; ++bad) {
    Fixture t, other;
    if (bad == 3) t.state(0,SIZE_MAX);
    if (bad == 4) t.state(0,std::size_t(INT32_MAX));
    if (bad == 5) t.state(0,4097); // Rounded immediate is outside the original SUB patterns.
    auto p = t.post();
    if (bad == 0) REQUIRE(t.f.create_label(t.entry));
    if (bad == 1) { auto s = t.f.state(); s.frame_formed = true; REQUIRE(t.rw.set_state(s)); }
    auto &f = bad == 2 ? other.f : t.f; auto before = f.dump(); auto revision = f.revision();
    auto r = form_frame(f,std::move(p)); REQUIRE_FALSE(r); CHECK(f.dump() == before); CHECK(f.revision() == revision);
    CHECK(r.error().message.find("frame") != std::string::npos);
  }
  Fixture next; CHECK(next.run().current(next.f));
}
TEST_CASE("owning frame retains independent source lifetime and allocation metadata") {
  if (!arm()) return;
  std::unique_ptr<MachineFunction> machine;
  {
    auto c = serialization::parse_text("m:module\nf:func i64,i64:x\nlocal i64:r\nalloca r,16\nret x\nendfunc\nendmodule\n"); REQUIRE(c);
    auto p = transforms::optimize(**c); REQUIRE(p); auto target = target::TargetMachine::native(); REQUIRE(target);
    auto r = prepare_machine_ir(*p->context().modules().begin()->functions().begin(),*target); REQUIRE(r); machine = std::move(*r);
  }
  REQUIRE(machinize(*machine)); REQUIRE(legalize_operands(*machine)); auto graph = MachineGraphAnalysis::build(*machine); REQUIRE(graph);
  auto merged = coalesce_registers(*machine,*graph); REQUIRE(merged); graph = MachineGraphAnalysis::build(*machine); REQUIRE(graph);
  auto live = MachineLiveness::analyze(*machine,*graph); REQUIRE(live); auto ranges = MachineLiveRanges::build(*machine,*graph,*live); REQUIRE(ranges);
  auto a = MachineAllocation::assign(*machine,*graph,*live,*ranges); REQUIRE(a); auto rewritten = rewrite_allocated(*machine,*graph,*live,*ranges,std::move(*a)); REQUIRE(rewritten);
  auto spilled = split_allocated_spills(*machine,std::move(*rewritten)); REQUIRE(spilled); auto post = optimize_allocated_machine(*machine,std::move(*spilled)); REQUIRE(post);
  auto result = form_frame(*machine,std::move(*post)); REQUIRE(result); CHECK(result->current(*machine)); CHECK(result->layout().frame_bytes >= 16); REQUIRE(machine->verify());
}
TEST_CASE("owning frames restore every exit before direct whole-function and BBV compilation") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  for (unsigned level = 0; level < 4; ++level) for (auto kind : {MachinePreparationKind::WholeFunction,MachinePreparationKind::BlockVersioning}) {
    CAPTURE(level); CAPTURE(unsigned(kind));
    auto prepared = [&] {
      auto c = serialization::parse_text("m:module\nf:func i64,i64:x\nlocal p:stack,i64:r\nalloca stack,64\nmov i64:(stack),x\nbt yes,x\nmov r,i64:(stack)\nadd r,r,17\nret r\nyes:mov r,i64:(stack)\nadd r,r,41\nret r\nendfunc\nendmodule\n"); REQUIRE(c);
      // Deliberately use valid register IR without simplify's unified return.
      auto p = prepare_machine_function(*(*c)->modules().begin()->functions().begin(),*target,{.optimization_level=level},kind);
      INFO((p ? "" : p.error().message)); REQUIRE(p); return std::move(*p);
    }();
    CHECK(prepared.frame().layout().frame_bytes > 0); unsigned exits = 0;
    for (auto &b : prepared.function().blocks()) {
      const MachineInstruction *previous = nullptr;
      for (auto &i : b.instructions()) {
        if (i.opcode() == MachineOpcode(Opcode::Ret)) {
          ++exits; auto inserted = prepared.frame().inserted_instructions();
          auto found = std::find(inserted.begin(),inserted.end(),previous);
          CHECK(found != inserted.end());
          // Avoid entering malformed code even in no-exceptions assert mode.
          if (found == inserted.end()) return;
        }
        previous = &i;
      }
    }
    CHECK(exits == 2); REQUIRE(prepared.verify());
    if (kind == MachinePreparationKind::WholeFunction) {
      auto artifact = encode_machine(prepared.function(),prepared.frame()); REQUIRE(artifact);
      auto executable = execution::CodeMemory().publish(std::move(*artifact)); REQUIRE(executable);
      auto call = reinterpret_cast<std::int64_t (*)(std::int64_t)>(executable->address());
      for (auto input : {0,1,2,0}) CHECK(call(input) == (input == 0 ? 17 : input+41));
    }
  }
}
TEST_CASE("owning frames restore each special jump-return exit without sharing mutable operand vectors") {
  Fixture t(false,true,true,false); auto &first_exit = t.f.create_block(); auto &other = t.f.create_block();
  auto label = t.f.create_label(other); REQUIRE(label); REQUIRE(t.rw.move_to(*t.ret,first_exit));
  REQUIRE(t.rw.append(t.entry,Opcode::Bt,{MachineOperand(**label),t.reg(1)}));
  auto second = t.rw.append(other,Opcode::JRet,{t.reg(0)}); REQUIRE(second);
  auto state = t.f.state(); state.uses_alloca = state.requires_frame_pointer = true; REQUIRE(t.rw.set_state(state));
  auto framed = t.run(); CHECK(framed.layout().frame_bytes > 0); unsigned exits = 0;
  for (auto &b : t.f.blocks()) {
    const MachineInstruction *previous = nullptr;
    for (auto &i : b.instructions()) {
      if (i.opcode() == MachineOpcode(Opcode::JRet)) {
        ++exits; auto inserted = framed.inserted_instructions();
        CHECK(std::find(inserted.begin(),inserted.end(),previous) != inserted.end());
      }
      previous = &i;
    }
  }
  CHECK(exits == 2);
}
