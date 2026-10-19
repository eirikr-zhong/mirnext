/* Licensed under the MIT License. Owning interpretation with a typed C++ import. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::execution;
Result<ExecutionSession> build() {
  Context context; auto m = context.create_module("counter"); if (!m) return m.error();
  auto total = (*m)->create_variable("total",Type::i64(),0); if (!total) return total.error();
  auto host = (*m)->import("twice",Signature{{Type::i64()},{Type::i64()}}); if (!host) return host.error();
  auto f = (*m)->create_function("add",{{Type::i64()},{Type::i64()}}); if (!f) return f.error();
  auto block = (*f)->create_block(); if (!block) return block.error(); IRBuilder body(**block);
  auto doubled = body.call(**host,{Operand(*(*f)->arguments()[0])}); if (!doubled) return doubled.error();
  auto value = doubled->value(); if (!value) return value.error();
  auto previous = body.load(**total); if (!previous) return previous.error();
  auto next = body.expression(**previous)+body.expression(**value);
  auto stored = body.store(**total,next); if (!stored) return stored.error();
  auto returned = body.ret(next); if (!returned) return returned.error();
  return ExecutionSession::create(context,{.mode=ExecutionMode::Interpreter,.link={.resolve=[](const ImportRequest &request) -> Result<ExternalSymbol> {
    if (request.name != "twice") return Error{ErrorCode::UnknownName,"unknown host import"};
    ExternalSymbol host{0,Signature{{Type::i64()},{Type::i64()}},{}};
    host.interpret = [](std::span<const ExecutionArgument> args) -> Result<std::vector<ScalarValue>> {
      auto value = args[0].scalar()->as_i64(); if (!value) return value.error();
      return std::vector<ScalarValue>{ScalarValue(*value*2)};
    }; return host;
  }},.instruction_limit=1000,.call_depth_limit=16});
}
int main() {
  auto program = build(); if (!program) { std::cerr << program.error().message << '\n'; return 1; }
  // Source IR has been destroyed; the program owns its IR snapshot and globals.
  auto f = program->find_function("counter","add"); if (!f) return 1;
  for (std::int64_t increment : {10,11}) {
    std::array args{increment}; auto value = program->call_i64(*f,args);
    if (!value) { std::cerr << value.error().message << '\n'; return 1; }
    std::cout << "total = " << *value << '\n'; if (*value != (increment == 10 ? 20 : 42)) return 1;
  }
}
