/* Licensed under the MIT License. One symbol image with per-function engines. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
mirnext::Result<mirnext::execution::FunctionHandle> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("mixed");
  auto *step = builder.create_function<std::int64_t (std::int64_t)> (module, "step");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (2)));
  // The global contains a typed symbol relocation, resolved by the session.
  auto *target
    = builder.create_variable (module, "target", Type::pointer (), Operand (step->symbol ()));
  builder.create_function<std::int64_t ()> (module, "entry");
  auto answer = builder.create_indirect_call (step, builder.create_load (target),
                                              {builder.constant_i64 (40)});
  builder.create_return (answer.value ());
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  auto session = ExecutionSession::create (std::move (program),
                                           {.select_engine
                                            = [] (const Function &f) -> Result<ExecutionEngine> {
                                             return f.symbol ().name () == "step"
                                                      ? ExecutionEngine::Interpreter
                                                      : ExecutionEngine::LazyJIT;
                                           }});
  if (!session.has_value ()) return session.error ();
  if (session.value ().mode () != ExecutionMode::Mixed)
    return Error{ErrorCode::InvalidArgument, "expected mixed execution"};
  return session.value ().find_function ("mixed", "entry");
}
int main () {
  auto function = build ();
  if (!function.has_value ()) {
    std::cerr << function.error ().message << '\n';
    return 1;
  }
  // The handle owns the program after both source builder and session die.
  auto result = function.value ().call_i64 ();
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  std::cout << "mixed result = " << result.value () << '\n';
  return result.value () == 42 ? 0 : 1;
}
