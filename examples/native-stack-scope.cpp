/* Licensed under the MIT License. Stack scope cleanup follows control flow. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("scopes");
  builder.create_function<std::int64_t ()> (module, "answer");
  auto *scope = builder.begin_stack_scope ("temporary");
  auto buffer = builder.create_stack_alloc (builder.constant_i64 (16));
  auto slot = builder.memory (Type::i64 (), buffer);
  builder.create_store (slot, builder.constant_i64 (42));
  builder.create_return (builder.create_load (slot));
  // The return edge restores the scope; this closes its construction scope.
  builder.end_stack_scope (scope);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  auto session = execution::ExecutionSession::create (std::move (program));
  if (!session.has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  auto result = session.value ().call_i64 ("scopes", "answer");
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  std::cout << result.value () << '\n';
  return result.value () == 42 ? 0 : 1;
}
