/* Licensed under the MIT License. Explicit cross-module function imports. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *provider = builder.create_module ("provider");
  auto *step = builder.create_function<std::int64_t (std::int64_t)> (provider, "step");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (2)));
  builder.export_symbol (step);
  auto *consumer = builder.create_module ("consumer");
  auto *imported = builder.create_import<std::int64_t (std::int64_t)> (consumer, "step");
  builder.create_function<std::int64_t ()> (consumer, "entry");
  auto call = builder.create_call (imported, {builder.constant_i64 (40)});
  builder.create_return (call.value ());
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
  auto answer = session.value ().call_i64 ("consumer", "entry");
  if (!answer.has_value ()) {
    std::cerr << answer.error ().message << '\n';
    return 1;
  }
  std::cout << answer.value () << '\n';
  return answer.value () == 42 ? 0 : 1;
}
