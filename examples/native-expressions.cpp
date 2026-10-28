/* Licensed under the MIT License. Expressions emit directly into canonical IR. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("expressions");
  auto *value = builder.create_variable (module, "value", std::int64_t{20});
  builder.create_function<std::int64_t ()> (module, "entry");
  auto incremented = builder.create_add (builder.create_load (value), builder.constant_i64 (1));
  auto answer = builder.create_mul (incremented, builder.constant_i64 (2));
  builder.create_store (value, answer);
  builder.create_return (answer);
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
  auto result = session.value ().call_i64 ("expressions", "entry");
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  std::cout << result.value () << '\n';
  return result.value () == 42 ? 0 : 1;
}
