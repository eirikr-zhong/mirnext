/* Licensed under the MIT License. Sessions own IR and global snapshots. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
TEST_CASE (
  "module sessions retain separate IR data and cold compilation snapshots after source rewrites") {
  for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                      ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
    CAPTURE (engine);
    Program c;
    auto m = mirnext::ir::detail::access (c).create_module ("m");
    REQUIRE ((m).has_value ());
    auto total
      = mirnext::ir::detail::access (((m)).value ()).create_variable ("total", Type::i64 (), 20);
    REQUIRE ((total).has_value ());
    auto f
      = mirnext::ir::detail::access (((m)).value ()).create_function ("f", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    InstructionBuilder b ((block).value ());
    auto loaded = b.load ((total).value ());
    REQUIRE ((loaded).has_value ());
    auto sum = b.binary (Opcode::Add, Operand ((loaded).value ()), 22);
    REQUIRE ((sum).has_value ());
    REQUIRE ((b.store ((total).value (), Operand ((sum).value ()))).has_value ());
    REQUIRE ((b.ret (Operand ((sum).value ()))).has_value ());
    ExecutionOptions options{.select_engine
                             = [=] (auto &) -> Result<ExecutionEngine> { return engine; }};
    auto first = ExecutionSession::create ((m).value (), options);
    REQUIRE ((first).has_value ());
    Instruction *add = nullptr;
    for (auto &i : block.value ().instructions ())
      if (i.opcode () == Opcode::Add) add = &i;
    REQUIRE (add);
    REQUIRE (
      (InstructionRewriter ((f).value ()).replace_operand (*add, 2, Operand (2))).has_value ());
    REQUIRE ((c.verify ()).has_value ());
    auto second = ExecutionSession::create ((m).value (), options);
    REQUIRE ((second).has_value ());
    auto a = first.value ().find_function ("m", "f"), z = second.value ().find_function ("m", "f");
    REQUIRE ((a).has_value ());
    REQUIRE ((z).has_value ());
    CHECK_FALSE (first.value ().owns ((z).value ()));
    CHECK_FALSE (second.value ().owns ((a).value ()));
    // Both native bodies may still be cold. A later source rewrite must affect
    // neither their eventual compilation nor either session's global storage.
    REQUIRE (
      (InstructionRewriter ((f).value ()).replace_operand (*add, 2, Operand (99))).has_value ());
    for (auto [function, expected] : {std::pair{&(a).value (), 42}, std::pair{&(z).value (), 22},
                                      std::pair{&(a).value (), 64}, std::pair{&(z).value (), 24}}) {
      auto result = function->call_i64 ();
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      CHECK ((result).value () == expected);
    }
    auto one = first.value ().find_data ("m", "total"),
         two = second.value ().find_data ("m", "total");
    REQUIRE ((one).has_value ());
    REQUIRE ((two).has_value ());
    CHECK (one.value ().address () != two.value ().address ());
  }
}
