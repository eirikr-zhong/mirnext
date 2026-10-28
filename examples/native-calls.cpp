/* Licensed under the MIT License. Block arguments with checked ABI metadata. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("example");
  auto *sum = builder.create_function (module, "sum",
                                       Signature{.results = {Type::i64 ()},
                                                 .parameters = {Type::block (16)}});
  auto pair = builder.argument (0);
  auto first = builder.create_load (builder.memory (Type::i64 (), pair));
  auto second = builder.create_load (builder.memory (Type::i64 (), pair, 8));
  builder.create_return (builder.create_add (first, second));
  builder.create_function<std::int64_t ()> (module, "entry");
  auto *scope = builder.begin_stack_scope ();
  auto buffer = builder.create_stack_alloc (builder.constant_i64 (16));
  builder.create_store (builder.memory (Type::i64 (), buffer), builder.constant_i64 (20));
  builder.create_store (builder.memory (Type::i64 (), buffer, 8), builder.constant_i64 (22));
  auto call = builder.create_call (sum, {builder.block_argument (buffer, Type::block (16))});
  builder.create_return (call.value ());
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
  auto answer = session.value ().call_i64 ("example", "entry");
  if (!answer.has_value ()) {
    std::cerr << answer.error ().message << '\n';
    return 1;
  }
  std::cout << "block sum = " << answer.value () << '\n';
  return answer.value () == 42 ? 0 : 1;
}
