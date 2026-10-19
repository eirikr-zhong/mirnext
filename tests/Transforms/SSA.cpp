/* Licensed under the MIT License. SSA ownership and lifecycle checks require no C oracle. */
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
std::unique_ptr<Context> parse(std::string_view text) {
  auto parsed = serialization::parse_text(text); INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed); return std::move(*parsed);
}
Function &entry(Context &context) { return *context.modules().begin()->functions().begin(); }
void prepare(Function &function, FunctionAnalysisManager &analyses, bool prune = true) {
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(prune);
  auto result = passes.run(function, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
}
void build(Function &function, FunctionAnalysisManager &analyses, bool rename = true) {
  FunctionPassManager passes; passes.add<BuildSSAPass>(rename);
  auto result = passes.run(function, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
  REQUIRE(function.ssa_form()); REQUIRE(function.verify());
}
void finish(Function &function, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes;
  if (function.ssa_form()->renamed()) passes.add<ConventionalSSAPass>();
  passes.add<DestroySSAPass>(); auto result = passes.run(function, analyses);
  INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(function.ssa_form() == nullptr); REQUIRE(function.verify());
}
std::size_t phi_count(const Function &function) {
  std::size_t count = 0; for (auto &phi : function.ssa_form()->phis()) count += phi.alive(); return count;
}
}
TEST_CASE("native SSA owns PHIs while borrowing canonical instructions and survives cache clearing") {
  auto context = parse(test::analysis_fixtures()[0]); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
  std::unordered_set<const Instruction *> identities;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) identities.insert(&i);
  auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto dom = analyses.get<DominatorAnalysis>(); REQUIRE(dom);
  auto graph_identity = *graph; auto dom_identity = *dom; build(f, analyses);
  CHECK(phi_count(f) > 0); CHECK(*analyses.get<FunctionGraphAnalysis>() == graph_identity); CHECK(*analyses.get<DominatorAnalysis>() == dom_identity);
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) CHECK(identities.contains(&i));
  auto form = f.ssa_form(); analyses.clear(); CHECK(f.ssa_form() == form); REQUIRE(form->verify());
  CHECK(dump(f).find("SSA renamed=1") != std::string::npos);
  CHECK(dump(*context).find("SSA renamed=1") != std::string::npos);
  CHECK_FALSE(serialization::write_text(*context)); CHECK_FALSE(serialization::write_binary(*context)); CHECK_FALSE(clone(*context));
  auto &instruction = *f.blocks().begin()->instructions().begin(); auto before = dump(f);
  CHECK_FALSE(IRRewriter(f).erase(instruction)); CHECK_FALSE(IRRewriter(f).erase_unused_labels());
  CHECK_FALSE(IRBuilder(*f.blocks().begin()).emit(instruction.opcode(), instruction.operands()));
  CHECK_FALSE(f.create_block()); CHECK_FALSE(f.create_label(*f.blocks().begin())); CHECK_FALSE(f.create_register(Type::i64()));
  CHECK(dump(f) == before); CHECK_FALSE(analyses.get<LivenessAnalysis>()); CHECK_FALSE(analyses.get<RegisterUseAnalysis>());
  DestroySSAPass early; CHECK_FALSE(early.run(f, analyses)); REQUIRE(f.verify());
  FunctionPassManager conventional; conventional.add<ConventionalSSAPass>(); REQUIRE(conventional.run(f, analyses));
  auto rewriter = SSARewriter::get(f); REQUIRE(rewriter); auto snapshot = dump(f);
  for (auto &phi : f.ssa_form()->phis()) if (phi.alive()) {
    CHECK_FALSE(rewriter->rename(const_cast<SSAValue &>(phi.output()), *f.arguments()[0]));
    break;
  }
  CHECK(dump(f) == snapshot); REQUIRE(f.verify());
  finish(f, analyses); auto text = serialization::write_text(*context); REQUIRE(text);
  auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
}
TEST_CASE("MIR maximal SSA minimization and conventional copies cover fixed and seeded CFGs") {
  auto inputs = test::analysis_fixtures(); auto random = test::random_analysis_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (bool rename : {false, true}) {
    CAPTURE(text); CAPTURE(rename); auto context = parse(text); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
    auto before = serialization::write_text(*context); REQUIRE(before); build(f, analyses, rename);
    auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto dom = analyses.get<DominatorAnalysis>(); REQUIRE(dom);
    auto form = f.ssa_form();
    for (auto &use : form->uses()) if (use.alive()) {
      std::size_t definition = FunctionGraph::entry, target;
      if (use.value().instruction()) definition = (*graph)->indices.at(&use.value().instruction()->block());
      else if (use.value().phi()) definition = (*graph)->indices.at(&use.value().phi()->block());
      if (auto site = std::get_if<SSAPhiUse>(&use.site())) {
        auto predecessor = site->phi->predecessors()[site->input]; target = predecessor ? (*graph)->indices.at(predecessor) : FunctionGraph::entry;
      } else target = (*graph)->indices.at(form->use_block(use));
      CHECK((*dom)->dominates(definition, target));
    }
    finish(f, analyses);
    if (!rename) { auto after = serialization::write_text(*context); REQUIRE(after); CHECK(*after == *before); }
    auto cloned = clone(*context); REQUIRE(cloned); REQUIRE((*cloned)->verify());
  }
}
TEST_CASE("SSA rewrites both memory components and batched repeated register uses") {
  auto context = parse(R"(m: module
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
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); build(f, analyses);
  auto &block = *f.blocks().begin(); auto iterator = block.instructions().begin(); auto &a = *iterator++; auto &b = *iterator++;
  auto &load = *iterator++; auto &add = *iterator++;
  auto av = f.ssa_form()->definitions(a)[0], bv = f.ssa_form()->definitions(b)[0];
  auto rewriter = SSARewriter::get(f); REQUIRE(rewriter); CHECK_FALSE(rewriter->erase_instruction(a));
  auto aliased = av->uses(); REQUIRE(rewriter->replace_uses(aliased, *bv)); CHECK(av->uses().empty()); REQUIRE(f.verify());
  REQUIRE(rewriter->replace_all_uses(*bv, *bv)); REQUIRE(f.verify());
  auto memory = load.operands()[1].get<Memory>(); REQUIRE(memory); CHECK(memory->base == &bv->reg()); CHECK(memory->index == &bv->reg());
  CHECK(*add.operands()[1].get<const Register *>() == &bv->reg()); CHECK(*add.operands()[2].get<const Register *>() == &bv->reg());
  REQUIRE(rewriter->erase_instruction(a)); REQUIRE(f.verify()); finish(f, analyses);
}
TEST_CASE("hardware definitions keep one register identity and retain return effects") {
  auto text = test::analysis_fixtures()[8]; text.insert(text.find("mov bound, 13"), "mov bound, 7\n");
  auto context = parse(text); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses); build(f, analyses);
  auto hardware = f.find_register("bound"); REQUIRE(hardware); std::size_t bindings = 0, definitions = 0;
  for (auto &reg : f.registers()) bindings += !reg.hardware_binding().empty(); CHECK(bindings == 1);
  for (auto &value : f.ssa_form()->values()) if (value.alive() && value.origin().hardware_binding() == hardware->hardware_binding()) {
    CHECK(&value.reg() == hardware); ++definitions;
  }
  CHECK(definitions >= 2); auto rewriter = SSARewriter::get(f); REQUIRE(rewriter); CHECK(*rewriter->fresh_register(*hardware) == hardware);
  for (auto &use : f.ssa_form()->uses()) if (use.alive() && std::holds_alternative<SSAHardwareUse>(use.site())) CHECK(use.value().reg().hardware_binding() == hardware->hardware_binding());
  finish(f, analyses);
}
TEST_CASE("SSA verifies wide mixed-type calls and repeated memory input sites") {
  Context context; auto module = context.create_module("wide"); REQUIRE(module);
  Signature signature;
  for (unsigned n = 0; n < 257; ++n) {
    auto type = Type(n % 3 == 0 ? Type::Kind::F32 : n % 3 == 1 ? Type::Kind::F64 : Type::Kind::I64);
    signature.results.push_back(type); signature.parameters.push_back(type);
  }
  auto target = (*module)->import("target", signature); REQUIRE(target);
  auto function = (*module)->create_function("entry", signature); REQUIRE(function);
  auto block = (*function)->create_block(); REQUIRE(block); IRBuilder body(**block);
  std::vector<CallArgument> arguments;
  for (auto argument : (*function)->arguments()) arguments.emplace_back(*argument);
  auto call = body.call(**target, arguments); REQUIRE(call);
  auto address = (*function)->create_register(Type::i64()); REQUIRE(address);
  auto loaded = (*function)->create_register(Type::i64()); REQUIRE(loaded);
  REQUIRE(body.assign(**address, 16));
  auto load = body.emit(Opcode::Mov, {Operand(**loaded), Operand(Memory{Type::i64(), *address, *address})}); REQUIRE(load);
  std::vector<Operand> returned;
  for (auto result : call->results()) returned.emplace_back(*result);
  REQUIRE(body.emit(Opcode::Ret, returned));
  FunctionAnalysisManager analyses(**function); build(**function, analyses);
  auto form = (*function)->ssa_form(); CHECK(form->definitions(call->instruction()).size() == 257);
  CHECK(form->inputs(call->instruction()).size() == 257);
  CHECK(form->inputs(**load).size() == 2);
  // Verify after edits, not only after construction, and keep base/index uses
  // distinct even when they have the same reaching definition.
  auto rewriter = SSARewriter::get(**function); REQUIRE(rewriter);
  for (auto definition : form->definitions(call->instruction())) {
    auto fresh = rewriter->fresh_register(definition->reg()); REQUIRE(fresh);
    REQUIRE(rewriter->rename(*definition, **fresh));
  }
  REQUIRE(form->verify()); REQUIRE((*function)->verify());
  auto use = form->inputs(**load)[0];
  CHECK_FALSE(rewriter->input(**load, 1, static_cast<SSAOperandPart>(64), const_cast<SSAValue &>(use->value())));
  REQUIRE(form->verify());
  // A damaged duplicate base edge must not conceal the missing index edge.
  auto &site = const_cast<SSAUseSite &>(form->inputs(**load)[1]->site());
  auto saved = site; std::get<SSAInstructionUse>(site).part = SSAOperandPart::MemoryBase;
  CHECK_FALSE(form->verify()); site = saved; REQUIRE(form->verify());
  finish(**function, analyses); REQUIRE(context.verify());
}
TEST_CASE("closed uninitialized predecessor cycles use explicit undefined definitions") {
  auto context = parse(test::analysis_fixtures()[6]); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses, false);
  build(f, analyses, false); bool undefined = false;
  for (auto &value : f.ssa_form()->values()) if (value.alive() && value.kind() == SSAValue::Kind::Undefined && !value.uses().empty()) undefined = true;
  CHECK(undefined); finish(f, analyses);
}
TEST_CASE("deep forwarding chains build SSA without recursive predecessor lookup") {
  Context context; auto module = context.create_module("deep"); REQUIRE(module);
  auto function = (*module)->create_function("entry", {{Type::i64()}, {Type::i64()}}); REQUIRE(function);
  for (unsigned n = 0; n < 12000; ++n) { auto b = (*function)->create_block(); REQUIRE(b); REQUIRE((*function)->create_label(**b)); }
  REQUIRE(IRBuilder(*std::prev((*function)->blocks().end())).ret(Operand(*(*function)->arguments()[0])));
  FunctionAnalysisManager analyses(**function); build(**function, analyses, false); CHECK(phi_count(**function) == 0); finish(**function, analyses);
}
TEST_CASE("SSA rewrite errors are atomic and observer failures retain an owned valid phase") {
  auto first = parse(test::analysis_fixtures()[0]), second = parse(test::analysis_fixtures()[0]);
  auto &f = entry(*first), &other = entry(*second); FunctionAnalysisManager analyses(f), other_analyses(other);
  prepare(f, analyses); prepare(other, other_analyses); build(other, other_analyses);
  FunctionPassManager passes; passes.add<BuildSSAPass>();
  auto observed = passes.run(f, analyses, [](const PassEvent &event) -> Result<void> {
    if (event.phase == PassEvent::Phase::After) return Error{ErrorCode::InvalidArgument, "observer failure"}; return {};
  });
  REQUIRE_FALSE(observed); CHECK(observed.error().message.find("m/entry [build-ssa]") != std::string::npos);
  REQUIRE(f.ssa_form()); REQUIRE(f.verify()); auto before = dump(f); auto rewriter = SSARewriter::get(f); REQUIRE(rewriter);
  auto value = const_cast<SSAValue *>(&*f.ssa_form()->values().begin());
  auto foreign = const_cast<SSAValue *>(&*other.ssa_form()->values().begin());
  CHECK_FALSE(rewriter->replace_all_uses(*value, *foreign)); CHECK_FALSE(rewriter->rename(*value, foreign->reg()));
  CHECK_FALSE(rewriter->fresh_register(foreign->reg()));
  CHECK_FALSE(rewriter->erase_instruction(*other.blocks().begin()->instructions().begin()));
  CHECK_FALSE(BuildSSAPass().run(f, analyses)); CHECK_FALSE(BuildSSAPass().run(f, other_analyses));
  CHECK(dump(f) == before); REQUIRE(f.verify()); finish(f, analyses); finish(other, other_analyses);
  build(f, analyses); finish(f, analyses); REQUIRE(first->verify());
}
