/* Licensed under the MIT License. Demand-driven execution on owning C++ IR. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
mirnext::Result<mirnext::execution::ExecutionSession> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("example");
  builder.create_function<std::int64_t (std::int64_t)> (module, "choose");
  auto *zero = builder.create_block ("zero");
  auto *nonzero = builder.create_block ("nonzero");
  builder.create_cond_branch (builder.argument (0), nonzero);
  builder.set_insert_point (zero);
  builder.create_return (builder.constant_i64 (17));
  builder.set_insert_point (nonzero);
  builder.create_return (builder.constant_i64 (42));
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  return ExecutionSession::create (std::move (program),
                                   {.link
                                    = {.compilation = JITCompilation::BlockVersioning,
                                       .block_versioning
                                       = {.code_capacity = 1024 * 1024,
                                          .observer = [] (const BlockCompilationSnapshot &event)
                                            -> Result<void> {
                                            std::cout << "compile "
                                                      << event.version.block ().function ().name ()
                                                      << " bb" << event.version.id ().index ()
                                                      << ": " << event.compiled.bytes ().size ()
                                                      << " bytes\n";
                                            return {};
                                          }}}});
}
int main () {
  auto session = build ();
  if (!session.has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  auto f = session.value ().find_function ("example", "choose");
  if (!f.has_value ()) return 1;
  for (std::int64_t input : {1, 1, 0}) {
    auto value = f.value ().call_i64 ({input});
    if (!value.has_value ()) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    if (value.value () != (input ? 42 : 17)) return 1;
    std::cout << "choose(" << input << ") = " << value.value () << '\n';
  }
  auto statistics = f.value ().block_statistics ();
  if (!statistics) return 1;
  std::cout << statistics->compiled_blocks << '/' << statistics->blocks << " blocks published\n";
}
