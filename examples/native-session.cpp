/* Licensed under the MIT License. Owning callable with persistent data. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
mirnext::Result<mirnext::execution::FunctionHandle> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("counter");
  auto *total = builder.create_variable (module, "total", std::int64_t{0});
  builder.create_function<std::int64_t (std::int64_t)> (module, "add");
  auto next = builder.create_add (builder.create_load (total), builder.argument (0));
  builder.create_store (total, next);
  builder.create_return (next);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  auto session
    = execution::ExecutionSession::create (std::move (program),
                                           {.link
                                            = {.compilation = execution::JITCompilation::Lazy}});
  if (!session.has_value ()) return session.error ();
  return session.value ().find_function ("counter", "add");
}
int main () {
  auto add = build ();
  if (!add.has_value ()) {
    std::cerr << add.error ().message << '\n';
    return 1;
  }
  for (std::int64_t increment : {20, 22}) {
    auto value = add.value ().call_i64 ({increment});
    if (!value.has_value ()) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    std::cout << "total = " << value.value () << '\n';
    if (value.value () != (increment == 20 ? 20 : 42)) return 1;
  }
}
