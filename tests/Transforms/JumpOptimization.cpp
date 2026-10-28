/* Licensed under the MIT License. Canonical post-SSA jump and identity regressions. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/JumpOptimization.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "JumpFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
std::unique_ptr<Program> parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
Function &entry (Program &c) {
  return const_cast<Function &> (
    *c.modules ().begin ()->find_symbol ("entry").get ()->function ().get ());
}
void prepare (Function &f, FunctionAnalysisManager &a, bool prune = true) {
  FunctionPassManager p;
  p.add<SimplifyPass> ();
  p.add<PrepareCFGPass> (prune);
  auto checked = p.run (f, a);
  INFO (((checked).has_value () ? "" : checked.error ().message));
  REQUIRE ((checked).has_value ());
}
void formats (Program &c) {
  REQUIRE ((c.verify ()).has_value ());
  auto binary = serialization::write_binary (c);
  REQUIRE ((binary).has_value ());
  REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
  auto text = serialization::write_text (c);
  REQUIRE ((text).has_value ());
  REQUIRE ((serialization::parse_text ((text).value ())).has_value ());
}
}  // namespace
TEST_CASE (
  "jump optimization removes direct fallthrough branches with stable surviving identities") {
  auto fixtures = test::jump_fixtures ();
  for (auto &text : fixtures)
    if (text.find (" done,") != text.npos && text.find ("ret 0") == text.npos
        && text.find ("jmp") == text.npos) {
      CAPTURE (text);
      auto c = parse (text);
      auto &f = entry (*c);
      FunctionAnalysisManager a (f);
      FunctionPassManager p;
      p.add<PrepareCFGPass> ();
      REQUIRE ((p.run (f, a)).has_value ());
      const Instruction *ret = nullptr;
      const BasicBlock *ret_block = nullptr;
      std::size_t branches = 0;
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ()) {
          if (i.opcode () == Opcode::Ret) {
            ret = &i;
            ret_block = &b;
          }
          if (instruction_descriptor (i.opcode ()).get ()->flow == ControlFlow::Branch) ++branches;
        }
      REQUIRE (branches == 1);
      auto result = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      CHECK (ret->opcode () == Opcode::Ret);
      CHECK (&ret->block () == ret_block);
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ())
          CHECK (instruction_descriptor (i.opcode ()).value ().flow != ControlFlow::Branch);
      auto graph = a.get<FunctionGraphAnalysis> ();
      REQUIRE ((graph).has_value ());
      auto cfg = a.get<ControlFlowAnalysis> ();
      REQUIRE ((cfg).has_value ());
      formats (*c);
    }
}
TEST_CASE ("jump optimization preserves forbidden property switch and indirect targets") {
  for (auto &text : test::jump_fixtures ())
    if (text.find ("prbe") != text.npos || text.find ("prbn") != text.npos
        || text.find ("switch") != text.npos || text.find ("jmpi") != text.npos) {
      CAPTURE (text);
      auto c = parse (text);
      auto &f = entry (*c);
      FunctionAnalysisManager a (f);
      prepare (f, a);
      std::vector<std::pair<const Instruction *, const Label *>> targets;
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ())
          if (i.opcode () == Opcode::Switch || i.opcode () == Opcode::PrBeq
              || i.opcode () == Opcode::PrBne || i.opcode () == Opcode::LAddr)
            for (auto &operand : i.operands ())
              if (auto label = mirnext::ir::detail::operand_access (operand).get<const Label *> ())
                targets.emplace_back (&i, *label);
      auto result = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      for (auto [i, label] : targets) {
        bool retained = false;
        for (auto &operand : i->operands ())
          if (auto l = mirnext::ir::detail::operand_access (operand).get<const Label *> ())
            retained |= *l == label;
        CHECK (retained);
      }
      formats (*c);
    }
}
TEST_CASE ("jump optimization preserves owned results formats and repeated independent analyses") {
  auto inputs = test::jump_fixtures ();
  auto random = test::random_jump_fixtures ();
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs) {
    CAPTURE (text);
    auto source = parse (text);
    auto original = dump (*source);
    auto c = clone (*source);
    REQUIRE ((c).has_value ());
    auto &f = entry ((c).value ());
    FunctionAnalysisManager a (f);
    prepare (f, a, false);
    auto checked = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a);
    INFO (((checked).has_value () ? "" : checked.error ().message));
    REQUIRE ((checked).has_value ());
    CHECK (dump (*source) == original);
    source.reset ();
    FunctionAnalysisManager fresh (f);
    REQUIRE (
      (::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, fresh)).has_value ());
    formats ((c).value ());
  }
}
TEST_CASE (
  "jump optimization retains historical address roots without artificial entry reachability") {
  auto c = parse (
    "m: module\nentry: func i64\nlocal i64:p\nladdr p, kept\nret 42\ndead:\nret 9\nkept:\nret "
    "13\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a, false);
  auto graph = a.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto saved = a.take_cached<FunctionGraphAnalysis> ();
  REQUIRE ((saved).has_value ());
  const BasicBlock *kept = nullptr;
  Instruction *address = nullptr;
  for (auto &b : f.blocks ()) {
    for (auto label : mirnext::ir::detail::ReferenceAccess::pointers (b.labels ()))
      if (label->name () == "kept") kept = &b;
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::LAddr) address = &i;
  }
  REQUIRE (kept);
  REQUIRE (address);
  REQUIRE (saved.value ().has_value ());
  REQUIRE (((saved).value ())->index (*kept));
  CHECK (((saved).value ())->blocks[*((saved).value ())->index (*kept)].address_taken);
  REQUIRE ((InstructionRewriter (f).erase (*address)).has_value ());
  REQUIRE ((a.set<FunctionGraphAnalysis> (std::move (*(saved).value ()))).has_value ());
  REQUIRE ((::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a)).has_value ());
  graph = a.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (graph.value ().index (*kept));
  for (auto &b : f.blocks ())
    for (auto l : mirnext::ir::detail::ReferenceAccess::pointers (b.labels ()))
      CHECK (l->name () != "dead");
  formats (*c);
}
TEST_CASE ("jump optimization rejects SSA foreign ownership and unseparated anchors before edits") {
  auto c = parse (test::jump_fixtures ()[1]), other = parse (test::jump_fixtures ()[0]);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f), foreign (entry (*other));
  auto before = dump (f);
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, foreign)).has_value ());
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, a);
  FunctionPassManager p;
  p.add<BuildSSAPass> ();
  REQUIRE ((p.run (f, a)).has_value ());
  before = dump_ssa (f);
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a)).has_value ());
  CHECK (dump_ssa (f) == before);
  FunctionPassManager finish;
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  REQUIRE ((finish.run (f, a)).has_value ());
  REQUIRE ((::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a)).has_value ());
  formats (*c);
}
TEST_CASE ("jump optimization handles long adjacent label chains without recursive traversal") {
  std::ostringstream text;
  text << "m: module\nentry: func i64\njmp label0\n";
  for (unsigned n = 0; n < 4096; ++n) text << "label" << n << ":\n";
  text << "ret 42\nendfunc\nendmodule\n";
  auto c = parse (text.str ());
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  FunctionPassManager p;
  p.add<PrepareCFGPass> ();
  REQUIRE ((p.run (f, a)).has_value ());
  CHECK (std::distance (f.blocks ().begin (), f.blocks ().end ()) >= 4096);
  auto result = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  CHECK (f.instruction_count () == 1);
  formats (*c);
}
TEST_CASE ("jump optimization builds verifies and encodes every public opcode") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto c = test::public_instruction_example (descriptor);
    auto &f = entry (*c);
    FunctionAnalysisManager a (f);
    prepare (f, a);
    auto checked = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (), f, a);
    INFO (((checked).has_value () ? "" : checked.error ().message));
    REQUIRE ((checked).has_value ());
    formats (*c);
  }
}
