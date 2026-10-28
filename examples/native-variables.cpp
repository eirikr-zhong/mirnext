/* Licensed under the MIT License. Shared cross-module variables. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *consumer = builder.create_module ("consumer");
  auto *provider = builder.create_module ("provider");
  auto *counter = builder.create_variable (provider, "counter", std::int64_t{20});
  auto *increment = builder.create_variable (provider, "increment", std::int64_t{22}, false);
  builder.export_symbol (counter);
  builder.export_symbol (increment);
  auto *imported_counter = builder.create_import_variable (consumer, "counter", Type::i64 ());
  auto *imported_increment
    = builder.create_import_variable (consumer, "increment", Type::i64 (), false);
  builder.create_function<std::int64_t ()> (consumer, "entry");
  auto sum = builder.create_add (builder.create_load (imported_counter),
                                 builder.create_load (imported_increment));
  builder.create_store (imported_counter, sum);
  builder.create_return (sum);
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
  std::cout << "answer = " << answer.value () << '\n';
  return answer.value () == 42 ? 0 : 1;
}
