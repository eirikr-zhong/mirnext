/* Licensed under the MIT License. Direct machine analyses without a runtime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
target::TargetMachine native_target() { auto t = target::TargetMachine::native(); REQUIRE(t); return std::move(*t); }
MachineVirtualRegister *reg(MachineFunction &f, Type t = Type::i64()) { auto r = f.create_register(t); REQUIRE(r); return *r; }
MachineInstruction *append(MachineRewriter &r, MachineBasicBlock &b, MachineOpcode code, std::vector<MachineOperand> operands = {}) {
  auto i = r.append(b, code, std::move(operands)); INFO((i ? "" : i.error().message)); REQUIRE(i); return *i;
}
bool has(const MachineLiveness &a, const SparseBitSet &set, MachineRegister r) { auto v = a.contains(set, r); REQUIRE(v); return *v; }
}
TEST_CASE("machine loop numbering and liveness retain stable owners and reject stale graphs") {
  auto source = serialization::parse_text("m:module\nf:func i64\nlocal i64:x\nmov x,42\nret x\nendfunc\nendmodule\n"); REQUIRE(source);
  auto p = transforms::optimize(**source); REQUIRE(p);
  auto f = prepare_machine_ir(*p->find_function("m", "f"), native_target()); REQUIRE(f); auto &function = **f;
  auto graph = MachineGraphAnalysis::build(function); REQUIRE(graph); auto dump = function.dump();
  auto live = MachineLiveness::analyze(function, *graph); REQUIRE(live); CHECK(live->current(function)); CHECK(graph->current(function)); CHECK(function.dump() == dump);
  MachineFunction other("m", "other", {}, native_target()); other.create_block();
  CHECK_FALSE(MachineLiveness::analyze(other, *graph)); CHECK_FALSE(MachineLiveness::analyze(function, *graph, {.optimization_level = 4}));
  reg(function); CHECK_FALSE(graph->current(function)); CHECK_FALSE(live->current(function)); CHECK_FALSE(MachineLiveness::analyze(function, *graph));
  CHECK_FALSE(live->frequency(live->registers()[0]));
}
TEST_CASE("machine register effects retain duplicate address uses and private instructions") {
  MachineFunction f("m", "effects", {}, native_target()); auto &b = f.create_block(); MachineRewriter r(f); auto x = reg(f), y = reg(f);
  MachineMemory memory{Type::i64(), x, x, 8, 1};
  auto store = append(r, b, Opcode::Mov, {MachineOperand(memory), MachineOperand(*y)});
  auto load = append(r, b, Opcode::Mov, {MachineOperand(*y), MachineOperand(memory)});
  auto label = f.create_label(b); REQUIRE(label);
  auto laddr = append(r, b, Opcode::LAddr, {MachineOperand(*y), MachineOperand(**label)});
  auto use = append(r, b, MachinePseudoOpcode::Use, {MachineOperand(*x), MachineOperand(*x)});
  auto a = machine_register_effects(f, *store); REQUIRE(a); CHECK(a->definitions.empty()); CHECK(a->uses == std::vector<MachineRegister>{x, x, y});
  a = machine_register_effects(f, *load); REQUIRE(a); CHECK(a->definitions == std::vector<MachineRegister>{y}); CHECK(a->uses == std::vector<MachineRegister>{x, x});
  a = machine_register_effects(f, *laddr); REQUIRE(a); CHECK(a->definitions.empty()); CHECK(a->uses == std::vector<MachineRegister>{y});
  a = machine_register_effects(f, *use); REQUIRE(a); CHECK(a->uses == std::vector<MachineRegister>{x, x});
  MachineFunction other("m", "other", {}, native_target()); CHECK_FALSE(machine_register_effects(other, *use));
  if (f.target().architecture() == target::Architecture::X86_64) {
    auto vector = f.target().find_register("xmm0"); REQUIRE(vector);
    auto move = append(r, b, MachinePseudoOpcode::X86Mov128, {MachineOperand(*vector), MachineOperand(MachineMemory{Type(Type::Kind::F64), x})});
    a = machine_register_effects(f, *move); REQUIRE(a); CHECK(a->definitions == std::vector<MachineRegister>{*vector}); CHECK(a->uses == std::vector<MachineRegister>{x});
  }
}
TEST_CASE("machine liveness solves loops and counts operand occurrences with original loop weights") {
  for (unsigned level = 0; level < 4; ++level) {
    MachineFunction f("m", "loop", {{Type::i64()}, {}}, native_target()); auto &entry = f.create_block(), &loop = f.create_block(), &exit = f.create_block();
    auto label = f.create_label(loop); REQUIRE(label); auto x = reg(f); MachineRewriter r(f);
    append(r, entry, Opcode::Mov, {MachineOperand(*x), MachineOperand(std::int64_t(0))});
    append(r, loop, Opcode::Add, {MachineOperand(*x), MachineOperand(*x), MachineOperand(std::int64_t(1))});
    append(r, loop, Opcode::Bt, {MachineOperand(**label), MachineOperand(*x)});
    append(r, exit, Opcode::Ret, {MachineOperand(*x)});
    auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph); CHECK(graph->has_loops()); CHECK(graph->loop_depth()[3] == 1);
    auto live = MachineLiveness::analyze(f, *graph, {.optimization_level = level}); REQUIRE(live);
    CHECK(*live->frequency(x) == (level ? 17 : 5));
    CHECK(has(*live, live->blocks()[3].in, x)); CHECK(has(*live, live->blocks()[3].out, x));
    CHECK_FALSE(has(*live, live->blocks()[2].in, x)); CHECK(has(*live, live->blocks()[2].kill, x));
    CHECK(live->blocks()[3].max_integer_pressure == (level ? 2 : 0));
    auto no_freq = MachineLiveness::analyze(f, *graph, {.optimization_level = level, .calculate_frequencies = false}); REQUIRE(no_freq); CHECK(*no_freq->frequency(x) == 0);
  }
}
TEST_CASE("machine call clobbers physical arguments globals and dead uses preserve separate identities") {
  MachineFunction f("m", "call", {}, native_target()); auto &b = f.create_block(); MachineRewriter r(f);
  auto prototype = f.intern_symbol({"m", "prototype", MachineSymbol::Kind::Prototype, Signature{}}); REQUIRE(prototype);
  auto callee = f.intern_symbol({"m", "callee", MachineSymbol::Kind::Import, Signature{}}); REQUIRE(callee);
  auto argument = f.target().registers()[0].reg;
  auto saved = f.target().architecture() == target::Architecture::AArch64 ? f.target().registers()[19].reg : f.target().registers()[3].reg;
  auto call = append(r, b, Opcode::Call, {MachineOperand(MachineSymbolReference{*prototype}), MachineOperand(MachineSymbolReference{*callee})});
  REQUIRE(r.set_call_arguments(*call, {&argument, 1}));
  auto use = append(r, b, MachinePseudoOpcode::Use, {MachineOperand(argument), MachineOperand(saved)}); append(r, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  auto live = MachineLiveness::analyze(f, *graph); REQUIRE(live);
  CHECK(has(*live, live->blocks()[2].kill, argument)); CHECK_FALSE(has(*live, live->blocks()[2].kill, saved));
  CHECK(has(*live, live->blocks()[2].gen, argument)); CHECK(has(*live, live->blocks()[2].gen, saved));
  auto deaths = live->deaths(*use); REQUIRE(deaths); CHECK(deaths->size() == 2);
  CHECK(std::vector<MachineRegister>(deaths->begin(), deaths->end()) == std::vector<MachineRegister>{argument, saved});
  auto globals = MachineLiveness::analyze(f, *graph, {.global_registers = {&saved, 1}}); REQUIRE(globals);
  CHECK(has(*globals, globals->blocks()[1].out, saved)); CHECK(has(*globals, globals->blocks()[0].in, saved));
  auto low = MachineLiveness::analyze(f, *graph, {.optimization_level = 0}); REQUIRE(low); CHECK_FALSE(low->deaths(*use));
}
TEST_CASE("compact machine scans retain ordered typed sets and validate membership") {
  MachineFunction f("m", "compact", {}, native_target()); auto &b = f.create_block(); MachineRewriter r(f); auto x = reg(f), y = reg(f);
  auto use = append(r, b, MachinePseudoOpcode::Use, {MachineOperand(*x), MachineOperand(*y), MachineOperand(*x)}); append(r, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  std::array<MachineRegister, 2> scan{y, x};
  auto live = MachineLiveness::analyze(f, *graph, {.scan_registers = scan}); REQUIRE(live);
  CHECK(live->registers()[0] == MachineRegister(y)); CHECK(live->registers()[1] == MachineRegister(x));
  CHECK(*live->frequency(x) == 2); CHECK(*live->frequency(y) == 1);
  CHECK(live->blocks()[2].gen.contains(0)); CHECK(live->blocks()[2].gen.contains(1));
  auto dead = live->deaths(*use); REQUIRE(dead); CHECK(std::vector<MachineRegister>(dead->begin(), dead->end()) == std::vector<MachineRegister>{x, y});
  scan[1] = y; CHECK_FALSE(MachineLiveness::analyze(f, *graph, {.scan_registers = scan}));
  MachineFunction other("m", "other", {}, native_target()); scan[1] = reg(other); CHECK_FALSE(MachineLiveness::analyze(f, *graph, {.scan_registers = scan}));
}
TEST_CASE("machine graph traversal does not recurse on long block chains") {
  MachineFunction f("m", "chain", {}, native_target()); MachineRewriter r(f);
  for (unsigned n = 0; n < 4096; ++n) f.create_block(); append(r, *std::prev(f.blocks().end()), Opcode::Ret);
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph); CHECK_FALSE(graph->has_loops()); CHECK(graph->preorder().size() == 4098);
  CHECK(graph->preorder()[1] == 2); CHECK(graph->preorder()[4097] == 4098); CHECK(graph->reverse_postorder()[0] == 1);
  auto live = MachineLiveness::analyze(f, *graph); REQUIRE(live); CHECK(live->blocks().size() == 4098);
}
TEST_CASE("hardware declarations remain observable across all functions in their module") {
  auto target = native_target(); auto hardware = target.architecture() == target::Architecture::AArch64 ? "r19" : "r12";
  auto source = serialization::parse_text("m:module\ng:func\nglobal i64:bound:" + std::string(hardware)
    + "\nmov bound,42\nret\nendfunc\nf:func\nret\nendfunc\nendmodule\n"); REQUIRE(source);
  auto symbol = (*source)->modules().begin()->find_symbol("f"); REQUIRE(symbol); auto function = symbol->function(); REQUIRE(function);
  auto machine = prepare_machine_ir(*function, target); REQUIRE(machine); source->reset();
  auto &f = **machine; auto physical = target.find_register(hardware); REQUIRE(physical);
  REQUIRE(f.global_registers().size() == 1); CHECK(f.global_registers()[0] == *physical);
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph); auto live = MachineLiveness::analyze(f, *graph); REQUIRE(live);
  CHECK(has(*live, live->blocks()[1].out, *physical)); CHECK(has(*live, live->blocks()[0].in, *physical));
  MachineRewriter rewrite(f); auto reserved = target.registers()[target.architecture() == target::Architecture::AArch64 ? 31 : 4].reg;
  auto revision = f.revision(); CHECK_FALSE(rewrite.set_global_registers({&reserved, 1})); CHECK(f.revision() == revision); CHECK(f.global_registers()[0] == *physical);
}
TEST_CASE("machine preparation observes late hardware declarations without cached module state") {
  auto target = native_target();
  auto first = target.architecture() == target::Architecture::AArch64 ? "r20" : "r13";
  auto second = target.architecture() == target::Architecture::AArch64 ? "r19" : "r12";
  auto c = serialization::parse_text("m:module\nf:func\nret\nendfunc\ng:func\nlocal i64:ordinary\nret\nendfunc\nendmodule\nother:module\nh:func\nglobal i64:unrelated:unknown\nret\nendfunc\nendmodule\n"); REQUIRE(c);
  auto &module = *(*c)->modules().begin(); auto &function = *module.functions().begin();
  auto declarations = std::next(module.functions().begin());
  CHECK_FALSE(function.has_hardware_bindings()); CHECK_FALSE(declarations->has_hardware_bindings());
  auto before = prepare_machine_ir(function,target); REQUIRE(before); CHECK((*before)->global_registers().empty());
  auto bound = declarations->create_register(Type::i64(),"bound",first); REQUIRE(bound);
  REQUIRE(declarations->create_register(Type::i64(),"second",second));
  auto alias = declarations->create_register(Type(Type::Kind::U32),"alias",first); REQUIRE(alias); CHECK(*alias == *bound);
  CHECK_FALSE(declarations->create_register(Type(Type::Kind::F64),"incompatible",first));
  CHECK(declarations->has_hardware_bindings());
  auto physical_first = target.find_register(first),physical_second = target.find_register(second); REQUIRE(physical_first); REQUIRE(physical_second);
  auto check = [&](const Function &f) {
    auto machine = prepare_machine_ir(f,target); REQUIRE(machine);
    auto globals = (*machine)->global_registers(); REQUIRE(globals.size() == 2);
    CHECK(globals[0] == *physical_first); CHECK(globals[1] == *physical_second);
  };
  check(function);
  auto copy = clone(**c); REQUIRE(copy); auto copied = (*copy)->find_module("m"); REQUIRE(copied);
  c->reset(); check(*copied->functions().begin());
  CHECK((*before)->global_registers().empty());
}
TEST_CASE("nested machine loops retain their complete forest without recursive ancestry scans") {
  constexpr unsigned count = 512;
  MachineFunction f("m", "nested", {}, native_target()); MachineRewriter r(f); auto x = reg(f);
  auto &entry = f.create_block(); append(r, entry, Opcode::Mov, {MachineOperand(*x), MachineOperand(std::int64_t(0))});
  std::vector<MachineLabel *> labels;
  for (unsigned n = 0; n < count; ++n) {
    auto &b = f.create_block(); auto label = f.create_label(b); REQUIRE(label); labels.push_back(*label);
    append(r, b, Opcode::Mov, {MachineOperand(*x), MachineOperand(*x)});
  }
  for (unsigned n = count; n-- > 0;) {
    auto &b = f.create_block(); append(r, b, Opcode::Bt, {MachineOperand(*labels[n]), MachineOperand(*x)});
  }
  append(r, f.create_block(), Opcode::Ret);
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph); CHECK(graph->has_loops()); CHECK(graph->loop_depth()[count + 2] == count);
  CHECK(graph->loops().size() == graph->cfg().blocks.size() + count + 1);
  auto live = MachineLiveness::analyze(f, *graph); REQUIRE(live); CHECK(*live->frequency(x) > 0);
}
