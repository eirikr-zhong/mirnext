/* Licensed under the MIT License. Node identity, structural edit boundaries and atomic diagnostics. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "PublicInstructions.hpp"
#include "Rewriter.hpp"
#include <array>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Context context; Module *module; Function *function; BasicBlock *entry; Register *value;
  Example() {
    auto m = context.create_module("rewriter"); REQUIRE(m); module = *m;
    auto f = module->create_function("entry", {{Type::i64()}, {}}); REQUIRE(f); function = *f;
    auto b = function->create_block("entry"); REQUIRE(b); entry = *b;
    auto r = function->create_register(Type::i64(), "value"); REQUIRE(r); value = *r;
  }
};
}
TEST_CASE("native rewriter insertion supports open blocks and preserves every existing node") {
  Example e; IRRewriter rewriter(*e.function);
  auto first = rewriter.insert(*e.entry, nullptr, Opcode::Mov, {Operand(*e.value), Operand(40)}); REQUIRE(first);
  auto before = rewriter.insert_before(**first, Opcode::Mov, {Operand(*e.value), Operand(0)}); REQUIRE(before);
  auto sum = rewriter.insert_after(**first, Opcode::Add, {Operand(*e.value), Operand(*e.value), Operand(2)}); REQUIRE(sum);
  auto ret = rewriter.insert_after(**sum, Opcode::Ret, {Operand(*e.value)}); REQUIRE(ret);
  auto inserted = rewriter.insert_after(**first, Opcode::Mov, {Operand(*e.value), Operand(*e.value)}); REQUIRE(inserted);
  const std::array expected{*before, *first, *inserted, *sum, *ret}; std::size_t n = 0;
  for (auto &instruction : e.entry->instructions()) CHECK(&instruction == expected[n++]);
  CHECK(n == expected.size()); REQUIRE(e.context.verify());
}
TEST_CASE("native rewriter rejects invalid insertion atomically and permits correction") {
  Example e; IRBuilder builder(*e.entry); auto first = builder.assign(*e.value, 42); REQUIRE(first);
  auto ret = builder.ret(Operand(*e.value)); REQUIRE(ret); IRRewriter rewriter(*e.function);
  auto other = e.function->create_block("other"); REQUIRE(other); auto label = e.function->create_label(**other); REQUIRE(label);
  REQUIRE(IRBuilder(**other).ret(Operand(0)));
  Example foreign; auto foreign_ret = IRBuilder(*foreign.entry).ret(Operand(0)); REQUIRE(foreign_ret);
  const auto revisions = e.function->revisions(); const auto original = dump(e.context);
  CHECK_FALSE(rewriter.insert(*e.entry, *foreign_ret, Opcode::Mov, {Operand(*e.value), Operand(1)}));
  CHECK_FALSE(rewriter.insert(*foreign.entry, nullptr, Opcode::Mov, {Operand(*e.value), Operand(1)}));
  CHECK_FALSE(rewriter.insert_before(**first, Opcode::Jmp, {Operand(**label)}));
  CHECK_FALSE(rewriter.insert_after(**first, Opcode::Ret, {Operand(0)}));
  CHECK_FALSE(rewriter.insert_after(**ret, Opcode::Mov, {Operand(*e.value), Operand(1)}));
  CHECK_FALSE(rewriter.insert(*e.entry, nullptr, Opcode::Mov, {Operand(*e.value), Operand(1)}));
  CHECK_FALSE(rewriter.insert_before(**ret, Opcode::Mov, {Operand(*e.value), Operand(1.0)}));
  CHECK_FALSE(rewriter.insert_before(**ret, static_cast<Opcode>(-1), {}));
  CHECK(e.function->revisions() == revisions); CHECK(dump(e.context) == original); CHECK_FALSE(e.module->error());
  REQUIRE(rewriter.insert_before(**ret, Opcode::Mov, {Operand(*e.value), Operand(42)})); REQUIRE(e.context.verify());
}
TEST_CASE("native splitting and merging transfer instructions without copying operands or labels") {
  Example e; IRBuilder builder(*e.entry); auto first = builder.assign(*e.value, 40); REQUIRE(first);
  auto sum = builder.emit(Opcode::Add, {Operand(*e.value), Operand(*e.value), Operand(2)}); REQUIRE(sum);
  auto ret = builder.ret(Operand(*e.value)); REQUIRE(ret);
  auto label = e.function->create_label(*e.entry, "entry"); REQUIRE(label);
  auto alias = e.function->create_label(*e.entry, "alias"); REQUIRE(alias);
  auto operands = (*sum)->operands().data(); auto ret_operands = (*ret)->operands().data();
  auto first_iterator = e.entry->instructions().begin(); auto sum_iterator = std::next(first_iterator);
  IRRewriter rewriter(*e.function); const auto revision = e.function->revisions();
  auto split = rewriter.split_before(**sum, "suffix"); REQUIRE(split); CHECK(e.entry->size() == 1); CHECK((*split)->size() == 2);
  CHECK(&(*label)->block() == e.entry); CHECK(&(*alias)->block() == e.entry); CHECK((*split)->labels().empty());
  CHECK(&(*sum)->block() == *split); CHECK(&(*ret)->block() == *split);
  CHECK(&*sum_iterator == *sum); CHECK((*sum)->operands().data() == operands); CHECK((*ret)->operands().data() == ret_operands);
  CHECK(e.function->find_block("suffix") == *split); CHECK(e.function->revisions().control_flow > revision.control_flow);
  REQUIRE(e.context.verify()); REQUIRE(rewriter.merge_fallthrough(*e.entry));
  CHECK(e.function->find_block("suffix") == nullptr); CHECK(e.entry->size() == 3);
  CHECK(&(*sum)->block() == e.entry); CHECK(&(*ret)->block() == e.entry); CHECK(&*sum_iterator == *sum);
  CHECK((*sum)->operands().data() == operands); REQUIRE(e.context.verify());
  REQUIRE(e.function->create_block_before(*e.entry, "suffix")); REQUIRE(e.context.verify());
}
TEST_CASE("native splits at the first instruction and terminator retain fallthrough semantics") {
  for (bool at_first : {false, true}) {
    Example e; IRBuilder builder(*e.entry); auto first = builder.assign(*e.value, 42); REQUIRE(first);
    auto ret = builder.ret(Operand(*e.value)); REQUIRE(ret); IRRewriter rewriter(*e.function);
    auto split = rewriter.split_before(at_first ? **first : **ret); REQUIRE(split);
    CHECK(e.entry->size() == (at_first ? 0 : 1)); CHECK((*split)->size() == (at_first ? 2 : 1));
    REQUIRE(e.context.verify()); REQUIRE(rewriter.merge_fallthrough(*e.entry)); REQUIRE(e.context.verify());
  }
}
TEST_CASE("native split failures do not allocate a block or change revisions") {
  Example e; auto ret = IRBuilder(*e.entry).ret(Operand(42)); REQUIRE(ret);
  Example other; auto foreign = IRBuilder(*other.entry).ret(Operand(0)); REQUIRE(foreign);
  IRRewriter rewriter(*e.function); auto original = dump(e.context); auto revision = e.function->revisions();
  CHECK_FALSE(rewriter.split_before(**ret, "entry")); CHECK_FALSE(rewriter.split_before(**foreign));
  CHECK_FALSE(rewriter.split_before(**ret, std::string_view("bad\0name", 8)));
  CHECK(e.function->revisions() == revision); CHECK(dump(e.context) == original); REQUIRE(e.context.verify());
}
TEST_CASE("native merging rejects labels terminators scope changes and foreign ownership atomically") {
  Example e; IRRewriter rewriter(*e.function); Example other;
  auto next = e.function->create_block("next"); REQUIRE(next);
  SUBCASE("label anchor") { REQUIRE(e.function->create_label(**next)); }
  SUBCASE("terminator") { REQUIRE(IRBuilder(*e.entry).ret(Operand(42))); }
  SUBCASE("scope boundary") {
    auto scope = e.function->create_stack_scope(); REQUIRE(scope); REQUIRE(rewriter.set_stack_scope(**next, *scope));
  }
  REQUIRE(IRBuilder(**next).ret(Operand(42))); REQUIRE(e.context.verify());
  auto original = dump(e.context); auto revision = e.function->revisions();
  CHECK_FALSE(rewriter.merge_fallthrough(*e.entry)); CHECK_FALSE(rewriter.merge_fallthrough(**next));
  CHECK_FALSE(rewriter.merge_fallthrough(*other.entry));
  CHECK(e.function->revisions() == revision); CHECK(dump(e.context) == original); REQUIRE(e.context.verify());
}
TEST_CASE("native label cleanup retains branches switch cases addresses and both data anchors") {
  Example e; auto target = e.function->create_block("target"); REQUIRE(target);
  auto unused = e.function->create_label(**target, "unused"); REQUIRE(unused);
  auto direct = e.function->create_label(**target, "direct"); REQUIRE(direct);
  auto indirect = e.function->create_label(**target, "indirect"); REQUIRE(indirect);
  auto data = e.function->create_label(**target, "data"); REQUIRE(data);
  auto base = e.function->create_label(**target, "base"); REQUIRE(base);
  auto case_alias = e.function->create_label(**target, "case"); REQUIRE(case_alias);
  auto switch_block = e.function->create_block("switch"); REQUIRE(switch_block);
  IRBuilder builder(*e.entry); REQUIRE(builder.address_of(**indirect)); REQUIRE(builder.jump(**direct));
  REQUIRE(IRBuilder(**target).ret(Operand(42)));
  REQUIRE(IRBuilder(**switch_block).emit(Opcode::Switch, {Operand(0), Operand(**case_alias), Operand(**case_alias)}));
  auto ending = e.function->create_block("ending"); REQUIRE(ending); REQUIRE(IRBuilder(**ending).ret(Operand(0)));
  REQUIRE(e.module->create_data("difference", LabelReference{*data, *base, 8}));
  IRRewriter rewriter(*e.function); auto revision = e.function->revisions(); auto erased = rewriter.erase_unused_labels(); REQUIRE(erased); CHECK(*erased == 1);
  CHECK(e.function->find_label("unused") == nullptr); CHECK((*target)->labels().size() == 5);
  CHECK(e.function->find_label("direct") == *direct); CHECK(e.function->find_label("indirect") == *indirect);
  CHECK(e.function->find_label("data") == *data); CHECK(e.function->find_label("base") == *base);
  CHECK(e.function->find_label("case") == *case_alias); REQUIRE(e.context.verify());
  CHECK(e.function->revisions().control_flow > revision.control_flow); revision = e.function->revisions();
  erased = rewriter.erase_unused_labels(); REQUIRE(erased); CHECK(*erased == 0); CHECK(e.function->revisions() == revision);
  REQUIRE(e.function->create_label(**target, "unused")); REQUIRE(e.context.verify());
}
TEST_CASE("native named block indexing survives clones insertions and removed names") {
  Example e;
  for (unsigned n = 0; n < 4096; ++n) {
    auto name = "block" + std::to_string(n); auto block = e.function->create_block_after(*e.entry, name); REQUIRE(block);
    CHECK(e.function->find_block(name) == *block);
  }
  auto &last = *std::prev(e.function->blocks().end()); REQUIRE(IRBuilder(last).ret(Operand(42)));
  auto copied = clone(e.context); REQUIRE(copied); auto &f = *(*copied)->modules().begin()->functions().begin();
  CHECK(f.find_block("entry") != e.entry); CHECK(f.find_block("missing") == nullptr); CHECK(f.find_block("") == nullptr);
  for (auto &block : f.blocks()) CHECK(f.find_block(block.name()) == &block);
  CHECK(dump(**copied) == dump(e.context)); REQUIRE((*copied)->verify());
}
TEST_CASE("native structural edits cover every public opcode with stable instruction storage") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    auto &function = *context->modules().begin()->functions().begin();
    IRRewriter rewriter(function); std::vector<Instruction *> instructions;
    for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) instructions.push_back(&instruction);
    for (auto instruction : instructions) {
      auto &source = const_cast<BasicBlock &>(instruction->block()); auto operands = instruction->operands().data();
      auto split = rewriter.split_before(*instruction); REQUIRE(split);
      CHECK(instruction->operands().data() == operands); CHECK(&instruction->block() == *split);
      REQUIRE(rewriter.merge_fallthrough(source)); CHECK(&instruction->block() == &source); CHECK(instruction->operands().data() == operands);
    }
    REQUIRE(context->verify());
  }
}
TEST_CASE("native structural fixtures clone and roundtrip text and binary without mutating source") {
  for (unsigned n = 0; n <= static_cast<unsigned>(test::RewriteExample::Scope); ++n) {
    CAPTURE(n); auto context = test::rewrite_example(static_cast<test::RewriteExample>(n)); auto original = dump(*context);
    auto copy = clone(*context); REQUIRE(copy); CHECK(dump(**copy) == original);
    auto text = serialization::write_text(*context); REQUIRE(text); auto parsed = serialization::parse_text(*text); REQUIRE(parsed);
    auto second = serialization::write_text(**parsed); REQUIRE(second); CHECK(*second == *text);
    auto bytes = serialization::write_binary(*context); REQUIRE(bytes); auto decoded = serialization::read_binary(*bytes); REQUIRE(decoded);
    auto reencoded = serialization::write_binary(**decoded); REQUIRE(reencoded); CHECK(*reencoded == *bytes);
    CHECK(dump(*context) == original);
  }
}
