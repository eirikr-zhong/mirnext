/* Licensed under the MIT License. Installed CXX-only construction consumer. */
#include <mirnext/Analysis/ConstructionOrder.hpp>
#include <mirnext/Execution/Session.hpp>
#include <iostream>
template <class T>
concept HasUncheckedMaterialization = requires (T &value) { materialize_stack_scopes (value); };
static_assert (!HasUncheckedMaterialization<mirnext::ir::Function>);
static_assert (!HasUncheckedMaterialization<mirnext::ir::Program>);
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("installed");
  builder.create_function<std::int64_t (std::int64_t)> (module, "add_one");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (1)));
  auto built = builder.finish ();
  if (!built.has_value ()) return 1;
  auto session = execution::ExecutionSession::create (std::move (program));
  if (!(session).has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  auto answer = session.value ().call_i64 ("installed", "add_one", {41});
  if (!(answer).has_value ()) {
    std::cerr << answer.error ().message << '\n';
    return 1;
  }
  return (answer).value () == 42 ? 0 : 1;
}
