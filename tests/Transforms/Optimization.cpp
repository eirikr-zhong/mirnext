/* Licensed under the MIT License. Owning canonical optimization pipeline. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "JumpFixtures.hpp"
#include "InlineFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/StackScopes.hpp"
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::analysis; using namespace mirnext::transforms;
namespace {
constexpr std::string_view scalar = "m:module\nentry:func i64\nlocal i64:r\nadd r, 20, 22\nret r\nendfunc\nendmodule\n";
std::unique_ptr<Context> parse(std::string_view text = scalar) {
  auto c = serialization::parse_text(text); INFO((c ? "" : c.error().message)); REQUIRE(c); return std::move(*c);
}
void formats(const OptimizedProgram &p) {
  REQUIRE(p.verify()); CHECK_FALSE(p.dump().empty()); auto text = p.write_text(); REQUIRE(text); REQUIRE(serialization::parse_text(*text));
  for (auto encoding : {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto binary = p.encode_binary(encoding); REQUIRE(binary); auto decoded = serialization::read_binary(*binary, {encoding}); INFO((decoded ? "" : decoded.error().message)); REQUIRE(decoded);
    auto again = serialization::write_binary(**decoded, encoding); REQUIRE(again); CHECK(*again == *binary);
  }
}
}
TEST_CASE("owning pipeline selects original levels and exposes real analysis and SSA stages") {
  for (unsigned n = 0; n != 4; ++n) {
    CAPTURE(n); auto source = parse(); auto original = dump(*source); std::vector<PassId> phases; std::size_t before = 0;
    OptimizationOptions options; options.level = static_cast<OptimizationLevel>(n);
    options.observer = [&](const OptimizationSnapshot &s) -> Result<void> {
      REQUIRE(s.function.verify());
      if (s.phase == PassEvent::Phase::Before) { ++before; return {}; }
      phases.push_back(s.statistics.pass);
      if (s.statistics.pass == PassId::BuildCFG) { REQUIRE(s.graph); CHECK_FALSE(s.ssa()); }
      if (s.statistics.pass == PassId::BuildSSA) { REQUIRE(s.ssa()); CHECK(s.ssa()->renamed()); REQUIRE(s.graph); }
      if (s.statistics.pass == PassId::BuildLoops) { REQUIRE(s.loops); REQUIRE(s.ssa()); }
      if (s.statistics.pass == PassId::DestroySSA || s.statistics.pass == PassId::JumpOptimization) CHECK_FALSE(s.ssa());
      return {};
    };
    auto p = optimize(*source, options); INFO((p ? "" : p.error().message)); REQUIRE(p);
    std::vector<PassId> expected{PassId::Materialize, PassId::Simplify, PassId::Inline, PassId::BuildCFG};
    if (n >= 2) for (auto pass : {PassId::CloneBlocks, PassId::BuildSSA, PassId::GVN, PassId::CopyPropagation,
      PassId::DeadStores, PassId::SSADeadCode, PassId::BuildLoops, PassId::LICM, PassId::PressureRelief,
      PassId::ConventionalSSA, PassId::SSACombine, PassId::DestroySSA, PassId::JumpOptimization}) expected.push_back(pass);
    CHECK(phases == expected); CHECK(before == phases.size()); CHECK(p->statistics().size() == phases.size());
    CHECK(dump(*source) == original); CHECK(p->level() == options.level); CHECK(p->target() != target::Architecture::Native);
    formats(*p);
  }
}
TEST_CASE("address path is chosen before unreachable blocks disappear") {
  for (bool unreachable : {false, true}) {
    auto source = parse(std::string("m:module\nentry:func i64\nlocal i64:r, i64:p\n")
      + (unreachable ? "jmp done\ndead:\n" : "mov r, 42\n") + "addr p, r\nmov r, i64:(p)\ndone:\nret 42\nendfunc\nendmodule\n");
    std::vector<PassId> stages; std::vector<bool> renames; OptimizationOptions options;
    options.observer = [&](const OptimizationSnapshot &s) -> Result<void> {
      if (s.phase != PassEvent::Phase::After) return {};
      stages.push_back(s.statistics.pass);
      if (s.statistics.pass == PassId::BuildSSA) { REQUIRE(s.ssa()); renames.push_back(s.ssa()->renamed()); }
      if (s.statistics.pass == PassId::TransformAddresses) CHECK_FALSE(s.ssa()); return {};
    };
    auto p = optimize(*source, options); INFO((p ? "" : p.error().message)); REQUIRE(p);
    CHECK(renames == std::vector<bool>{false, true});
    REQUIRE(stages.size() > 7); CHECK(stages[4] == PassId::BuildSSA); CHECK(stages[5] == PassId::TransformAddresses);
    CHECK(stages[6] == PassId::CloneBlocks); CHECK(stages[7] == PassId::BuildSSA); formats(*p);
  }
}
TEST_CASE("optimization owns all nodes retains no callback and supports repeat and move assignment") {
  std::weak_ptr<int> callback_lifetime;
  auto make = [&]() -> Result<OptimizedProgram> {
    auto source = parse(); auto identity = &*source->modules().begin()->functions().begin();
    auto captured = std::make_shared<int>(0); callback_lifetime = captured; OptimizationOptions options;
    options.observer = [captured](const OptimizationSnapshot &) -> Result<void> { ++*captured; return {}; };
    auto result = optimize(*source, options); REQUIRE(result); CHECK(result->find_function("m", "entry") != identity); return result;
  };
  auto first = make(); REQUIRE(first); CHECK(callback_lifetime.expired()); formats(*first);
  auto second = optimize(first->context()); REQUIRE(second); formats(*second);
  auto moved = std::move(*first); CHECK_FALSE(first->verify()); CHECK_FALSE(first->encode_binary()); CHECK_FALSE(first->write_text());
  CHECK(first->statistics().empty()); CHECK(first->find_function("m", "entry") == nullptr);
  *second = std::move(moved); CHECK_FALSE(moved.verify()); formats(*second);
}
TEST_CASE("module cloning and optimization ignore unrelated invalid source modules") {
  auto source = parse(); auto &module = *source->modules().begin();
  auto bad = source->create_module("unrelated"); REQUIRE(bad);
  auto f = (*bad)->create_function("bad", {}); REQUIRE(f); auto b = (*f)->create_block(); REQUIRE(b);
  CHECK_FALSE(IRBuilder(**b).emit(Opcode::Add, {})); CHECK_FALSE(source->verify());
  auto copied = clone(module); REQUIRE(copied); CHECK(std::distance((*copied)->modules().begin(), (*copied)->modules().end()) == 1);
  auto p = optimize(module); REQUIRE(p); formats(*p); CHECK_FALSE(optimize(*source));
}
TEST_CASE("module optimization retains external imports while context optimization resolves cross module inline") {
  auto fixtures = test::inline_fixtures(); auto source = parse(fixtures[fixtures.size() - 2]); auto module = source->find_module("b"); REQUIRE(module);
  auto imported = optimize(*module); REQUIRE(imported); auto entry = imported->find_function("b", "entry"); REQUIRE(entry);
  std::size_t calls = 0; for (auto &b : entry->blocks()) for (auto &i : b.instructions()) calls += i.opcode() == Opcode::Call; CHECK(calls == 1);
  CHECK(imported->context().find_module("a") == nullptr); formats(*imported);
  auto complete = optimize(*source); REQUIRE(complete); entry = complete->find_function("b", "entry"); REQUIRE(entry);
  calls = 0; for (auto &b : entry->blocks()) for (auto &i : b.instructions()) calls += i.opcode() == Opcode::Call; CHECK(calls == 0);
  CHECK(complete->context().find_module("a") != nullptr); source.reset(); formats(*complete);
}
TEST_CASE("pipeline diagnostics preserve source state for observer invalid target level and SSA failures") {
  auto source = parse(); auto original = dump(*source);
  for (auto phase : {PassEvent::Phase::Before, PassEvent::Phase::After}) {
    OptimizationOptions options; options.observer = [phase](const OptimizationSnapshot &s) -> Result<void> {
      if (s.phase == phase && s.statistics.pass == PassId::BuildSSA) return Error{ErrorCode::InvalidArgument, "observer rejected SSA"}; return {};
    };
    auto p = optimize(*source, options); CHECK_FALSE(p); CHECK(p.error().message.find("entry [build-ssa]") != std::string::npos);
    CHECK(p.error().message.find("observer rejected SSA") != std::string::npos); CHECK(dump(*source) == original);
  }
  OptimizationOptions options; options.level = static_cast<OptimizationLevel>(4); CHECK_FALSE(optimize(*source, options));
  options = {}; options.target = static_cast<target::Architecture>(255); CHECK_FALSE(optimize(*source, options));
  auto native = target::AddressingRules::get(); REQUIRE(native);
  options.target = native->architecture() == target::Architecture::X86_64 ? target::Architecture::AArch64 : target::Architecture::X86_64;
  CHECK_FALSE(optimize(*source, options)); CHECK(dump(*source) == original);
  auto &f = *source->modules().begin()->functions().begin(); FunctionAnalysisManager analyses(f); FunctionPassManager passes;
  passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(); REQUIRE(passes.run(f, analyses)); CHECK_FALSE(optimize(*source));
}
TEST_CASE("cached analysis observations never construct recompute or erase analyses") {
  auto source = parse(); auto &f = *source->modules().begin()->functions().begin(); FunctionAnalysisManager a(f);
  CHECK(a.get_cached<FunctionGraphAnalysis>() == nullptr); CHECK(a.cached() == 0);
  auto graph = a.get<FunctionGraphAnalysis>(); REQUIRE(graph); CHECK(a.get_cached<FunctionGraphAnalysis>() == *graph); auto count = a.cached();
  auto block = f.create_block(); REQUIRE(block); REQUIRE(IRBuilder(**block).ret(Operand(0)));
  CHECK(a.get_cached<FunctionGraphAnalysis>() == nullptr); CHECK(a.cached() == count);
}
TEST_CASE("branch register rewrites preserve CFG but label rewrites invalidate it") {
  auto source = parse("m:module\nentry:func i64, i64:c, i64:d\nbt yes, c\nret 0\nyes:\nret 42\nother:\nret 7\nendfunc\nendmodule\n");
  auto &f = *source->modules().begin()->functions().begin(); FunctionAnalysisManager analyses(f);
  auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto branch = &*f.blocks().begin()->instructions().begin();
  auto revision = f.revisions(); std::vector<Operand> operands(branch->operands().begin(), branch->operands().end()); operands[1] = Operand(*f.arguments()[1]);
  REQUIRE(IRRewriter(f).replace(*branch, Opcode::Bt, operands));
  CHECK(f.revisions().instructions > revision.instructions); CHECK(f.revisions().control_flow == revision.control_flow);
  CHECK(analyses.get_cached<FunctionGraphAnalysis>() == *graph);
  auto label = f.find_label("other"); REQUIRE(label); operands[0] = Operand(*label);
  REQUIRE(IRRewriter(f).replace(*branch, Opcode::Bt, operands));
  CHECK(f.revisions().control_flow > revision.control_flow); CHECK(analyses.get_cached<FunctionGraphAnalysis>() == nullptr);
}
TEST_CASE("cloning constant pruning and GVN eliminate closed single input PHIs") {
  auto source = parse("m:module\nentry:func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\nblt hot, v, 14\nret v\ncold:\nmov v, 14\njmp hot\nendfunc\nendmodule\n");
  for (auto level : {OptimizationLevel::O2, OptimizationLevel::O3}) {
    OptimizationOptions options; options.level = level; auto p = optimize(*source, options); INFO((p ? "" : p.error().message)); REQUIRE(p); formats(*p);
  }
}
TEST_CASE("pipeline materializes nested scope edges without altering source scope handles") {
  for (auto mode : {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto source = test::stack_scope_example(mode); auto before = dump(*source);
    auto p = optimize(*source); INFO((p ? "" : p.error().message)); REQUIRE(p); CHECK(dump(*source) == before);
    for (auto &m : p->context().modules()) for (auto &f : m.functions()) { CHECK_FALSE(f.needs_materialization()); CHECK_FALSE(f.ssa_form()); }
    source.reset(); formats(*p);
  }
}
TEST_CASE("full pipeline covers all public opcodes at every optimization level") {
  for (auto &d : instruction_descriptors()) for (unsigned n = 0; n != 4; ++n) {
    CAPTURE(d.name); CAPTURE(n); auto source = test::public_instruction_example(d); OptimizationOptions options; options.level = static_cast<OptimizationLevel>(n);
    auto p = optimize(*source, options); INFO((p ? "" : p.error().message)); REQUIRE(p); formats(*p);
  }
}
TEST_CASE("owning pipeline integrates inlining relocations and seeded jump interactions") {
  auto inputs = test::inline_fixtures(); auto random = test::random_jump_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto source = parse(text); auto p = optimize(*source); INFO((p ? "" : p.error().message)); REQUIRE(p);
    for (auto &m : source->modules()) for (auto &f : m.functions()) CHECK(p->find_function(m.name(), f.symbol().name()) != nullptr);
    source.reset(); formats(*p);
  }
}
