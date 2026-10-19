/* Licensed under the MIT License. Sessions own IR and global snapshots. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::execution;
TEST_CASE("module sessions retain separate IR data and cold compilation snapshots after source rewrites") {
  for (auto engine : {ExecutionEngine::Interpreter,ExecutionEngine::EagerJIT,ExecutionEngine::LazyJIT,ExecutionEngine::BlockVersioning}) {
    CAPTURE(engine); Context c; auto m = c.create_module("m"); REQUIRE(m);
    auto total = (*m)->create_variable("total",Type::i64(),20); REQUIRE(total);
    auto f = (*m)->create_function("f",{{Type::i64()},{}}); REQUIRE(f);
    auto block = (*f)->create_block(); REQUIRE(block); IRBuilder b(**block);
    auto loaded = b.load(**total); REQUIRE(loaded);
    auto sum = b.binary(Opcode::Add,Operand(**loaded),22); REQUIRE(sum);
    REQUIRE(b.store(**total,Operand(**sum))); REQUIRE(b.ret(Operand(**sum)));
    ExecutionOptions options{.select_engine=[=](auto &) -> Result<ExecutionEngine> { return engine; }};
    auto first = ExecutionSession::create(**m,options); REQUIRE(first);
    Instruction *add = nullptr;
    for (auto &i : (*block)->instructions()) if (i.opcode() == Opcode::Add) add = &i;
    REQUIRE(add); REQUIRE(IRRewriter(**f).replace_operand(*add,2,Operand(2))); REQUIRE(c.verify());
    auto second = ExecutionSession::create(**m,options); REQUIRE(second);
    auto a = first->find_function("m","f"),z = second->find_function("m","f"); REQUIRE(a); REQUIRE(z);
    CHECK_FALSE(first->owns(*z)); CHECK_FALSE(second->owns(*a));
    // Both native bodies may still be cold. A later source rewrite must affect
    // neither their eventual compilation nor either session's global storage.
    REQUIRE(IRRewriter(**f).replace_operand(*add,2,Operand(99)));
    for (auto [function,expected] : {std::pair{&*a,42},std::pair{&*z,22},std::pair{&*a,64},std::pair{&*z,24}}) {
      auto result = function->call_i64(); INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(*result == expected);
    }
    auto one = first->find_data("m","total"),two = second->find_data("m","total"); REQUIRE(one); REQUIRE(two);
    CHECK(one->address() != two->address());
  }
}
