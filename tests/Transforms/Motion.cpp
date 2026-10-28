/* Licensed under the MIT License. Canonical SSA deletion and code motion. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Transforms/Motion.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "MemoryFixtures.hpp"
#include "GVNFixtures.hpp"
#include "MotionFixtures.hpp"
#include <algorithm>
#include <set>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
std::unique_ptr<Program> parse (std::string_view text) {
  auto result = serialization::parse_text (text);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::make_unique<Program> (std::move ((result).value ()));
}
Function &entry (Program &c) {
  return const_cast<Function &> (
    *c.modules ().begin ()->find_symbol ("entry").get ()->function ().get ());
}
void prepare (Function &f, FunctionAnalysisManager &a, bool gvn = false) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> ();
  if (gvn) {
    passes.add<GVNPass> ();
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
  }
  auto result = passes.run (f, a);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
}
std::size_t count (const Function &f, Opcode op) {
  std::size_t result = 0;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) result += i.opcode () == op;
  return result;
}
void finish (Program &c) {
  FunctionPassManager passes;
  passes.add<ConventionalSSAPass> ();
  passes.add<DestroySSAPass> ();
  REQUIRE ((passes.run (c)).has_value ());
  auto binary = serialization::write_binary (c);
  REQUIRE ((binary).has_value ());
  REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
  auto text = serialization::write_text (c);
  REQUIRE ((text).has_value ());
  REQUIRE ((serialization::parse_text ((text).value ())).has_value ());
}
}  // namespace
TEST_CASE ("SSA dead code recursively removes unused definitions and PHIs with stable live nodes") {
  auto c = parse (R"(m: module
entry: func i64, i64:x
local i64:a, i64:b, i64:c
bt yes, x
add a, x, 1
jmp join
yes:
mul a, x, 2
join:
add b, a, a
mul c, b, b
ret x
endfunc
endmodule
)");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto old = f.instruction_count ();
  REQUIRE (f.ssa_form ().value ().phis ().begin () != f.ssa_form ().value ().phis ().end ());
  const Instruction *ret = nullptr;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Ret) ret = &i;
  auto result = ::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().transformations >= 4);
  CHECK (f.instruction_count () < old);
  CHECK (count (f, Opcode::Add) == 0);
  CHECK (count (f, Opcode::Mul) == 0);
  CHECK (ret->opcode () == Opcode::Ret);
  for (auto &phi : f.ssa_form ().get ()->phis ()) CHECK_FALSE (phi.alive ());
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
TEST_CASE ("SSA dead code retains allocation call and hardware return effects") {
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  auto c = parse("m: module\nsig: proto i64\nimport side\nentry: func i64, i64:x\nglobal i64:h:" + std::string(binding)
    + "\nlocal i64:p, i64:s, i64:r\nalloca p, 16\nbstart s\ncall sig, side, r\nadd h, x, 1\nbend s\nret x\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto result = ::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (count (f, Opcode::Alloca) == 1);
  CHECK (count (f, Opcode::BStart) == 1);
  CHECK (count (f, Opcode::BEnd) == 1);
  CHECK (count (f, Opcode::Call) == 1);
  CHECK (count (f, Opcode::Add) == 1);
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
TEST_CASE ("SSA dead code preserves overflow flags across only the permitted instructions") {
  for (auto op : {"addo", "addos", "subo", "subos", "mulo", "mulos", "umulo", "umulos"})
    for (auto branch : {"bo", "ubo", "bno", "ubno"}) {
      bool unsigned_branch
        = std::string_view (branch) == "ubo" || std::string_view (branch) == "ubno";
      if ((std::string_view (op).starts_with ("umulo") && !unsigned_branch)
          || (std::string_view (op).starts_with ("mulo") && unsigned_branch))
        continue;
      auto c = parse ("m: module\nentry: func i64, i64:x\nlocal i64:a, i64:b\n" + std::string (op)
                      + " a, x, 3\nmov b, x\n" + branch
                      + " yes\nret 0\nyes:\nret b\nendfunc\nendmodule\n");
      auto &f = entry (*c);
      FunctionAnalysisManager a (f);
      prepare (f, a);
      auto result = ::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a);
      REQUIRE ((result).has_value ());
      bool found = false;
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ())
          found |= instruction_descriptor (i.opcode ()).get ()->name == op;
      CHECK (found);
      REQUIRE ((f.verify ()).has_value ());
      finish (*c);
    }
}
TEST_CASE ("SSA dead code verifies every public opcode and fails invalid phases before edits") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto c = test::public_instruction_example (descriptor);
    auto &f = entry (*c);
    FunctionAnalysisManager a (f);
    prepare (f, a, true);
    auto result = ::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a);
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    REQUIRE ((f.verify ()).has_value ());
    finish (*c);
  }
  auto c = parse (test::memory_fixtures ()[0]), foreign = parse (test::memory_fixtures ()[0]);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f), other (entry (*foreign));
  auto before = dump (f);
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, a);
  before = dump_ssa (f);
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, other)).has_value ());
  CHECK (dump_ssa (f) == before);
  FunctionPassManager conventional;
  conventional.add<ConventionalSSAPass> ();
  REQUIRE ((conventional.run (f, a)).has_value ());
  before = dump_ssa (f);
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a)).has_value ());
  CHECK (dump_ssa (f) == before);
}
TEST_CASE ("loop motion and pressure relief preserve ownership snapshots and round trips") {
  auto inputs = test::motion_fixtures ();
  auto random = test::random_motion_fixtures ();
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs) {
    CAPTURE (text);
    auto source = parse (text);
    auto original = dump (*source);
    auto copy = clone (*source);
    REQUIRE ((copy).has_value ());
    auto &f = entry ((copy).value ());
    FunctionAnalysisManager a (f);
    prepare (f, a, true);
    std::set<const Instruction *> live;
    FunctionPassManager passes;
    passes.add<SSADeadCodePass> ();
    passes.add<LICMPass> ();
    passes.add<PressureReliefPass> ();
    auto result = passes.run (f, a, [&] (const PassEvent &event) -> Result<void> {
      if (event.phase == PassEvent::Phase::After) {
        REQUIRE ((event.function.verify ()).has_value ());
        std::set<const Instruction *> current;
        for (auto &b : event.function.blocks ())
          for (auto &i : b.instructions ()) current.insert (&i);
        if (event.pass == "ssa-dce")
          live = std::move (current);
        else
          CHECK (current == live);
      }
      return {};
    });
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    CHECK (dump (*source) == original);
    CHECK_FALSE ((serialization::write_binary ((copy).value ())).has_value ());
    source.reset ();
    finish ((copy).value ());
  }
}
TEST_CASE ("LICM hoists multiplication but excludes division memory and hardware bindings") {
  for (auto op : {"mul", "div", "mov"}) {
    auto c = parse (
      "m: module\nentry: func i64, i64:x, i64:y, i64:p\nlocal i64:i, i64:a, i64:sum\nmov i, 0\nmov "
      "sum, 0\nloop:\n"
      + (std::string_view (op) == "mov" ? "mov a, i64:0(p)\n" : std::string (op) + " a, x, y\n")
      + "add sum, sum, a\nadd i, i, 1\nblt loop, i, 4\nret sum\nendfunc\nendmodule\n");
    auto &f = entry (*c);
    FunctionAnalysisManager a (f);
    prepare (f, a);
    auto loop = &f.find_label ("loop").get ()->block ();
    const Instruction *candidate = &*loop->instructions ().begin ();
    auto result = ::mirnext::transforms::detail::run_pass (LICMPass (), f, a);
    REQUIRE ((result).has_value ());
    CHECK ((&candidate->block () != loop) == (std::string_view (op) == "mul"));
    REQUIRE ((f.verify ()).has_value ());
    finish (*c);
  }
}
TEST_CASE ("pressure relief places a single-use literal after PHIs near its user") {
  auto c = parse (
    "m: module\nentry: func i64, i64:x\nlocal i64:a, i64:b\nmov a, 42\nbt yes, x\nret x\nyes:\nadd "
    "b, a, x\nret b\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  const Instruction *literal = &*f.blocks ().begin ()->instructions ().begin ();
  auto old = &literal->block ();
  auto result = ::mirnext::transforms::detail::run_pass (PressureReliefPass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().transformations == 1);
  CHECK (&literal->block () != old);
  CHECK (&literal->block () == &f.find_label ("yes").value ().block ());
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
TEST_CASE ("motion rewriter rejects invalid anchors phases and owners before mutation") {
  auto text
    = "m: module\nentry: func i64, i64:x\nlocal i64:a\nadd a, x, 1\nbt yes, x\nret a\nyes:\nret "
      "x\nendfunc\nendmodule\n";
  auto c = parse (text), foreign = parse (text);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f), other (entry (*foreign));
  auto before = dump (f);
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (LICMPass (), f, a)).has_value ());
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (PressureReliefPass (), f, a)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, a);
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  before = dump_ssa (f);
  auto blocks = f.blocks ();
  auto nonempty = std::find_if (blocks.begin (), blocks.end (),
                                [] (auto &block) { return block.size () != 0; });
  REQUIRE (nonempty != blocks.end ());
  auto &first = *nonempty;
  auto &i = *first.instructions ().begin ();
  auto &yes = const_cast<BasicBlock &> (f.find_label ("yes").get ()->block ());
  CHECK_FALSE (
    (rewriter.value ().move_before (i, *entry (*foreign).blocks ().begin ())).has_value ());
  // Simplify may move the final return out of the labeled block, leaving
  // "yes" as an empty fallthrough. Select a real foreign-block anchor.
  const Instruction *other_anchor = nullptr;
  for (auto &block : f.blocks ())
    if (&block != &first && block.instructions ().begin () != block.instructions ().end ()) {
      other_anchor = &*block.instructions ().begin ();
      break;
    }
  REQUIRE (other_anchor);
  CHECK_FALSE ((rewriter.value ().move_before (i, first, other_anchor)).has_value ());
  CHECK_FALSE ((rewriter.value ().move_before (i, yes, &i)).has_value ());
  CHECK_FALSE (
    (rewriter.value ().move_before (*const_cast<Instruction *> (first.terminator ().get ()), yes))
      .has_value ());
  CHECK_FALSE ((rewriter.value ().split_fallthrough (yes, first)).has_value ());
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (LICMPass (), f, other)).has_value ());
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (PressureReliefPass (), f, other)).has_value ());
  CHECK (dump_ssa (f) == before);
  REQUIRE ((f.verify ()).has_value ());
}
TEST_CASE ("SSA inverse use order survives unrelated deletion and chain transfers") {
  auto c = parse (
    "m: module\nentry: func i64, i64:x\nlocal i64:z, i64:a, i64:b, i64:c, i64:d\nadd z, x, 7\nadd "
    "a, x, 1\nadd b, a, 2\nadd c, a, 3\nadd d, a, 4\nret a\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto form = f.ssa_form ().get ();
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  auto definition = [&] (std::string_view name) -> Instruction & {
    for (auto &block : f.blocks ())
      for (auto &instruction : block.instructions ())
        for (auto value :
             mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (instruction)))
          if (value->origin ().name () == name) return instruction;
    FAIL ("missing named definition");
    return *f.blocks ().begin ()->instructions ().begin ();
  };
  auto &z = definition ("z"), &producer = definition ("a"), &b = definition ("b"),
       &erased = definition ("c");
  auto value = mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (producer))[0],
       target = mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (z))[0];
  std::vector<SSAUse *>
    original (mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).begin (),
              mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).end ());
  std::erase_if (original, [&] (auto use) {
    auto site = std::get_if<SSAInstructionUse> (&use->site ());
    return site && mirnext::ir::detail::NodeAccess::instruction (*site) == &erased;
  });
  REQUIRE ((rewriter.value ().erase_instruction (erased)).has_value ());
  CHECK (
    std::vector<SSAUse *> (mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).begin (),
                           mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).end ())
    == original);
  auto use = mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (b)).front ();
  REQUIRE ((rewriter.value ().retarget (*use, *target)).has_value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (target->uses ()).front () == use);
  std::vector<SSAUse *>
    remainder (mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).begin (),
               mirnext::ir::detail::ReferenceAccess::pointers (value->uses ()).end ());
  REQUIRE ((rewriter.value ().replace_all_uses (*value, *target)).has_value ());
  remainder.insert (remainder.begin (), use);
  CHECK (
    std::vector<SSAUse *> (mirnext::ir::detail::ReferenceAccess::pointers (target->uses ())
                             .begin (),
                           mirnext::ir::detail::ReferenceAccess::pointers (target->uses ()).end ())
    == remainder);
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
TEST_CASE ("property branches remain control flow during canonical loop motion") {
  for (auto branch : {"prbeq", "prbne"}) {
    auto c = parse (
      "m: module\nentry: func i64, i64:x, i64:y\nlocal i64:i, i64:a, i64:sum\nmov i, 0\nmov sum, "
      "0\nprset x, 1\n"
      + std::string (branch) + " done, x, 0\nloop:\nmul a, x, y\nadd sum, sum, a\n" + branch
      + " done, x, 1\nadd i, i, 1\nblt loop, i, 4\ndone:\nret sum\nendfunc\nendmodule\n");
    auto &f = entry (*c);
    FunctionAnalysisManager a (f);
    prepare (f, a);
    std::vector<const Instruction *> branches;
    std::vector<const BasicBlock *> blocks;
    for (auto &block : f.blocks ())
      for (auto &i : block.instructions ())
        if (i.opcode () == Opcode::PrBeq || i.opcode () == Opcode::PrBne) {
          branches.push_back (&i);
          blocks.push_back (&block);
        }
    REQUIRE ((::mirnext::transforms::detail::run_pass (LICMPass (), f, a)).has_value ());
    REQUIRE (branches.size () == 2);
    for (std::size_t p = 0; p < branches.size (); ++p) CHECK (&branches[p]->block () == blocks[p]);
    finish (*c);
  }
}
TEST_CASE ("LICM reacquires an independently consumed graph and supports repeated passes") {
  auto c = parse (test::motion_fixtures ()[0]);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto loops = a.get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  auto consumed = a.take_cached<FunctionGraphAnalysis> ();
  REQUIRE ((consumed).has_value ());
  REQUIRE (consumed.value ().has_value ());
  auto result = ::mirnext::transforms::detail::run_pass (LICMPass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().transformations > 0);
  auto before = dump_ssa (f);
  result = ::mirnext::transforms::detail::run_pass (LICMPass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().transformations == 0);
  CHECK (dump_ssa (f) == before);
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
TEST_CASE ("LICM preheader graphs retain only live edges and valid dominance") {
  auto c = parse (test::motion_fixtures ()[2]);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto blocks = std::ranges::distance (f.blocks ());
  REQUIRE ((::mirnext::transforms::detail::run_pass (LICMPass (), f, a)).has_value ());
  CHECK (std::ranges::distance (f.blocks ()) == blocks + 1);
  auto graph = a.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  for (std::size_t id = 0; id < graph.value ().edges.size (); ++id) {
    auto &edge = graph.value ().edges[id];
    CHECK (std::ranges::count (graph.value ().outgoing[edge.source], id) == 1);
    CHECK (std::ranges::count (graph.value ().incoming[edge.destination], id) == 1);
  }
  FunctionAnalysisManager fresh (f);
  auto expected = fresh.get<DominatorAnalysis> ();
  REQUIRE ((expected).has_value ());
  auto actual = a.get<DominatorAnalysis> ();
  REQUIRE ((actual).has_value ());
  for (std::size_t from = 0; from < graph.value ().blocks.size (); ++from)
    for (std::size_t to = 0; to < graph.value ().blocks.size (); ++to)
      CHECK (actual.value ().dominates (from, to) == expected.value ().dominates (from, to));
  finish (*c);
}
TEST_CASE ("dead SSA removes wide PHIs but retains cyclic PHI uses") {
  std::string text = "m: module\nentry: func i64, i64:x\nlocal i64:a, i64:b\nswitch x";
  for (unsigned n = 0; n < 12; ++n) text += ", b" + std::to_string (n);
  for (unsigned n = 0; n < 12; ++n)
    text += "\nb" + std::to_string (n) + ":\nadd a, x, " + std::to_string (n) + "\njmp join";
  text += "\njoin:\nadd b, a, a\nret x\nendfunc\nendmodule\n";
  auto c = parse (text);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  bool wide = false;
  for (auto &phi : f.ssa_form ().get ()->phis ())
    if (phi.alive ()
        && mirnext::ir::detail::ReferenceAccess::pointers (phi.inputs ()).size () == 12) {
      wide = true;
      auto before = dump_ssa (f);
      CHECK_FALSE ((rewriter.value ().erase_phi (const_cast<SSAPhi &> (phi))).has_value ());
      CHECK (dump_ssa (f) == before);
    }
  REQUIRE (wide);
  auto result = ::mirnext::transforms::detail::run_pass (SSADeadCodePass (), f, a);
  REQUIRE ((result).has_value ());
  CHECK (count (f, Opcode::Add) == 0);
  finish (*c);
  c = parse (
    "m: module\nentry: func i64, i64:x\nlocal i64:a\nmov a, 0\nloop:\nadd a, a, x\nbt loop, x\nret "
    "x\nendfunc\nendmodule\n");
  auto &cycle = entry (*c);
  FunctionAnalysisManager ca (cycle);
  prepare (cycle, ca);
  REQUIRE ((::mirnext::transforms::detail::run_pass (SSADeadCodePass (), cycle, ca)).has_value ());
  bool live_phi = false;
  for (auto &phi : cycle.ssa_form ().get ()->phis ()) live_phi |= phi.alive ();
  CHECK (live_phi);
  CHECK (count (cycle, Opcode::Add) == 1);
  finish (*c);
}
