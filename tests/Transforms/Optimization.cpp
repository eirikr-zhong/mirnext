/* Licensed under the MIT License. Owning canonical optimization pipeline. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
constexpr std::string_view scalar
  = "m:module\nentry:func i64\nlocal i64:r\nadd r, 20, 22\nret r\nendfunc\nendmodule\n";
std::unique_ptr<Program> parse (std::string_view text = scalar) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
void formats (const OptimizedProgram &p) {
  REQUIRE ((p.verify ()).has_value ());
  CHECK_FALSE (p.dump ().empty ());
  auto text = p.write_text ();
  REQUIRE ((text).has_value ());
  REQUIRE ((serialization::parse_text ((text).value ())).has_value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto binary = p.write_binary (encoding);
    REQUIRE ((binary).has_value ());
    auto decoded = serialization::read_binary ((binary).value (), {encoding});
    INFO (((decoded).has_value () ? "" : decoded.error ().message));
    REQUIRE ((decoded).has_value ());
    auto again = serialization::write_binary ((decoded).value (), encoding);
    REQUIRE ((again).has_value ());
    CHECK ((again).value () == (binary).value ());
  }
}
}  // namespace
TEST_CASE ("owning pipeline selects original levels and exposes real analysis and SSA stages") {
  for (unsigned n = 0; n != 4; ++n) {
    CAPTURE (n);
    auto source = parse ();
    auto original = dump (*source);
    std::vector<PassId> phases;
    std::size_t before = 0;
    OptimizationOptions options;
    options.level = static_cast<OptimizationLevel> (n);
    options.observer = [&] (const OptimizationSnapshot &s) -> Result<void> {
      REQUIRE ((s.function.verify ()).has_value ());
      if (s.phase == PassEvent::Phase::Before) {
        ++before;
        return {};
      }
      phases.push_back (s.statistics.pass);
      if (s.statistics.pass == PassId::BuildCFG) {
        REQUIRE ((s.graph).has_value ());
        CHECK_FALSE ((s.ssa ()).has_value ());
      }
      if (s.statistics.pass == PassId::BuildSSA) {
        REQUIRE ((s.ssa ()).has_value ());
        CHECK (s.ssa ().value ().renamed ());
        REQUIRE ((s.graph).has_value ());
      }
      if (s.statistics.pass == PassId::BuildLoops) {
        REQUIRE ((s.loops).has_value ());
        REQUIRE ((s.ssa ()).has_value ());
      }
      if (s.statistics.pass == PassId::DestroySSA || s.statistics.pass == PassId::JumpOptimization)
        CHECK_FALSE ((s.ssa ()).has_value ());
      return {};
    };
    auto p = optimize (*source, options);
    INFO (((p).has_value () ? "" : p.error ().message));
    REQUIRE ((p).has_value ());
    std::vector<PassId> expected{PassId::Materialize, PassId::Simplify, PassId::Inline,
                                 PassId::BuildCFG};
    if (n >= 2)
      for (auto pass : {PassId::CloneBlocks, PassId::BuildSSA, PassId::GVN, PassId::CopyPropagation,
                        PassId::DeadStores, PassId::SSADeadCode, PassId::BuildLoops, PassId::LICM,
                        PassId::PressureRelief, PassId::ConventionalSSA, PassId::SSACombine,
                        PassId::DestroySSA, PassId::JumpOptimization})
        expected.push_back (pass);
    CHECK (phases == expected);
    CHECK (before == phases.size ());
    CHECK (p.value ().statistics ().size () == phases.size ());
    CHECK (dump (*source) == original);
    CHECK (p.value ().level () == options.level);
    CHECK (p.value ().target () != target::Architecture::Native);
    formats ((p).value ());
  }
}
TEST_CASE ("address path is chosen before unreachable blocks disappear") {
  for (bool unreachable : {false, true}) {
    auto source = parse (std::string ("m:module\nentry:func i64\nlocal i64:r, i64:p\n")
                         + (unreachable ? "jmp done\ndead:\n" : "mov r, 42\n")
                         + "addr p, r\nmov r, i64:(p)\ndone:\nret 42\nendfunc\nendmodule\n");
    std::vector<PassId> stages;
    std::vector<bool> renames;
    OptimizationOptions options;
    options.observer = [&] (const OptimizationSnapshot &s) -> Result<void> {
      if (s.phase != PassEvent::Phase::After) return {};
      stages.push_back (s.statistics.pass);
      if (s.statistics.pass == PassId::BuildSSA) {
        REQUIRE ((s.ssa ()).has_value ());
        renames.push_back (s.ssa ().get ()->renamed ());
      }
      if (s.statistics.pass == PassId::TransformAddresses) CHECK_FALSE ((s.ssa ()).has_value ());
      return {};
    };
    auto p = optimize (*source, options);
    INFO (((p).has_value () ? "" : p.error ().message));
    REQUIRE ((p).has_value ());
    CHECK (renames == std::vector<bool>{false, true});
    REQUIRE (stages.size () > 7);
    CHECK (stages[4] == PassId::BuildSSA);
    CHECK (stages[5] == PassId::TransformAddresses);
    CHECK (stages[6] == PassId::CloneBlocks);
    CHECK (stages[7] == PassId::BuildSSA);
    formats ((p).value ());
  }
}
TEST_CASE (
  "optimization owns all nodes retains no callback and supports repeat and move assignment") {
  std::weak_ptr<int> callback_lifetime;
  auto make = [&] () -> Result<OptimizedProgram> {
    auto source = parse ();
    auto identity = &*source->modules ().begin ()->functions ().begin ();
    auto captured = std::make_shared<int> (0);
    callback_lifetime = captured;
    OptimizationOptions options;
    options.observer = [captured] (const OptimizationSnapshot &) -> Result<void> {
      ++*captured;
      return {};
    };
    auto result = optimize (*source, options);
    REQUIRE ((result).has_value ());
    CHECK (result.value ().find_function ("m", "entry").get () != identity);
    return result;
  };
  auto first = make ();
  REQUIRE ((first).has_value ());
  CHECK (callback_lifetime.expired ());
  formats ((first).value ());
  auto second = optimize (first.value ().program ());
  REQUIRE ((second).has_value ());
  formats ((second).value ());
  auto moved = std::move ((first).value ());
  CHECK_FALSE ((first.value ().verify ()).has_value ());
  CHECK_FALSE ((first.value ().write_binary ()).has_value ());
  CHECK_FALSE ((first.value ().write_text ()).has_value ());
  CHECK (first.value ().statistics ().empty ());
  CHECK (first.value ().find_function ("m", "entry").get () == nullptr);
  (second).value () = std::move (moved);
  CHECK_FALSE ((moved.verify ()).has_value ());
  formats ((second).value ());
}
TEST_CASE ("module cloning and optimization ignore unrelated invalid source modules") {
  auto source = parse ();
  auto &module = *source->modules ().begin ();
  auto bad = mirnext::ir::detail::access (*(source)).create_module ("unrelated");
  REQUIRE ((bad).has_value ());
  auto f = mirnext::ir::detail::access (((bad)).value ()).create_function ("bad", {});
  REQUIRE ((f).has_value ());
  auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  CHECK_FALSE ((InstructionBuilder ((b).value ()).emit (Opcode::Add, {})).has_value ());
  CHECK_FALSE ((source->verify ()).has_value ());
  auto copied = clone (module);
  REQUIRE ((copied).has_value ());
  CHECK (std::distance (copied.value ().modules ().begin (), copied.value ().modules ().end ())
         == 1);
  auto p = optimize (module);
  REQUIRE ((p).has_value ());
  formats ((p).value ());
  CHECK_FALSE ((optimize (*source)).has_value ());
}
TEST_CASE (
  "module optimization retains external imports while context optimization resolves cross module "
  "inline") {
  auto fixtures = test::inline_fixtures ();
  auto source = parse (fixtures[fixtures.size () - 2]);
  auto module = source->find_module ("b").get ();
  REQUIRE (module);
  auto imported = optimize (*module);
  REQUIRE ((imported).has_value ());
  auto entry = imported.value ().find_function ("b", "entry").get ();
  REQUIRE (entry);
  std::size_t calls = 0;
  for (auto &b : entry->blocks ())
    for (auto &i : b.instructions ()) calls += i.opcode () == Opcode::Call;
  CHECK (calls == 1);
  CHECK (imported.value ().program ().find_module ("a").get () == nullptr);
  formats ((imported).value ());
  auto complete = optimize (*source);
  REQUIRE ((complete).has_value ());
  entry = complete.value ().find_function ("b", "entry").get ();
  REQUIRE (entry);
  calls = 0;
  for (auto &b : entry->blocks ())
    for (auto &i : b.instructions ()) calls += i.opcode () == Opcode::Call;
  CHECK (calls == 0);
  CHECK (complete.value ().program ().find_module ("a").get () != nullptr);
  source.reset ();
  formats ((complete).value ());
}
TEST_CASE (
  "pipeline diagnostics preserve source state for observer invalid target level and SSA failures") {
  auto source = parse ();
  auto original = dump (*source);
  for (auto phase : {PassEvent::Phase::Before, PassEvent::Phase::After}) {
    OptimizationOptions options;
    options.observer = [phase] (const OptimizationSnapshot &s) -> Result<void> {
      if (s.phase == phase && s.statistics.pass == PassId::BuildSSA)
        return Error{ErrorCode::InvalidArgument, "observer rejected SSA"};
      return {};
    };
    auto p = optimize (*source, options);
    CHECK_FALSE ((p).has_value ());
    CHECK (p.error ().message.find ("entry [build-ssa]") != std::string::npos);
    CHECK (p.error ().message.find ("observer rejected SSA") != std::string::npos);
    CHECK (dump (*source) == original);
  }
  OptimizationOptions options;
  options.level = static_cast<OptimizationLevel> (4);
  CHECK_FALSE ((optimize (*source, options)).has_value ());
  options = {};
  options.target = static_cast<target::Architecture> (255);
  CHECK_FALSE ((optimize (*source, options)).has_value ());
  auto native = target::AddressingRules::get ();
  REQUIRE ((native).has_value ());
  options.target = native.value ().architecture () == target::Architecture::X86_64
                     ? target::Architecture::AArch64
                     : target::Architecture::X86_64;
  CHECK_FALSE ((optimize (*source, options)).has_value ());
  CHECK (dump (*source) == original);
  auto &f = *source->modules ().begin ()->functions ().begin ();
  FunctionAnalysisManager analyses (f);
  FunctionPassManager passes;
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
  CHECK_FALSE ((optimize (*source)).has_value ());
}
TEST_CASE ("cached analysis observations never construct recompute or erase analyses") {
  auto source = parse ();
  auto &f = *source->modules ().begin ()->functions ().begin ();
  FunctionAnalysisManager a (f);
  CHECK (a.get_cached<FunctionGraphAnalysis> ().get () == nullptr);
  CHECK (a.cached () == 0);
  auto graph = a.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (a.get_cached<FunctionGraphAnalysis> ().get () == &(graph).value ());
  auto count = a.cached ();
  auto block = mirnext::ir::detail::access (f).create_block ();
  REQUIRE ((block).has_value ());
  REQUIRE ((InstructionBuilder ((block).value ()).ret (Operand (0))).has_value ());
  CHECK (a.get_cached<FunctionGraphAnalysis> ().get () == nullptr);
  CHECK (a.cached () == count);
}
TEST_CASE ("branch register rewrites preserve CFG but label rewrites invalidate it") {
  auto source = parse (
    "m:module\nentry:func i64, i64:c, i64:d\nbt yes, c\nret 0\nyes:\nret 42\nother:\nret "
    "7\nendfunc\nendmodule\n");
  auto &f = *source->modules ().begin ()->functions ().begin ();
  FunctionAnalysisManager analyses (f);
  auto graph = analyses.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto branch = &*f.blocks ().begin ()->instructions ().begin ();
  auto revision = f.revisions ();
  std::vector<Operand> operands (branch->operands ().begin (), branch->operands ().end ());
  operands[1] = Operand (*mirnext::ir::detail::ReferenceAccess::pointers (f.arguments ())[1]);
  REQUIRE ((InstructionRewriter (f).replace (*branch, Opcode::Bt, operands)).has_value ());
  CHECK (f.revisions ().instructions > revision.instructions);
  CHECK (f.revisions ().control_flow == revision.control_flow);
  CHECK (analyses.get_cached<FunctionGraphAnalysis> ().get () == &(graph).value ());
  auto label = f.find_label ("other").get ();
  REQUIRE (label);
  operands[0] = Operand (*label);
  REQUIRE ((InstructionRewriter (f).replace (*branch, Opcode::Bt, operands)).has_value ());
  CHECK (f.revisions ().control_flow > revision.control_flow);
  CHECK (analyses.get_cached<FunctionGraphAnalysis> ().get () == nullptr);
}
TEST_CASE ("cloning constant pruning and GVN eliminate closed single input PHIs") {
  auto source = parse (
    "m:module\nentry:func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\nblt hot, "
    "v, 14\nret v\ncold:\nmov v, 14\njmp hot\nendfunc\nendmodule\n");
  for (auto level : {OptimizationLevel::O2, OptimizationLevel::O3}) {
    OptimizationOptions options;
    options.level = level;
    auto p = optimize (*source, options);
    INFO (((p).has_value () ? "" : p.error ().message));
    REQUIRE ((p).has_value ());
    formats ((p).value ());
  }
}
TEST_CASE ("pipeline materializes nested scope edges without altering source scope handles") {
  for (auto mode :
       {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto source = test::stack_scope_example (mode);
    auto before = dump (*source);
    auto p = optimize (*source);
    INFO (((p).has_value () ? "" : p.error ().message));
    REQUIRE ((p).has_value ());
    CHECK (dump (*source) == before);
    for (auto &m : p.value ().program ().modules ())
      for (auto &f : m.functions ()) {
        CHECK_FALSE (f.needs_materialization ());
        CHECK_FALSE ((f.ssa_form ()).has_value ());
      }
    source.reset ();
    formats ((p).value ());
  }
}
TEST_CASE ("full pipeline covers all public opcodes at every optimization level") {
  for (auto &d : instruction_descriptors ())
    for (unsigned n = 0; n != 4; ++n) {
      CAPTURE (d.name);
      CAPTURE (n);
      auto source = test::public_instruction_example (d);
      OptimizationOptions options;
      options.level = static_cast<OptimizationLevel> (n);
      auto p = optimize (*source, options);
      INFO (((p).has_value () ? "" : p.error ().message));
      REQUIRE ((p).has_value ());
      formats ((p).value ());
    }
}
TEST_CASE ("owning pipeline integrates inlining relocations and seeded jump interactions") {
  auto inputs = test::inline_fixtures ();
  auto random = test::random_jump_fixtures ();
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs) {
    CAPTURE (text);
    auto source = parse (text);
    auto p = optimize (*source);
    INFO (((p).has_value () ? "" : p.error ().message));
    REQUIRE ((p).has_value ());
    for (auto &m : source->modules ())
      for (auto &f : m.functions ())
        CHECK (p.value ().find_function (m.name (), f.symbol ().name ()).get () != nullptr);
    source.reset ();
    formats ((p).value ());
  }
}
