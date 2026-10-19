/* Licensed under the MIT License. Demand-driven execution on owning C++ IR. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
mirnext::Result<mirnext::execution::ExecutionSession> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Context source;
  auto module = source.create_module ("example");
  if (!module) return module.error ();
  auto f = (*module)->create_function ("choose", {{Type::i64 ()}, {Type::i64 ()}});
  if (!f) return f.error ();
  auto entry = (*f)->create_block (), zero = (*f)->create_block (), nonzero = (*f)->create_block ();
  if (!entry) return entry.error ();
  if (!zero) return zero.error ();
  if (!nonzero) return nonzero.error ();
  auto label = (*f)->create_label (**nonzero, "nonzero");
  if (!label) return label.error ();
  auto branch = IRBuilder (**entry).branch_if (Operand (*(*f)->arguments ()[0]), **label);
  if (!branch) return branch.error ();
  auto returned = IRBuilder (**zero).ret (17);
  if (!returned) return returned.error ();
  returned = IRBuilder (**nonzero).ret (42);
  if (!returned) return returned.error ();
  return ExecutionSession::create (source,
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
  if (!session) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  auto f = session->find_function ("example", "choose");
  if (!f) return 1;
  for (std::int64_t input : {1, 1, 0}) {
    const std::array args{input};
    auto value = f->call_i64 (args);
    if (!value) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    if (*value != (input ? 42 : 17)) return 1;
    std::cout << "choose(" << input << ") = " << *value << '\n';
  }
  auto statistics = f->block_statistics ();
  if (!statistics) return 1;
  std::cout << statistics->compiled_blocks << '/' << statistics->blocks << " blocks published\n";
}
