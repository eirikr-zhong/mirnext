/* Licensed under the MIT License. One typed call API for interpretation and JIT. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
Result<ExecutionSession> build (ExecutionMode mode) {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("values");
  builder.create_function (module, "sum",
                           Signature{.results = {Type::i64 (), Type (Type::Kind::F64)},
                                     .parameters = {Type::block (16), Type (Type::Kind::F64)}});
  auto pair = builder.argument (0);
  auto first = builder.create_load (builder.memory (Type::i64 (), pair));
  auto second = builder.create_load (builder.memory (Type::i64 (), pair, 8));
  auto total = builder.create_add (first, second);
  builder.create_return ({total, builder.argument (1)});
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  return ExecutionSession::create (std::move (program), {.mode = mode});
}
Result<void> run (ExecutionMode mode) {
  auto session = build (mode);
  if (!session.has_value ()) return session.error ();
  // The session owns its IR and loaded data after build's Program is destroyed.
  auto sum = session.value ().find_function ("values", "sum");
  if (!sum.has_value ()) return sum.error ();
  std::array<std::int64_t, 2> pair{20, 22};
  std::array args{ExecutionArgument::block (std::as_bytes (std::span (pair))),
                  ExecutionArgument (2.5)};
  auto result = session.value ().call (sum.value (), args);
  if (!result.has_value ()) return result.error ();
  auto integer = (result.value ())[0].as_i64 ();
  if (!integer.has_value ()) return integer.error ();
  auto floating = (result.value ())[1].as_double ();
  if (!floating.has_value ()) return floating.error ();
  std::cout << (mode == ExecutionMode::JIT ? "JIT" : "Interpreter") << ": " << integer.value ()
            << ", " << floating.value () << '\n';
  if (integer.value () != 42 || floating.value () != 2.5)
    return Error{ErrorCode::InvalidOperand, "unexpected result"};
  return {};
}
int main () {
  auto interpreted = run (ExecutionMode::Interpreter);
  if (!interpreted.has_value ()) {
    std::cerr << interpreted.error ().message << '\n';
    return 1;
  }
  auto target = target::TargetMachine::native ();
  if (!target.has_value ()) return 1;
  if (target.value ().abi () == target::ABI::X86Windows) {
    auto native = build (ExecutionMode::JIT);
    if (native.has_value ()
        || native.error ().message.find ("Windows does not support multiple return values")
             == std::string::npos)
      return 1;
    std::cout
      << "Windows JIT supports one return value; both results above execute in the interpreter.\n";
  } else {
    auto native = run (ExecutionMode::JIT);
    if (!native.has_value ()) {
      std::cerr << native.error ().message << '\n';
      return 1;
    }
  }
}
