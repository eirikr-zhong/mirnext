/* Licensed under the MIT License. One typed call API for interpretation and JIT. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
Result<ExecutionSession> build (ExecutionMode mode) {
  Context context;
  auto module = context.create_module ("values");
  if (!module) return module.error ();
  auto f = (*module)->create_function ("sum", {{Type::i64 (), Type (Type::Kind::F64)},
                                               {Type::block (16), Type (Type::Kind::F64)}});
  if (!f) return f.error ();
  auto block = (*f)->create_block ();
  if (!block) return block.error ();
  IRBuilder body (**block);
  auto pair = (*f)->arguments ()[0];
  auto left = body.load (Memory{Type::i64 (), pair});
  if (!left) return left.error ();
  auto right = body.load (Memory{Type::i64 (), pair, nullptr, 8});
  if (!right) return right.error ();
  auto sum = (body.expression (**left) + body.expression (**right)).value ();
  if (!sum) return sum.error ();
  std::array results{*sum, Operand (*(*f)->arguments ()[1])};
  auto returned = body.ret (results);
  if (!returned) return returned.error ();
  return ExecutionSession::create (context, {.mode = mode});
}
Result<void> run (ExecutionMode mode) {
  auto session = build (mode);
  if (!session) return session.error ();
  // The session owns its IR and loaded data after build's Context is destroyed.
  auto sum = session->find_function ("values", "sum");
  if (!sum) return sum.error ();
  std::array<std::int64_t, 2> pair{20, 22};
  std::array args{ExecutionArgument::block (std::as_bytes (std::span (pair))),
                  ExecutionArgument (2.5)};
  auto result = session->call (*sum, args);
  if (!result) return result.error ();
  auto integer = (*result)[0].as_i64 ();
  if (!integer) return integer.error ();
  auto floating = (*result)[1].as_double ();
  if (!floating) return floating.error ();
  std::cout << (mode == ExecutionMode::JIT ? "JIT" : "Interpreter") << ": " << *integer << ", "
            << *floating << '\n';
  if (*integer != 42 || *floating != 2.5)
    return Error{ErrorCode::InvalidOperand, "unexpected result"};
  return {};
}
int main () {
  auto interpreted = run (ExecutionMode::Interpreter);
  if (!interpreted) {
    std::cerr << interpreted.error ().message << '\n';
    return 1;
  }
  auto target = target::TargetMachine::native ();
  if (!target) return 1;
  if (target->abi () == target::ABI::X86Windows) {
    auto native = build (ExecutionMode::JIT);
    if (native
        || native.error ().message.find ("Windows does not support multiple return values")
             == std::string::npos)
      return 1;
    std::cout
      << "Windows JIT supports one return value; both results above execute in the interpreter.\n";
  } else {
    auto native = run (ExecutionMode::JIT);
    if (!native) {
      std::cerr << native.error ().message << '\n';
      return 1;
    }
  }
}
