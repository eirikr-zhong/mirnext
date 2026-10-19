/* Licensed under the MIT License. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Transforms/Materialize.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
Operand integer(std::int64_t value) { return Operand(value); }
struct Example {
  Context context;
  Module *module;
  Function *function;
  BasicBlock *entry;
  Example() {
    auto m = context.create_module("example"); REQUIRE(m); module = *m;
    auto f = module->create_function("entry", {{Type::i64()}, {}}); REQUIRE(f); function = *f;
    auto b = function->create_block("entry"); REQUIRE(b); entry = *b;
  }
};
struct CountCFG {
  using Result = std::size_t;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run(const Function &function) { ++runs; return function.instruction_count(); }
};
struct CountInstructions {
  using Result = std::size_t;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::Instructions;
  static mirnext::Result<Result> run(const Function &function) { ++runs; return function.instruction_count(); }
};
struct CountActiveScopes {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run(const Function &function) {
    std::size_t count = 0; for (auto &scope : function.stack_scopes()) count += !scope.materialized(); return count;
  }
};
struct ReplaceRunning {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run(const Function &, FunctionAnalysisManager &manager) {
    auto replaced = manager.set<ReplaceRunning>(42); if (!replaced) return replaced.error(); return 42;
  }
};
struct OwnedAnalysis {
  using Result = std::unique_ptr<unsigned>;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::Instructions;
  static mirnext::Result<Result> run(const Function &) { ++runs; return std::make_unique<unsigned>(42); }
};
struct TakeRunning {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run(const Function &, FunctionAnalysisManager &manager) {
    auto taken = manager.take_cached<TakeRunning>(); if (!taken) return taken.error(); return 42;
  }
};
struct RewriteLiteral final : FunctionPass {
  Instruction *instruction;
  explicit RewriteLiteral(Instruction *instruction) : instruction(instruction) {}
  std::string_view name() const noexcept override { return "rewrite-literal"; }
  mirnext::Result<PassResult> run(Function &function, FunctionAnalysisManager &) override {
    auto rewritten = IRRewriter(function).replace_operand(*instruction, 1, integer(42)); if (!rewritten) return rewritten.error();
    return PassResult{1, PreservedAnalyses::all()};
  }
};
}
TEST_CASE("scope materialization invalidates cached analyses and emits pass observations") {
  Example e; IRBuilder builder(*e.entry); auto scope = builder.begin_stack_scope(); REQUIRE(scope);
  REQUIRE(builder.allocate_stack(integer(16))); REQUIRE(builder.ret(integer(42))); REQUIRE(builder.end_stack_scope(**scope));
  FunctionAnalysisManager manager(*e.function); CountCFG::runs = 0;
  REQUIRE(manager.get<CountCFG>()); CHECK(CountCFG::runs == 1);
  FunctionPassManager passes; passes.add<MaterializeStackScopesPass>(); std::size_t observations = 0;
  auto result = passes.run(*e.function, manager, [&](const PassEvent &event) -> mirnext::Result<void> {
    ++observations; CHECK(event.pass == "materialize-stack-scopes");
    if (event.phase == PassEvent::Phase::After) { CHECK(event.transformations == 2); CHECK_FALSE(event.function.needs_materialization()); }
    return {};
  }); REQUIRE(result); CHECK(observations == 2); REQUIRE(manager.get<CountCFG>()); CHECK(CountCFG::runs == 2);
  REQUIRE(passes.run(*e.function, manager)); REQUIRE(manager.get<CountCFG>()); CHECK(CountCFG::runs == 2);
  Example unused; REQUIRE(unused.function->create_stack_scope()); REQUIRE(IRBuilder(*unused.entry).ret(integer(42)));
  FunctionAnalysisManager unused_analyses(*unused.function); auto before = unused_analyses.get<CountActiveScopes>(); REQUIRE(before); CHECK(**before == 1);
  auto materialized = materialize_stack_scopes(*unused.function); REQUIRE(materialized); CHECK(materialized->saves == 0);
  auto after = unused_analyses.get<CountActiveScopes>(); REQUIRE(after); CHECK(**after == 0);
}
TEST_CASE("native CFG preserves duplicate switch edges fallthrough and unreachable layout") {
  Example e;
  auto yes = e.function->create_block("yes"); REQUIRE(yes);
  auto no = e.function->create_block("no"); REQUIRE(no);
  auto dead = e.function->create_block("dead"); REQUIRE(dead);
  auto yes_label = e.function->create_label(**yes, "yes"); REQUIRE(yes_label);
  auto no_label = e.function->create_label(**no, "no"); REQUIRE(no_label);
  IRBuilder entry(*e.entry); REQUIRE(entry.emit(Opcode::Switch, {integer(0), Operand(**yes_label), Operand(**no_label), Operand(**yes_label)}));
  IRBuilder yes_builder(**yes); REQUIRE(yes_builder.ret(integer(42)));
  IRBuilder no_builder(**no); REQUIRE(no_builder.ret(integer(0)));
  IRBuilder dead_builder(**dead); REQUIRE(dead_builder.ret(integer(13)));
  auto graph = ControlFlowAnalysis::run(*e.function); REQUIRE(graph);
  CHECK(graph->blocks.size() == 4); CHECK(graph->edges.size() == 3);
  CHECK(graph->incoming[1].size() == 2); CHECK(graph->outgoing[0].size() == 3);
  CHECK_FALSE(graph->reachable[3]); CHECK(graph->reverse_postorder.size() == 3);
  for (auto &edge : graph->edges) CHECK(edge.kind == EdgeKind::SwitchCase);
}
TEST_CASE("native CFG traverses deep graphs without recursive stack growth") {
  Example e;
  for (unsigned n = 0; n < 12000; ++n) { auto block = e.function->create_block(); REQUIRE(block); }
  auto &last = *std::prev(e.function->blocks().end());
  IRBuilder builder(last); REQUIRE(builder.ret(integer(42)));
  auto graph = ControlFlowAnalysis::run(*e.function); REQUIRE(graph);
  CHECK(graph->reverse_postorder.size() == 12001); CHECK(graph->edges.size() == 12000);
}
TEST_CASE("native analysis cache reuses CFG on value edits but cannot preserve stale results") {
  Example e; auto reg = e.function->create_register(Type::i64(), "value"); REQUIRE(reg);
  IRBuilder builder(*e.entry); auto instruction = builder.assign(**reg, integer(40)); REQUIRE(instruction);
  REQUIRE(builder.ret(Operand(**reg))); FunctionAnalysisManager manager(*e.function);
  CountCFG::runs = 0; CountInstructions::runs = 0;
  REQUIRE(manager.get<CountCFG>()); REQUIRE(manager.get<CountInstructions>());
  for (unsigned n = 0; n < 100; ++n) REQUIRE(manager.get<CountCFG>());
  CHECK(CountCFG::runs == 1);
  REQUIRE(IRRewriter(*e.function).replace_operand(**instruction, 1, integer(42)));
  manager.invalidate(PreservedAnalyses::all()); CHECK(manager.cached() == 1);
  REQUIRE(manager.get<CountCFG>()); REQUIRE(manager.get<CountInstructions>());
  CHECK(CountCFG::runs == 1); CHECK(CountInstructions::runs == 2);
  manager.invalidate(PreservedAnalyses::none()); CHECK(manager.cached() == 0);
}
TEST_CASE("updated analyses retain new revisions and cannot replace an active computation") {
  Example e; IRBuilder builder(*e.entry); auto reg = e.function->create_register(Type::i64()); REQUIRE(reg);
  auto assigned = builder.assign(**reg, integer(1)); REQUIRE(assigned); REQUIRE(builder.ret(Operand(**reg)));
  FunctionAnalysisManager manager(*e.function); CountInstructions::runs = 0;
  REQUIRE(manager.set<CountInstructions>(17)); CHECK(**manager.get<CountInstructions>() == 17); CHECK(CountInstructions::runs == 0);
  REQUIRE(IRRewriter(*e.function).replace_operand(**assigned, 1, integer(2)));
  CHECK(**manager.get<CountInstructions>() == 2); CHECK(CountInstructions::runs == 1);
  REQUIRE(manager.set<CountInstructions>(19)); manager.invalidate(PreservedAnalyses::all()); CHECK(**manager.get<CountInstructions>() == 19);
  auto replaced = manager.get<ReplaceRunning>(); REQUIRE_FALSE(replaced);
  CHECK(replaced.error().message.find("running analysis") != std::string::npos); CHECK(manager.cached() == 1);
}
TEST_CASE("analysis ownership transfer moves results without recomputing or returning stale values") {
  Example e; IRBuilder builder(*e.entry); auto reg = e.function->create_register(Type::i64()); REQUIRE(reg);
  auto assigned = builder.assign(**reg, integer(1)); REQUIRE(assigned); REQUIRE(builder.ret(Operand(**reg)));
  FunctionAnalysisManager manager(*e.function); OwnedAnalysis::runs = 0;
  auto missing = manager.take_cached<OwnedAnalysis>(); REQUIRE(missing); CHECK_FALSE(*missing); CHECK(OwnedAnalysis::runs == 0);
  auto cached = manager.get<OwnedAnalysis>(); REQUIRE(cached); auto identity = (*cached)->get(); CHECK(OwnedAnalysis::runs == 1);
  auto owned = manager.take_cached<OwnedAnalysis>(); REQUIRE(owned); REQUIRE(*owned); CHECK((**owned).get() == identity); CHECK(manager.cached() == 0);
  REQUIRE(manager.set<OwnedAnalysis>(std::move(**owned))); auto restored = manager.get<OwnedAnalysis>(); REQUIRE(restored); CHECK((*restored)->get() == identity);
  REQUIRE(IRRewriter(*e.function).replace_operand(**assigned, 1, integer(2)));
  auto stale = manager.take_cached<OwnedAnalysis>(); REQUIRE(stale); CHECK_FALSE(*stale); CHECK(manager.cached() == 0); CHECK(OwnedAnalysis::runs == 1);
  auto running = manager.get<TakeRunning>(); REQUIRE_FALSE(running); CHECK(running.error().message == "cannot take a running analysis");
  CHECK(manager.cached() == 0);
}
TEST_CASE("native register use index distinguishes definitions and both address occurrences") {
  Example e; auto a = e.function->create_register(Type::pointer(), "a"); REQUIRE(a);
  auto b = e.function->create_register(Type::pointer(), "b"); REQUIRE(b);
  auto value = e.function->create_register(Type::i64(), "value"); REQUIRE(value);
  IRBuilder builder(*e.entry); auto assignment = builder.assign(**a, integer(4096)); REQUIRE(assignment);
  Memory memory{Type::i64(), *a, *a, 0, 1}; auto load = builder.assign(**value, Operand(memory)); REQUIRE(load);
  REQUIRE(builder.ret(Operand(**value))); auto index = RegisterUseAnalysis::run(*e.function); REQUIRE(index);
  REQUIRE(index->occurrences.at(*a).size() == 3); CHECK(index->occurrences.at(*a)[0].definition);
  CHECK(index->occurrences.at(*a)[1].part == RegisterOccurrence::Part::MemoryBase);
  REQUIRE(IRRewriter(*e.function).replace_uses(**a, **b));
  CHECK(*(*assignment)->operands()[0].get<const Register *>() == *a);
  auto address = (*load)->operands()[1].get<Memory>(); REQUIRE(address); CHECK(address->base == *b); CHECK(address->index == *b);
}
TEST_CASE("native pass manager observes transformations invalidates analyses and contextualizes failures") {
  Example e; auto reg = e.function->create_register(Type::i64()); REQUIRE(reg);
  IRBuilder builder(*e.entry); auto instruction = builder.assign(**reg, integer(40)); REQUIRE(instruction);
  REQUIRE(builder.ret(Operand(**reg))); FunctionAnalysisManager manager(*e.function);
  REQUIRE(manager.get<CountCFG>()); REQUIRE(manager.get<CountInstructions>());
  FunctionPassManager passes; passes.add<RewriteLiteral>(*instruction);
  std::vector<PassEvent::Phase> observed;
  auto executed = passes.run(*e.function, manager, [&](const PassEvent &event) -> Result<void> {
    observed.push_back(event.phase); CHECK(event.instructions_before == 2);
    if (event.phase == PassEvent::Phase::After) CHECK(event.transformations == 1); return {};
  }); REQUIRE(executed); CHECK(observed.size() == 2); CHECK(manager.cached() == 1);
  auto failed = passes.run(*e.function, manager, [](const PassEvent &) -> Result<void> {
    return Error{ErrorCode::InvalidArgument, "observer declined"};
  }); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("example/entry [rewrite-literal]") != std::string::npos);
  CHECK(failed.error().message.find("observer declined") != std::string::npos);
}
TEST_CASE("native CFG uses only address taken labels and invalidates on label data insertion") {
  Example e; auto address = e.function->create_register(Type::pointer()); REQUIRE(address);
  auto target = e.function->create_block("target"); REQUIRE(target);
  auto untouched = e.function->create_block("untouched"); REQUIRE(untouched);
  auto label = e.function->create_label(**target); REQUIRE(label);
  auto unused = e.function->create_label(**untouched); REQUIRE(unused);
  IRBuilder builder(*e.entry); REQUIRE(builder.emit(Opcode::JmpIndirect, {Operand(**address)}));
  IRBuilder target_builder(**target); REQUIRE(target_builder.ret(integer(42)));
  IRBuilder unused_builder(**untouched); REQUIRE(unused_builder.ret(integer(0)));
  FunctionAnalysisManager analyses(*e.function); auto first = analyses.get<ControlFlowAnalysis>(); REQUIRE(first);
  CHECK((*first)->edges.empty()); CHECK_FALSE((*first)->reachable[1]); CHECK_FALSE((*first)->reachable[2]);
  REQUIRE(e.module->create_data("target_address", LabelReference{*label}));
  auto second = analyses.get<ControlFlowAnalysis>(); REQUIRE(second);
  REQUIRE((*second)->edges.size() == 1); CHECK((*second)->edges[0].destination == 1);
  CHECK((*second)->reachable[1]); CHECK_FALSE((*second)->reachable[2]);
  CHECK((*second)->entry_roots.size() == 2);
}
TEST_CASE("native CFG invalidates when replacing a label address with an ordinary move") {
  Example e; auto address = e.function->create_register(Type::pointer()); REQUIRE(address);
  auto target = e.function->create_block(); REQUIRE(target);
  auto label = e.function->create_label(**target); REQUIRE(label);
  IRBuilder builder(*e.entry); auto load_address = builder.emit(Opcode::LAddr, {Operand(**address), Operand(**label)}); REQUIRE(load_address);
  REQUIRE(builder.emit(Opcode::JmpIndirect, {Operand(**address)}));
  IRBuilder ending(**target); REQUIRE(ending.ret(integer(42)));
  FunctionAnalysisManager analyses(*e.function); auto first = analyses.get<ControlFlowAnalysis>(); REQUIRE(first);
  CHECK((*first)->edges.size() == 1); CHECK((*first)->reachable[1]);
  const Operand replacement[]{Operand(**address), integer(0)};
  REQUIRE(IRRewriter(*e.function).replace(**load_address, Opcode::Mov, replacement));
  auto second = analyses.get<ControlFlowAnalysis>(); REQUIRE(second);
  CHECK((*second)->edges.empty()); CHECK_FALSE((*second)->reachable[1]);
}
TEST_CASE("native value insertion erasure and movement preserve cached CFG") {
  Example e; auto exit = e.function->create_block(); REQUIRE(exit);
  auto reg = e.function->create_register(Type::i64()); REQUIRE(reg);
  IRBuilder ending(**exit); auto ret = ending.ret(integer(42)); REQUIRE(ret);
  FunctionAnalysisManager analyses(*e.function); REQUIRE(analyses.get<ControlFlowAnalysis>());
  auto revision = e.function->revisions().control_flow;
  IRBuilder builder(*e.entry); auto value = builder.assign(**reg, integer(0)); REQUIRE(value);
  IRRewriter rewriter(*e.function);
  for (unsigned n = 0; n < 16; ++n) {
    REQUIRE(rewriter.move_before(**value, **exit, *ret));
    REQUIRE(rewriter.replace_operand(**value, 1, integer(n)));
    REQUIRE(rewriter.move_before(**value, *e.entry));
  }
  REQUIRE(rewriter.erase(**value)); REQUIRE(e.context.verify());
  CHECK(e.function->revisions().control_flow == revision);
  analyses.invalidate(PreservedAnalyses::all()); CHECK(analyses.cached() == 1);
}
TEST_CASE("native structural edits invalidate CFG and use analyses even when preserved") {
  Example e; auto value = e.function->create_register(Type::i64()); REQUIRE(value);
  IRBuilder builder(*e.entry); auto assignment = builder.assign(**value, integer(42)); REQUIRE(assignment);
  auto ret = builder.ret(Operand(**value)); REQUIRE(ret);
  FunctionAnalysisManager analyses(*e.function); CountCFG::runs = 0; CountInstructions::runs = 0;
  REQUIRE(analyses.get<CountCFG>()); REQUIRE(analyses.get<CountInstructions>());
  REQUIRE(analyses.get<RegisterUseAnalysis>()); REQUIRE(analyses.get<ControlFlowAnalysis>());
  IRRewriter rewriter(*e.function); auto split = rewriter.split_before(**ret); REQUIRE(split);
  analyses.invalidate(PreservedAnalyses::all()); CHECK(analyses.cached() == 0);
  REQUIRE(analyses.get<CountCFG>()); CHECK(CountCFG::runs == 2);
  auto graph = analyses.get<ControlFlowAnalysis>(); REQUIRE(graph);
  CHECK((*graph)->blocks.size() == 2); REQUIRE((*graph)->edges.size() == 1);
  CHECK((*graph)->edges[0].kind == EdgeKind::Fallthrough); CHECK((*graph)->blocks[1] == *split);
  auto uses = analyses.get<RegisterUseAnalysis>(); REQUIRE(uses);
  REQUIRE((*uses)->occurrences.at(*value).size() == 2);
  CHECK(&(*uses)->occurrences.at(*value)[1].instruction->block() == *split);
  REQUIRE(rewriter.merge_fallthrough(*e.entry)); analyses.invalidate(PreservedAnalyses::all()); CHECK(analyses.cached() == 0);
  graph = analyses.get<ControlFlowAnalysis>(); REQUIRE(graph); CHECK((*graph)->blocks.size() == 1); CHECK((*graph)->edges.empty());
  uses = analyses.get<RegisterUseAnalysis>(); REQUIRE(uses); CHECK(&(*uses)->occurrences.at(*value)[1].instruction->block() == e.entry);
  REQUIRE(analyses.get<CountCFG>()); CHECK(CountCFG::runs == 3); REQUIRE(e.context.verify());
}
TEST_CASE("native value insertion keeps CFG cache while label cleanup invalidates it once") {
  Example e; auto value = e.function->create_register(Type::i64()); REQUIRE(value);
  auto label = e.function->create_label(*e.entry, "unused"); REQUIRE(label);
  IRBuilder builder(*e.entry); auto ret = builder.ret(integer(42)); REQUIRE(ret);
  FunctionAnalysisManager analyses(*e.function); CountCFG::runs = 0;
  REQUIRE(analyses.get<CountCFG>()); auto revision = e.function->revisions();
  IRRewriter rewriter(*e.function); REQUIRE(rewriter.insert_before(**ret, Opcode::Mov, {Operand(**value), integer(0)}));
  CHECK(e.function->revisions().control_flow == revision.control_flow);
  REQUIRE(analyses.get<CountCFG>()); CHECK(CountCFG::runs == 1);
  auto erased = rewriter.erase_unused_labels(); REQUIRE(erased); CHECK(*erased == 1); REQUIRE(analyses.get<CountCFG>()); CHECK(CountCFG::runs == 2);
  erased = rewriter.erase_unused_labels(); REQUIRE(erased); CHECK(*erased == 0); REQUIRE(analyses.get<CountCFG>()); CHECK(CountCFG::runs == 2);
}
TEST_CASE("native split and merge preserve overflow flags across unlabeled blocks") {
  Example e; auto value = e.function->create_register(Type::i64()); REQUIRE(value);
  auto exit = e.function->create_block(); REQUIRE(exit); auto label = e.function->create_label(**exit); REQUIRE(label);
  IRBuilder builder(*e.entry); REQUIRE(builder.emit(Opcode::Addo, {Operand(**value), integer(INT64_MAX), integer(1)}));
  auto move = builder.assign(**value, Operand(**value)); REQUIRE(move); REQUIRE(builder.emit(Opcode::Bo, {Operand(**label)}));
  REQUIRE(IRBuilder(**exit).ret(integer(42))); REQUIRE(e.context.verify());
  IRRewriter rewriter(*e.function); auto split = rewriter.split_before(**move); REQUIRE(split); REQUIRE(e.context.verify());
  auto graph = ControlFlowAnalysis::run(*e.function); REQUIRE(graph); CHECK(graph->blocks.size() == 3); CHECK(graph->edges.size() == 3);
  REQUIRE(rewriter.merge_fallthrough(*e.entry)); REQUIRE(e.context.verify());
}
