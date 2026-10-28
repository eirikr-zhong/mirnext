/* Licensed under the MIT License. SSA ownership and lifecycle checks. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../Analysis/Fixtures.hpp"
#include <unordered_set>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
std::unique_ptr<Program> parse (std::string_view text) {
  auto parsed = serialization::parse_text (text);
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  return std::make_unique<Program> (std::move ((parsed).value ()));
}
Function &entry (Program &context) { return *context.modules ().begin ()->functions ().begin (); }
void prepare (Function &function, FunctionAnalysisManager &analyses, bool prune = true) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> (prune);
  auto result = passes.run (function, analyses);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
}
void build (Function &function, FunctionAnalysisManager &analyses, bool rename = true) {
  FunctionPassManager passes;
  passes.add<BuildSSAPass> (rename);
  auto result = passes.run (function, analyses);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  REQUIRE ((function.ssa_form ()).has_value ());
  REQUIRE ((function.verify ()).has_value ());
}
void finish (Function &function, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes;
  if (function.ssa_form ().get ()->renamed ()) passes.add<ConventionalSSAPass> ();
  passes.add<DestroySSAPass> ();
  auto result = passes.run (function, analyses);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  CHECK (function.ssa_form ().get () == nullptr);
  REQUIRE ((function.verify ()).has_value ());
}
std::size_t phi_count (const Function &function) {
  std::size_t count = 0;
  for (auto &phi : function.ssa_form ().get ()->phis ()) count += phi.alive ();
  return count;
}
}  // namespace
TEST_CASE (
  "native SSA owns PHIs while borrowing canonical instructions and survives cache clearing") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses);
  std::unordered_set<const Instruction *> identities;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) identities.insert (&i);
  auto graph = analyses.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto dom = analyses.get<DominatorAnalysis> ();
  REQUIRE ((dom).has_value ());
  auto graph_identity = &(graph).value ();
  auto dom_identity = &(dom).value ();
  build (f, analyses);
  CHECK (phi_count (f) > 0);
  CHECK (&(analyses.get<FunctionGraphAnalysis> ()).value () == graph_identity);
  CHECK (&(analyses.get<DominatorAnalysis> ()).value () == dom_identity);
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) CHECK (identities.contains (&i));
  auto form = f.ssa_form ().get ();
  analyses.clear ();
  CHECK (f.ssa_form ().get () == form);
  REQUIRE ((form->verify ()).has_value ());
  CHECK (dump (f).find ("SSA renamed=1") != std::string::npos);
  CHECK (dump (*context).find ("SSA renamed=1") != std::string::npos);
  CHECK_FALSE ((serialization::write_text (*context)).has_value ());
  CHECK_FALSE ((serialization::write_binary (*context)).has_value ());
  CHECK_FALSE ((clone (*context)).has_value ());
  auto &instruction = *f.blocks ().begin ()->instructions ().begin ();
  auto before = dump (f);
  CHECK_FALSE ((InstructionRewriter (f).erase (instruction)).has_value ());
  CHECK_FALSE ((InstructionRewriter (f).erase_unused_labels ()).has_value ());
  CHECK_FALSE ((InstructionBuilder (*f.blocks ().begin ())
                  .emit (instruction.opcode (), instruction.operands ()))
                 .has_value ());
  CHECK_FALSE ((mirnext::ir::detail::access (f).create_block ()).has_value ());
  CHECK_FALSE ((mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ())).has_value ());
  CHECK_FALSE ((mirnext::ir::detail::access (f).create_register (Type::i64 ())).has_value ());
  CHECK (dump (f) == before);
  CHECK_FALSE ((analyses.get<LivenessAnalysis> ()).has_value ());
  CHECK_FALSE ((analyses.get<RegisterUseAnalysis> ()).has_value ());
  DestroySSAPass early;
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (early, f, analyses)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  FunctionPassManager conventional;
  conventional.add<ConventionalSSAPass> ();
  REQUIRE ((conventional.run (f, analyses)).has_value ());
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  auto snapshot = dump (f);
  for (auto &phi : f.ssa_form ().get ()->phis ())
    if (phi.alive ()) {
      CHECK_FALSE ((rewriter.value ().rename (const_cast<SSAValue &> (phi.output ()),
                                              *mirnext::ir::detail::ReferenceAccess::pointers (
                                                f.arguments ())[0]))
                     .has_value ());
      break;
    }
  CHECK (dump (f) == snapshot);
  REQUIRE ((f.verify ()).has_value ());
  finish (f, analyses);
  auto text = serialization::write_text (*context);
  REQUIRE ((text).has_value ());
  auto binary = serialization::write_binary (*context);
  REQUIRE ((binary).has_value ());
  REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
}
TEST_CASE ("MIR maximal SSA minimization and conventional copies cover fixed and seeded CFGs") {
  auto inputs = test::analysis_fixtures ();
  auto random = test::random_analysis_fixtures ();
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs)
    for (bool rename : {false, true}) {
      CAPTURE (text);
      CAPTURE (rename);
      auto context = parse (text);
      auto &f = entry (*context);
      FunctionAnalysisManager analyses (f);
      prepare (f, analyses);
      auto before = serialization::write_text (*context);
      REQUIRE ((before).has_value ());
      build (f, analyses, rename);
      auto graph = analyses.get<FunctionGraphAnalysis> ();
      REQUIRE ((graph).has_value ());
      auto dom = analyses.get<DominatorAnalysis> ();
      REQUIRE ((dom).has_value ());
      auto form = f.ssa_form ().get ();
      for (auto &use : form->uses ())
        if (use.alive ()) {
          std::size_t definition = FunctionGraph::entry, target;
          if (use.value ().instruction ().get ())
            definition = mirnext::analysis::detail::NodeAccess::indices (((graph)).value ())
                           .at (&use.value ().instruction ().get ()->block ());
          else if (use.value ().phi ().get ())
            definition = mirnext::analysis::detail::NodeAccess::indices (((graph)).value ())
                           .at (&use.value ().phi ().get ()->block ());
          if (auto site = std::get_if<SSAPhiUse> (&use.site ())) {
            auto predecessor = mirnext::ir::detail::ReferenceAccess::pointers (
              mirnext::ir::detail::NodeAccess::phi (*(site))->predecessors ())[site->input];
            target = predecessor
                       ? mirnext::analysis::detail::NodeAccess::indices (((graph)).value ())
                           .at (predecessor)
                       : FunctionGraph::entry;
          } else
            target = mirnext::analysis::detail::NodeAccess::indices (((graph)).value ())
                       .at (form->use_block (use).get ());
          CHECK (dom.value ().dominates (definition, target));
        }
      finish (f, analyses);
      if (!rename) {
        auto after = serialization::write_text (*context);
        REQUIRE ((after).has_value ());
        CHECK ((after).value () == (before).value ());
      }
      auto cloned = clone (*context);
      REQUIRE ((cloned).has_value ());
      REQUIRE ((cloned.value ().verify ()).has_value ());
    }
}
TEST_CASE ("SSA rewrites both memory components and batched repeated register uses") {
  auto context = parse (R"(m: module
entry: func i64
local i64:a, i64:b, i64:c, i64:d
mov a, 16
mov b, 32
mov c, i64:0(a, a, 1)
add d, a, a
ret d
endfunc
endmodule
)");
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  build (f, analyses);
  auto &block = *f.blocks ().begin ();
  auto iterator = block.instructions ().begin ();
  auto &a = *iterator++;
  auto &b = *iterator++;
  auto &load = *iterator++;
  auto &add = *iterator++;
  auto av
    = mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->definitions (a))[0],
    bv = mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->definitions (b))[0];
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  CHECK_FALSE ((rewriter.value ().erase_instruction (a)).has_value ());
  auto aliased = mirnext::ir::detail::ReferenceAccess::pointers (av->uses ());
  REQUIRE ((rewriter.value ().replace_uses (aliased, *bv)).has_value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (av->uses ()).empty ());
  REQUIRE ((f.verify ()).has_value ());
  REQUIRE ((rewriter.value ().replace_all_uses (*bv, *bv)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  auto memory = mirnext::ir::detail::operand_access (load.operands ()[1]).get<Memory> ();
  REQUIRE (memory);
  CHECK (mirnext::ir::detail::NodeAccess::base (*(memory)) == &bv->reg ());
  CHECK (mirnext::ir::detail::NodeAccess::index (*(memory)) == &bv->reg ());
  CHECK (*mirnext::ir::detail::operand_access (add.operands ()[1]).get<const Register *> ()
         == &bv->reg ());
  CHECK (*mirnext::ir::detail::operand_access (add.operands ()[2]).get<const Register *> ()
         == &bv->reg ());
  REQUIRE ((rewriter.value ().erase_instruction (a)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  finish (f, analyses);
}
TEST_CASE ("hardware definitions keep one register identity and retain return effects") {
  auto text = test::analysis_fixtures ()[8];
  text.insert (text.find ("mov bound, 13"), "mov bound, 7\n");
  auto context = parse (text);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses);
  build (f, analyses);
  auto hardware = f.find_register ("bound").get ();
  REQUIRE (hardware);
  std::size_t bindings = 0, definitions = 0;
  for (auto &reg : f.registers ()) bindings += !reg.hardware_binding ().empty ();
  CHECK (bindings == 1);
  for (auto &value : f.ssa_form ().get ()->values ())
    if (value.alive () && value.origin ().hardware_binding () == hardware->hardware_binding ()) {
      CHECK (&value.reg () == hardware);
      ++definitions;
    }
  CHECK (definitions >= 2);
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  CHECK (&(rewriter.value ().fresh_register (*hardware)).value () == hardware);
  for (auto &use : f.ssa_form ().value ().uses ())
    if (use.alive () && std::holds_alternative<SSAHardwareUse> (use.site ()))
      CHECK (use.value ().reg ().hardware_binding () == hardware->hardware_binding ());
  finish (f, analyses);
}
TEST_CASE ("SSA verifies wide mixed-type calls and repeated memory input sites") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("wide");
  REQUIRE ((module).has_value ());
  Signature signature;
  for (unsigned n = 0; n < 257; ++n) {
    auto type = Type (n % 3 == 0   ? Type::Kind::F32
                      : n % 3 == 1 ? Type::Kind::F64
                                   : Type::Kind::I64);
    signature.results.push_back (type);
    signature.parameters.push_back (type);
  }
  auto target = mirnext::ir::detail::access (((module)).value ()).import ("target", signature);
  REQUIRE ((target).has_value ());
  auto function
    = mirnext::ir::detail::access (((module)).value ()).create_function ("entry", signature);
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder body ((block).value ());
  std::vector<mirnext::ir::detail::CallArgumentState> arguments;
  for (auto argument :
       mirnext::ir::detail::ReferenceAccess::pointers (function.value ().arguments ()))
    arguments.emplace_back (*argument);
  auto call = body.call ((target).value (), arguments);
  REQUIRE ((call).has_value ());
  auto address = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
  REQUIRE ((address).has_value ());
  auto loaded = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
  REQUIRE ((loaded).has_value ());
  REQUIRE ((body.assign ((address).value (), 16)).has_value ());
  auto load
    = body.emit (Opcode::Mov,
                 {Operand ((loaded).value ()),
                  Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                         &(address).value (),
                                                                         &(address).value ()))});
  REQUIRE ((load).has_value ());
  std::vector<Operand> returned;
  for (auto result : mirnext::ir::detail::ReferenceAccess::pointers (call.value ().results ()))
    returned.emplace_back (*result);
  REQUIRE ((body.emit (Opcode::Ret, returned)).has_value ());
  FunctionAnalysisManager analyses ((function).value ());
  build ((function).value (), analyses);
  auto form = function.value ().ssa_form ().get ();
  CHECK (form->definitions (call.value ().instruction ()).size () == 257);
  CHECK (form->inputs (call.value ().instruction ()).size () == 257);
  CHECK (form->inputs ((load).value ()).size () == 2);
  // Verify after edits, not only after construction, and keep base/index uses
  // distinct even when they have the same reaching definition.
  auto rewriter = SSAUpdater::get ((function).value ());
  REQUIRE ((rewriter).has_value ());
  for (auto definition : mirnext::ir::detail::ReferenceAccess::pointers (
         form->definitions (call.value ().instruction ()))) {
    auto fresh = rewriter.value ().fresh_register (definition->reg ());
    REQUIRE ((fresh).has_value ());
    REQUIRE ((rewriter.value ().rename (*definition, (fresh).value ())).has_value ());
  }
  REQUIRE ((form->verify ()).has_value ());
  REQUIRE ((function.value ().verify ()).has_value ());
  auto use = mirnext::ir::detail::ReferenceAccess::pointers (form->inputs ((load).value ()))[0];
  CHECK_FALSE ((rewriter.value ().input ((load).value (), 1, static_cast<SSAOperandPart> (64),
                                         const_cast<SSAValue &> (use->value ())))
                 .has_value ());
  REQUIRE ((form->verify ()).has_value ());
  // A damaged duplicate base edge must not conceal the missing index edge.
  auto &site = const_cast<SSAUseSite &> (
    mirnext::ir::detail::ReferenceAccess::pointers (form->inputs ((load).value ()))[1]->site ());
  auto saved = site;
  std::get<SSAInstructionUse> (site).part = SSAOperandPart::MemoryBase;
  CHECK_FALSE ((form->verify ()).has_value ());
  site = saved;
  REQUIRE ((form->verify ()).has_value ());
  finish ((function).value (), analyses);
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("closed uninitialized predecessor cycles use explicit undefined definitions") {
  auto context = parse (test::analysis_fixtures ()[6]);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, false);
  build (f, analyses, false);
  bool undefined = false;
  for (auto &value : f.ssa_form ().get ()->values ())
    if (value.alive () && value.kind () == SSAValue::Kind::Undefined
        && !mirnext::ir::detail::ReferenceAccess::pointers (value.uses ()).empty ())
      undefined = true;
  CHECK (undefined);
  finish (f, analyses);
}
TEST_CASE ("deep forwarding chains build SSA without recursive predecessor lookup") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("deep");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((function).has_value ());
  for (unsigned n = 0; n < 12000; ++n) {
    auto b = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((b).has_value ());
    REQUIRE ((mirnext::ir::detail::access (((function)).value ()).create_label ((b).value ()))
               .has_value ());
  }
  REQUIRE ((InstructionBuilder (*std::prev (function.value ().blocks ().end ()))
              .ret (Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                function.value ().arguments ())[0])))
             .has_value ());
  FunctionAnalysisManager analyses ((function).value ());
  build ((function).value (), analyses, false);
  CHECK (phi_count ((function).value ()) == 0);
  finish ((function).value (), analyses);
}
TEST_CASE ("SSA rewrite errors are atomic and observer failures retain an owned valid phase") {
  auto first = parse (test::analysis_fixtures ()[0]),
       second = parse (test::analysis_fixtures ()[0]);
  auto &f = entry (*first), &other = entry (*second);
  FunctionAnalysisManager analyses (f), other_analyses (other);
  prepare (f, analyses);
  prepare (other, other_analyses);
  build (other, other_analyses);
  FunctionPassManager passes;
  passes.add<BuildSSAPass> ();
  auto observed = passes.run (f, analyses, [] (const PassEvent &event) -> Result<void> {
    if (event.phase == PassEvent::Phase::After)
      return Error{ErrorCode::InvalidArgument, "observer failure"};
    return {};
  });
  REQUIRE_FALSE ((observed).has_value ());
  CHECK (observed.error ().message.find ("m/entry [build-ssa]") != std::string::npos);
  REQUIRE ((f.ssa_form ()).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  auto before = dump (f);
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  auto value = const_cast<SSAValue *> (&*f.ssa_form ().get ()->values ().begin ());
  auto foreign = const_cast<SSAValue *> (&*other.ssa_form ().get ()->values ().begin ());
  CHECK_FALSE ((rewriter.value ().replace_all_uses (*value, *foreign)).has_value ());
  CHECK_FALSE ((rewriter.value ().rename (*value, foreign->reg ())).has_value ());
  CHECK_FALSE ((rewriter.value ().fresh_register (foreign->reg ())).has_value ());
  CHECK_FALSE (
    (rewriter.value ().erase_instruction (*other.blocks ().begin ()->instructions ().begin ()))
      .has_value ());
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (BuildSSAPass (), f, analyses)).has_value ());
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (BuildSSAPass (), f, other_analyses)).has_value ());
  CHECK (dump (f) == before);
  REQUIRE ((f.verify ()).has_value ());
  finish (f, analyses);
  finish (other, other_analyses);
  build (f, analyses);
  finish (f, analyses);
  REQUIRE ((first->verify ()).has_value ());
}
