/* Licensed under the MIT License. MIR text and binary corpus coverage. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Serialization/Compression.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/StackScopes.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../IR/Expressions.hpp"
#include "../IR/Rewriter.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/Inline.hpp"
#include "../Transforms/InlineFixtures.hpp"
#include "../Fixtures/UtilityPrograms.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE ("complete native format corpus survives text and binary roundtrips") {
  using namespace mirnext::serialization;
  auto write = [&] (const mirnext::ir::Program &context, const std::string &name) {
    CAPTURE (name);
    auto text = write_text (context);
    INFO (((text).has_value () ? "" : text.error ().message));
    REQUIRE ((text).has_value ());
    auto parsed = parse_text ((text).value ());
    INFO (((parsed).has_value () ? "" : parsed.error ().message));
    REQUIRE ((parsed).has_value ());
    REQUIRE ((parsed.value ().verify ()).has_value ());
    auto second_text = write_text ((parsed).value ());
    REQUIRE ((second_text).has_value ());
    auto text_again = parse_text ((second_text).value ());
    REQUIRE ((text_again).has_value ());
    auto canonical_text = write_text ((text_again).value ());
    REQUIRE ((canonical_text).has_value ());
    CHECK ((canonical_text).value () == (second_text).value ());
    auto bytes = write_binary (context);
    REQUIRE ((bytes).has_value ());
    auto binary = read_binary ((bytes).value ());
    INFO (((binary).has_value () ? "" : binary.error ().message));
    REQUIRE ((binary).has_value ());
    REQUIRE ((binary.value ().verify ()).has_value ());
    auto second_bytes = write_binary ((binary).value ());
    REQUIRE ((second_bytes).has_value ());
    auto binary_again = read_binary ((second_bytes).value ());
    REQUIRE ((binary_again).has_value ());
    auto canonical_bytes = write_binary ((binary_again).value ());
    REQUIRE ((canonical_bytes).has_value ());
    CHECK ((canonical_bytes).value () == (second_bytes).value ());
    auto raw = decompress ((bytes).value (), 256 * 1024 * 1024);
    REQUIRE ((raw).has_value ());
    auto compressed = compress ((raw).value ());
    REQUIRE ((compressed).has_value ());
    CHECK ((compressed).value () == (bytes).value ());
  };
  auto write_simplified = [&] (mirnext::ir::Program &context, const std::string &name) {
    auto original = mirnext::ir::dump (context);
    auto copied = mirnext::ir::clone (context);
    REQUIRE ((copied).has_value ());
    for (auto &module : copied.value ().modules ())
      for (auto &function : module.functions ()) {
        mirnext::analysis::FunctionAnalysisManager analyses (function);
        mirnext::transforms::FunctionPassManager passes;
        passes.add<mirnext::transforms::SimplifyPass> ();
        auto result = passes.run (function, analyses);
        INFO (((result).has_value () ? "" : result.error ().message));
        REQUIRE ((result).has_value ());
      }
    REQUIRE ((copied.value ().verify ()).has_value ());
    write ((copied).value (), "simplified-" + name);
    CHECK (mirnext::ir::dump (context) == original);
    mirnext::transforms::FunctionPassManager inliner;
    inliner.add<mirnext::transforms::InlinePass> ();
    auto inlined = inliner.run ((copied).value ());
    INFO (((inlined).has_value () ? "" : inlined.error ().message));
    REQUIRE ((inlined).has_value ());
    write ((copied).value (), "inlined-" + name);
    CHECK (mirnext::ir::dump (context) == original);
  };
  {
    auto memory = parse_text (mirnext::test::utility_memory_text);
    REQUIRE ((memory).has_value ());
    write ((memory).value (), "utility-memory.mir");
    mirnext::ir::Program sieve;
    REQUIRE ((mirnext::test::append_api_sieve (sieve, "api", 127, 2)).has_value ());
    write (sieve, "utility-api-sieve.mir");
    auto hello = mirnext::test::utility_hello ();
    REQUIRE ((hello).has_value ());
    write ((hello).value (), "utility-hello.mir");
    std::ifstream file (std::filesystem::path (MIRNEXT_TEST_SOURCE_DIR)
                        / "tests/Fixtures/scanner.mir");
    REQUIRE (file);
    std::string text ((std::istreambuf_iterator<char> (file)), {});
    auto scanner = parse_text (text);
    REQUIRE ((scanner).has_value ());
    write ((scanner).value (), "utility-scanner.mir");
  }
  for (auto &descriptor : mirnext::ir::instruction_descriptors ()) {
    auto context = mirnext::test::public_instruction_example (descriptor);
    write (*context, "opcode-" + std::string (descriptor.name) + ".mir");
  }
  for (unsigned n = 1; n <= 16; ++n) {
    auto name = "test" + std::to_string (n) + ".mir";
    std::ifstream file (std::filesystem::path (MIRNEXT_TEST_SOURCE_DIR) / "tests/Fixtures/Original"
                        / name);
    REQUIRE (file);
    std::string input ((std::istreambuf_iterator<char> (file)), {});
    auto parsed = parse_text (input);
    REQUIRE ((parsed).has_value ());
    write ((parsed).value (), name);
  }
  std::ifstream file (std::filesystem::path (MIRNEXT_TEST_SOURCE_DIR) / "tests/Fixtures/loop.mir");
  REQUIRE (file);
  std::string input ((std::istreambuf_iterator<char> (file)), {});
  auto loop = parse_text (input);
  REQUIRE ((loop).has_value ());
  write ((loop).value (), "loop.mir");
  auto layout = parse_text (R"(layout: module
head: i64 42
      i64 17
      import unused
      u8 1
before: expr constant
constant: func i64
          ret 42
          endfunc
          u8 2
labels: lref alias, alias, 0
entry: func i64
       local i64:p, i64:value
origin: alias: mov p, head
       mov value, i64:8(p)
       add value, value, i64:0(p)
       sub value, value, 17
       ret value
       endfunc
       endmodule
)");
  INFO (((layout).has_value () ? "" : layout.error ().message));
  REQUIRE ((layout).has_value ());
  write ((layout).value (), "layout.mir");
  auto alias_difference = parse_text (R"(labels: module
value: lref origin, alias, 0
entry: func i64
origin: alias: ret 42
       endfunc
       endmodule
)");
  REQUIRE ((alias_difference).has_value ());
  write ((alias_difference).value (), "alias-difference.mir");
  std::string block_text = "blocks:module\nimport unused\n";
  for (unsigned classification = 0; classification <= 5; ++classification) {
    auto suffix = std::to_string (classification);
    auto type = classification == 5   ? "rblk"
                : classification == 0 ? std::string ("blk")
                                      : "blk" + suffix;
    auto signature
      = (classification == 5 ? std::string{} : std::string ("i64, ")) + type + ":16(arg), ...\n";
    block_text += "sig" + suffix + ":proto " + signature + "fn" + suffix + ":func " + signature
                  + "local i64:r\nmov r, 0\ncall sig" + suffix + ", unused, "
                  + (classification == 5 ? std::string{} : std::string ("r, ")) + type
                  + ":16(arg), 1.0\nret"
                  + (classification == 5 ? std::string{} : std::string (" r")) + "\nendfunc\n";
  }
  block_text += "endmodule\n";
  auto blocks = parse_text (block_text);
  INFO (((blocks).has_value () ? "" : blocks.error ().message));
  REQUIRE ((blocks).has_value ());
  for (auto &function : blocks.value ().modules ().begin ()->functions ())
    for (auto &block : function.blocks ())
      for (auto &instruction : block.instructions ())
        if (instruction.opcode () == mirnext::ir::Opcode::Call) {
          auto index = function.signature ().results.empty () ? 2u : 3u;
          auto memory = *mirnext::ir::detail::operand_access (instruction.operands ()[index])
                           .get<mirnext::ir::Memory> ();
          memory.displacement = -12;
          mirnext::ir::detail::NodeAccess::index (memory) = function.find_register ("r").get ();
          memory.scale = 4;
          REQUIRE ((mirnext::ir::InstructionRewriter (function)
                      .replace_operand (instruction, index, mirnext::ir::Operand (memory)))
                     .has_value ());
        }
  write ((blocks).value (), "block-address-displacements.mir");
  for (auto [mode, name] :
       {std::pair{mirnext::test::ScopeTransfer::Branch, "scopes-branch.mir"},
        std::pair{mirnext::test::ScopeTransfer::Switch, "scopes-switch.mir"},
        std::pair{mirnext::test::ScopeTransfer::Indirect, "scopes-indirect.mir"}}) {
    auto scope_context = mirnext::test::stack_scope_example (mode);
    write (*scope_context, name);
  }
  mirnext::ir::Program large;
  for (auto [mode, name] :
       {std::pair{mirnext::test::RewriteExample::Fallthrough, "rewriter-fallthrough.mir"},
        {mirnext::test::RewriteExample::Branch, "rewriter-branch.mir"},
        {mirnext::test::RewriteExample::Indirect, "rewriter-indirect.mir"},
        {mirnext::test::RewriteExample::Switch, "rewriter-switch.mir"},
        {mirnext::test::RewriteExample::LabelData, "rewriter-label-data.mir"},
        {mirnext::test::RewriteExample::Scope, "rewriter-scope.mir"}}) {
    auto context = mirnext::test::rewrite_example (mode);
    write (*context, name);
  }
  for (auto [mode, name] :
       {std::pair{mirnext::test::ExpressionExample::Arithmetic, "expressions-arithmetic.mir"},
        {mirnext::test::ExpressionExample::Unsigned, "expressions-unsigned.mir"},
        {mirnext::test::ExpressionExample::Narrow, "expressions-narrow.mir"},
        {mirnext::test::ExpressionExample::Floating, "expressions-floating.mir"},
        {mirnext::test::ExpressionExample::NaN, "expressions-nan.mir"},
        {mirnext::test::ExpressionExample::Branch, "expressions-branch.mir"},
        {mirnext::test::ExpressionExample::Conversion, "expressions-conversion.mir"},
        {mirnext::test::ExpressionExample::Memory, "expressions-memory.mir"},
        {mirnext::test::ExpressionExample::Call, "expressions-call.mir"},
        {mirnext::test::ExpressionExample::Boolean, "expressions-boolean.mir"},
        {mirnext::test::ExpressionExample::Initializer, "expressions-initializer.mir"}}) {
    auto context = mirnext::test::expression_example (mode);
    write (*context, name);
    write_simplified (*context, name);
  }
  for (auto [mode, name] :
       {std::pair{mirnext::test::VariableExample::Initialized, "variables-initialized.mir"},
        std::pair{mirnext::test::VariableExample::Zero, "variables-zero.mir"},
        std::pair{mirnext::test::VariableExample::Constant, "variables-constant.mir"},
        std::pair{mirnext::test::VariableExample::Boolean, "variables-boolean.mir"},
        std::pair{mirnext::test::VariableExample::Floating, "variables-floating.mir"},
        std::pair{mirnext::test::VariableExample::Pointer, "variables-pointer.mir"},
        std::pair{mirnext::test::VariableExample::ExpressionInitializer,
                  "variables-expression.mir"},
        std::pair{mirnext::test::VariableExample::CrossModule, "variables-cross-module.mir"},
        std::pair{mirnext::test::VariableExample::Narrow, "variables-narrow.mir"}}) {
    auto context = mirnext::test::variable_example (mode);
    write (*context, name);
    write_simplified (*context, name);
  }
  for (auto [mode, name] :
       {std::pair{mirnext::test::CallExample::Direct, "calls-direct.mir"},
        std::pair{mirnext::test::CallExample::Indirect, "calls-indirect.mir"},
        std::pair{mirnext::test::CallExample::Block, "calls-block.mir"},
        std::pair{mirnext::test::CallExample::ReturnBlock, "calls-rblk.mir"},
        std::pair{mirnext::test::CallExample::Variadic, "calls-variadic.mir"},
        std::pair{mirnext::test::CallExample::VariadicBlock, "calls-variadic-block.mir"},
        std::pair{mirnext::test::CallExample::MultipleResults, "calls-results.mir"},
        std::pair{mirnext::test::CallExample::Inline, "calls-inline.mir"}}) {
    auto context = mirnext::test::call_example (mode);
    write (*context, name);
    write_simplified (*context, name);
  }
  auto inline_cases = mirnext::test::inline_fixtures ();
  for (auto [index, name] : {std::pair{inline_cases.size () - 2, "inlined-private-data.mir"},
                             {inline_cases.size () - 1, "inlined-private-function.mir"}}) {
    auto parsed = parse_text (inline_cases[index]);
    REQUIRE ((parsed).has_value ());
    mirnext::transforms::FunctionPassManager passes;
    passes.add<mirnext::transforms::SimplifyPass> ();
    passes.add<mirnext::transforms::InlinePass> ();
    auto result = passes.run ((parsed).value ());
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    write ((parsed).value (), name);
  }
  auto module = mirnext::ir::detail::access (large).create_module ("large");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{mirnext::ir::Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto value = mirnext::ir::detail::access (((function)).value ())
                 .create_register (mirnext::ir::Type::i64 ());
  REQUIRE ((value).has_value ());
  mirnext::ir::InstructionBuilder builder ((block).value ());
  std::uint64_t bits = 42;
  for (unsigned n = 0; n < 40000; ++n) {
    bits = bits * UINT64_C (6364136223846793005) + 1;
    REQUIRE (
      (builder.assign ((value).value (), mirnext::ir::Operand (std::bit_cast<std::int64_t> (bits))))
        .has_value ());
  }
  REQUIRE ((builder.ret (mirnext::ir::Operand (std::int64_t{42}))).has_value ());
  write (large, "multiple-compression-blocks.mir");
}
