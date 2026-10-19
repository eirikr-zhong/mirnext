/* Licensed under the MIT License. Target facts and native owning x86 frames. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Detail/X86Frame.hpp"
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include "X86Fixtures.hpp"
#include <map>
#include <climits>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::target; using namespace mirnext::codegen;
namespace xframe = mirnext::codegen::detail;
namespace {
using R = X86Register;
using xframe::X86FrameInput; using xframe::X86FramePlan; using xframe::X86FrameMemory;
unsigned number(R r) { return static_cast<unsigned>(r); }
X86FramePlan plan(const X86FrameInput &input) {
  auto r = xframe::plan_x86_frame(input); INFO((r ? "" : r.error().message)); REQUIRE(r); return std::move(*r);
}
std::vector<R> saved(ABI abi) {
  std::vector<R> out; for (unsigned n = 0; n < 34; ++n) if (x86_callee_saved_register(abi,static_cast<R>(n))) out.push_back(static_cast<R>(n)); return out;
}
// Execute the target sequence as typed register/memory operations. This tests
// both ABIs on arm64 without generating or executing foreign machine code.
struct FrameState {
  std::array<std::array<std::uint64_t,2>,32> registers{};
  std::map<std::uint64_t,std::uint64_t> memory;
  bool no_red_zone;
  FrameState(bool no_red_zone,bool jump) : no_red_zone(no_red_zone) {
    for (unsigned n = 0; n < registers.size(); ++n) registers[n] = {0x100 + n,0x200 + n};
    registers[number(R::SP)][0] = jump ? 0x100000000ull : 0x100000008ull;
    registers[number(R::BP)][0] = 0x200000000ull;
  }
  std::uint64_t &reg(R r) { return registers[number(r)][0]; }
  void run(std::span<const xframe::X86FrameEmission> code) {
    for (auto &i : code) {
      auto value = [&](auto &operand,unsigned word = 0) -> std::uint64_t {
        if (auto r = std::get_if<R>(&operand)) return registers[number(*r)][word];
        if (auto n = std::get_if<std::int64_t>(&operand)) return *n;
        auto &m = std::get<X86FrameMemory>(operand); auto address = reg(m.base) + m.displacement + 8 * word;
        REQUIRE(memory.contains(address)); return memory.at(address);
      };
      if (i.opcode == MachineOpcode(Opcode::Add) || i.opcode == MachineOpcode(Opcode::Sub)) {
        reg(std::get<R>(i.operands[0])) = i.opcode == MachineOpcode(Opcode::Add) ? value(i.operands[1]) + value(i.operands[2]) : value(i.operands[1]) - value(i.operands[2]);
      } else {
        const auto aligned = i.opcode == MachineOpcode(MachinePseudoOpcode::X86Mov128);
        const auto words = aligned || i.opcode == MachineOpcode(MachinePseudoOpcode::X86Mov128Unaligned) ? 2u : 1u;
        if (aligned) for (auto &o : i.operands) if (auto m = std::get_if<X86FrameMemory>(&o)) CHECK((reg(m->base) + m->displacement) % 16 == 0);
        for (unsigned w = 0; w < words; ++w) {
          auto v = value(i.operands[1],w);
          if (auto r = std::get_if<R>(&i.operands[0])) registers[number(*r)][w] = v;
          else { auto &m = std::get<X86FrameMemory>(i.operands[0]); auto address = reg(m.base) + m.displacement + 8 * w;
            if (no_red_zone) CHECK(address >= reg(R::SP)); memory[address] = v;
          }
        }
      }
    }
  }
};
MachinePostResult post(MachineFunction &f,unsigned level = 2) {
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  auto live = MachineLiveness::analyze(f,*graph,{.optimization_level=level}); REQUIRE(live);
  auto ranges = MachineLiveRanges::build(f,*graph,*live); REQUIRE(ranges);
  auto allocation = MachineAllocation::assign(f,*graph,*live,*ranges); REQUIRE(allocation);
  auto rewritten = rewrite_allocated(f,*graph,*live,*ranges,std::move(*allocation)); REQUIRE(rewritten);
  auto spilled = split_allocated_spills(f,std::move(*rewritten)); REQUIRE(spilled);
  auto result = optimize_allocated_machine(f,std::move(*spilled)); REQUIRE(result); return std::move(*result);
}
MachineFrameResult frame(MachineFunction &f,unsigned level = 2,MachineFrameOptions options = {}) {
  auto input = post(f,level); auto result = form_frame(f,std::move(input),options); INFO((result ? "" : result.error().message)); REQUIRE(result);
  auto revision = f.revision(); CHECK_FALSE(form_frame(f,std::move(input),options)); CHECK(f.revision() == revision);
  REQUIRE(result->current(f)); REQUIRE(result->cfg().verify(f)); REQUIRE(f.verify()); return std::move(*result);
}
}
TEST_CASE("x86 frames restore registers stack and variadic homes for both ABIs") {
  for (auto abi : {ABI::X86SysV,ABI::X86Windows}) for (bool fp : {false,true}) for (bool variadic : {false,true}) {
    if (variadic && !fp) continue;
    for (bool red_zone : {false,true}) for (bool jump : {false,true}) {
      if (variadic && jump) continue;
      for (std::size_t bytes : {0u,8u,24u,4096u,unsigned(INT32_MAX - 4096u) / 8 * 8}) for (bool preserve : {false,true}) {
        CAPTURE(abi); CAPTURE(fp); CAPTURE(variadic); CAPTURE(red_zone); CAPTURE(jump); CAPTURE(bytes); CAPTURE(preserve);
        auto regs = preserve ? saved(abi) : std::vector<R>{}; X86FrameInput input{abi};
        input.spill_bytes = bytes; input.saved_registers = regs; input.leaf = false; input.frame_pointer = fp;
        input.uses_alloca = fp; input.variadic = variadic; input.no_red_zone = !red_zone; input.jump_return = jump;
        auto p = plan(input); FrameState state(!red_zone,jump); const auto original = state.registers;
        state.run(p.prologue);
        CHECK(state.reg(R::SP) == original[number(R::SP)][0] - p.frame_bytes); CHECK(state.reg(R::SP) % 16 == 0);
        CHECK(p.variadic_save_bytes == (variadic && abi == ABI::X86SysV ? 176 : 0));
        if (fp) CHECK(state.reg(R::BP) == original[number(R::SP)][0] - 8);
        const auto base = state.reg(fp ? R::BP : R::SP);
        for (auto &s : p.saves) {
          CHECK(state.memory.at(base + s.offset) == original[number(s.reg)][0]);
          if (s.size == 16) CHECK(state.memory.at(base + s.offset + 8) == original[number(s.reg)][1]);
          state.registers[number(s.reg)] = {0xdead,0xbeef};
        }
        if (variadic && abi == ABI::X86Windows) for (unsigned n = 0; n < 4; ++n)
          CHECK(state.memory.at(original[number(R::SP)][0] + 8 + 8*n) == original[number(*x86_integer_argument_register(abi,n))][0]);
        if (variadic && abi == ABI::X86SysV) {
          auto at = state.reg(R::SP) + p.block_bytes;
          for (unsigned n = 0; n < 6; ++n) CHECK(state.memory.at(at + n*8) == original[number(*x86_integer_argument_register(abi,n))][0]);
          for (unsigned n = 0; n < 8; ++n) CHECK(state.memory.at(at + 48 + n*16) == original[number(R::XMM0) + n][0]);
        }
        if (fp) state.reg(R::SP) -= 128; // Dynamic allocation must be discarded by the epilogue.
        state.run(p.epilogue);
        CHECK(state.reg(R::SP) == original[number(R::SP)][0]); CHECK(state.reg(R::BP) == original[number(R::BP)][0]);
        for (auto r : regs) {
          CHECK(state.registers[number(r)][0] == original[number(r)][0]);
          if (number(r) >= 16) CHECK(state.registers[number(r)][1] == original[number(r)][1]);
        }
      }
    }
  }
}
TEST_CASE("x86 frames retain omission saved-register order and no-return boundaries") {
  for (auto abi : {ABI::X86SysV,ABI::X86Windows}) {
    X86FrameInput input{abi}; input.frame_pointer = true;
    auto leaf = plan(input); CHECK(leaf.prologue.empty()); CHECK(leaf.epilogue.empty()); CHECK(leaf.frame_bytes == 0);
    auto regs = saved(abi); std::reverse(regs.begin(),regs.end()); input.saved_registers = regs;
    input.frame_pointer = false; input.spill_bytes = 8; input.has_return = false;
    auto p = plan(input); CHECK_FALSE(p.prologue.empty()); CHECK(p.epilogue.empty()); CHECK(p.frame_bytes % 16 == 8);
    std::int64_t offset = 16; unsigned last_vector = 0, last_integer = 0; bool integer = false;
    for (auto &s : p.saves) {
      CHECK(s.offset == offset); offset += s.size;
      if (number(s.reg) >= 16) { CHECK_FALSE(integer); CHECK(number(s.reg) > last_vector); last_vector = number(s.reg); CHECK(s.size == 16); }
      else { integer = true; CHECK(number(s.reg) > last_integer); last_integer = number(s.reg); CHECK(s.size == 8); }
    }
    for (unsigned reason = 0; reason < 3; ++reason) {
      X86FrameInput force{abi}; force.leaf = reason != 0; force.uses_alloca = reason == 1; force.block_arguments = reason == 2;
      force.frame_pointer = reason != 0;
      CHECK(plan(force).frame_bytes == 8);
    }
  }
}
TEST_CASE("x86 frames diagnose invalid facts without allocating overflowing areas") {
  CHECK_FALSE(xframe::plan_x86_frame({ABI::AArch64Linux}));
  for (auto abi : {ABI::X86SysV,ABI::X86Windows}) {
    X86FrameInput input{abi}; input.variadic = true; CHECK_FALSE(xframe::plan_x86_frame(input)); input.variadic = false;
    input.uses_alloca = true; CHECK_FALSE(xframe::plan_x86_frame(input)); input.uses_alloca = false;
    input.block_arguments = true; CHECK_FALSE(xframe::plan_x86_frame(input)); input.block_arguments = false;
    for (auto size : {std::size_t{1},std::size_t(INT32_MAX),SIZE_MAX}) { input.spill_bytes = size; CHECK_FALSE(xframe::plan_x86_frame(input)); }
    input.spill_bytes = 0;
    for (auto bad : {R::AX,R::BP,R::SP,R::ST0,static_cast<R>(999)}) { input.saved_registers = std::span(&bad,1); CHECK_FALSE(xframe::plan_x86_frame(input)); }
    const R duplicates[]{R::BX,R::BX}; input.saved_registers = duplicates; CHECK_FALSE(xframe::plan_x86_frame(input));
  }
}
TEST_CASE("x86 frame layout retains exact scalar vector and variadic save displacements") {
  const R sysv[]{R::R15,R::BX}; X86FrameInput input{ABI::X86SysV}; input.saved_registers = sysv; input.spill_bytes = 24;
  auto frameless = plan(input); CHECK(frameless.block_bytes == 48); CHECK(frameless.frame_bytes == 56);
  REQUIRE(frameless.saves.size() == 2); CHECK(frameless.saves[0].reg == R::BX); CHECK(frameless.saves[0].offset == 32); CHECK(frameless.saves[1].offset == 40);
  input.frame_pointer = true; auto fp = plan(input); CHECK(fp.block_bytes == 48); CHECK(fp.frame_bytes == 56);
  CHECK(fp.saves[0].offset == -48); CHECK(fp.saves[1].offset == -40);
  input.variadic = true; auto va = plan(input); CHECK(va.frame_bytes == 232); CHECK(va.saves[0].offset == -224); CHECK(va.saves[1].offset == -216);
  REQUIRE(va.prologue.size() == 19);
  for (unsigned n = 0; n < 14; ++n) {
    auto &i = va.prologue[3 + n]; auto &m = std::get<X86FrameMemory>(i.operands[0]);
    CHECK(m.base == R::SP); CHECK(m.displacement == (n < 6 ? 48 + n*8 : 96 + (n-6)*16));
    CHECK(i.opcode == MachineOpcode(n < 6 ? Opcode::Mov : Opcode::DMov));
  }
  const R windows[]{R::BX,R::XMM15,R::SI,R::XMM6}; input.abi = ABI::X86Windows; input.saved_registers = windows;
  auto win = plan(input); CHECK(win.block_bytes == 80); CHECK(win.frame_bytes == 88); CHECK(win.variadic_save_bytes == 0);
  REQUIRE(win.saves.size() == 4);
  CHECK(win.saves[0].reg == R::XMM6); CHECK(win.saves[0].offset == -80); CHECK(win.saves[0].size == 16);
  CHECK(win.saves[1].reg == R::XMM15); CHECK(win.saves[1].offset == -64);
  CHECK(win.saves[2].reg == R::BX); CHECK(win.saves[2].offset == -48);
  CHECK(win.saves[3].reg == R::SI); CHECK(win.saves[3].offset == -40);
  CHECK(win.prologue[7].opcode == MachineOpcode(MachinePseudoOpcode::X86Mov128));
}
TEST_CASE("x86 no-red-zone frames correct the C uninitialized BP and unallocated JRET save defects") {
  X86FrameInput input{ABI::X86Windows}; input.frame_pointer = input.variadic = input.no_red_zone = true;
  auto win = plan(input); FrameState windows(true,false); const auto old_bp = windows.reg(R::BP), old_sp = windows.reg(R::SP);
  windows.run(win.prologue);
  for (unsigned n = 0; n < 4; ++n) {
    auto &home = std::get<X86FrameMemory>(win.prologue[1 + n].operands[0]); CHECK(home.base == R::R10);
    CHECK_FALSE(windows.memory.contains(old_bp + 16 + 8*n)); CHECK(windows.memory.contains(old_sp + 8 + 8*n));
  }
  input.abi = ABI::X86SysV; input.variadic = false; input.jump_return = input.uses_alloca = true;
  auto jump = plan(input); CHECK(jump.block_bytes == 0); CHECK(jump.frame_bytes == 16);
  FrameState state(true,true); auto original_sp = state.reg(R::SP), original_bp = state.reg(R::BP);
  state.run(jump.prologue); CHECK(state.memory.at(original_sp - 8) == original_bp);
  state.run(jump.epilogue); CHECK(state.reg(R::SP) == original_sp); CHECK(state.reg(R::BP) == original_bp);
}
TEST_CASE("native x86 frame insertion preserves labels identities and last return") {
  auto target = TargetMachine::native(); REQUIRE(target); if (target->architecture() != Architecture::X86_64) return;
  for (bool fp : {false,true}) for (bool returns : {false,true}) for (bool jump : {false,true}) for (bool no_red_zone : {false,true}) for (unsigned level = 0; level < 4; ++level) {
    MachineFunction f("m","f",{jump ? std::vector<Type>{} : std::vector{Type::i64()},{}},*target); MachineRewriter rw(f);
    auto &entry = f.create_block(); auto label = f.create_label(entry,"entry"); REQUIRE(label);
    auto reg = [&](R r) { return MachineOperand(target->registers()[number(r)].reg); };
    REQUIRE(rw.append(entry,MachinePseudoOpcode::Use,{reg(R::BX)}));
    MachineInstruction *last = nullptr;
    if (returns) { auto r = rw.append(entry,jump ? Opcode::JRet : Opcode::Ret,{reg(R::AX)}); REQUIRE(r); last = *r; }
    else REQUIRE(rw.append(entry,Opcode::Jmp,{MachineOperand(**label)}));
    auto state = f.state(); state.machinized = state.operands_legalized = true; state.is_leaf = false; state.requires_frame_pointer = fp;
    REQUIRE(rw.set_state(state)); auto result = frame(f,level,{.no_red_zone=no_red_zone});
    CHECK(&*f.blocks().begin() != &entry); CHECK(&(*label)->block() == &entry); CHECK(result.layout().saves_frame_pointer == fp);
    CHECK(result.layout().frame_bytes == (jump && returns ? (no_red_zone && fp ? 32 : 16) : 24)); REQUIRE(result.layout().callee_saves.size() == 1);
    CHECK(result.layout().callee_saves[0].offset == (fp ? -16 : 0));
    unsigned epilogue = 0;
    for (auto i : result.inserted_instructions()) {
      CHECK((&i->block() == &entry || &i->block() == &*f.blocks().begin()));
      epilogue += &i->block() == &entry;
    }
    CHECK(epilogue == (returns ? (fp ? (no_red_zone ? 4 : 3) : 2) : 0));
    if (last) CHECK(&*std::prev(entry.instructions().end()) == last);
  }
}
TEST_CASE("native x86 frames cover the public instruction and ABI fixture pipelines") {
  auto target = TargetMachine::native(); REQUIRE(target); if (target->architecture() != Architecture::X86_64) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto run = [&](const Context &source) {
      transforms::OptimizationOptions options; options.level = static_cast<transforms::OptimizationLevel>(level);
      auto p = transforms::optimize(source,options); REQUIRE(p);
      for (auto &module : p->context().modules()) for (auto &f : module.functions()) {
        auto m = prepare_machine_ir(f,*target); REQUIRE(m); REQUIRE(machinize(**m)); REQUIRE(legalize_operands(**m));
        auto r = frame(**m,level,{.preserve_host_registers=true}); CHECK(r.layout().frame_bytes % 16 <= 8);
      }
    };
    for (auto &d : instruction_descriptors()) { CAPTURE(d.name); auto source = test::public_instruction_example(d); run(*source); }
    for (auto &text : test::x86_lowering_fixtures(target->abi() == ABI::X86Windows)) { CAPTURE(text); auto source = serialization::parse_text(text); REQUIRE(source); run(**source); }
  }
}
TEST_CASE("native x86 frame errors leave stable nodes and revisions untouched") {
  auto target = TargetMachine::native(); REQUIRE(target); if (target->architecture() != Architecture::X86_64) return;
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto make = [&](bool variadic) {
      auto f = std::make_unique<MachineFunction>("m","f",Signature{{Type::i64()},{},variadic},*target); auto &b = f->create_block(); MachineRewriter rw(*f);
      REQUIRE(rw.append(b,Opcode::Ret,{MachineOperand(target->registers()[0].reg)}));
      auto state = f->state(); state.machinized = state.operands_legalized = true; REQUIRE(rw.set_state(state)); return f;
    };
    auto f = make(bad == 3), other = make(false); auto input = post(*f); MachineRewriter rw(*f);
    if (bad == 0) REQUIRE(f->create_label(*f->blocks().begin()));
    if (bad == 1) { auto state = f->state(); state.frame_formed = true; REQUIRE(rw.set_state(state)); }
    auto &selected = bad == 2 ? *other : *f; auto before = selected.dump(); auto revision = selected.revision();
    auto result = form_frame(selected,std::move(input)); REQUIRE_FALSE(result);
    CHECK(selected.dump() == before); CHECK(selected.revision() == revision); CHECK(result.error().message.find("frame") != std::string::npos);
  }
}
