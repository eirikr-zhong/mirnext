/* Licensed under the MIT License. Owning interpretation with a typed C++ import. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
Result<ExecutionSession> build () {
  Program program;
  MIRBuilder builder (program);
  auto *m = builder.create_module ("counter");
  auto *total = builder.create_variable (m, "total", std::int64_t{0});
  auto *host = builder.create_import<std::int64_t (std::int64_t)> (m, "twice");
  builder.create_function<std::int64_t (std::int64_t)> (m, "add");
  auto previous = builder.create_load (total);
  auto doubled = builder.create_call (host, {builder.argument (0)});
  auto next = builder.create_add (previous, doubled.value ());
  builder.create_store (total, next);
  builder.create_return (next);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  return ExecutionSession::create (std::move (program),
                                   {.mode = ExecutionMode::Interpreter,
                                    .link = {.resolve = [] (const ImportRequest &request)
                                               -> Result<ExternalSymbol> {
                                      if (request.name != "twice")
                                        return Error{ErrorCode::UnknownName, "unknown host import"};
                                      ExternalSymbol host{0,
                                                          Signature{{Type::i64 ()}, {Type::i64 ()}},
                                                          {}};
                                      host.interpret = [] (std::span<const ExecutionArgument> args)
                                        -> Result<std::vector<ScalarValue>> {
                                        auto value = args[0].scalar ().get ()->as_i64 ();
                                        if (!value.has_value ()) return value.error ();
                                        return std::vector<ScalarValue>{
                                          ScalarValue (value.value () * 2)};
                                      };
                                      return host;
                                    }},
                                    .instruction_limit = 1000,
                                    .call_depth_limit = 16});
}
int main () {
  auto program = build ();
  if (!program.has_value ()) {
    std::cerr << program.error ().message << '\n';
    return 1;
  }
  // Source IR has been destroyed; the program owns its IR snapshot and globals.
  auto f = program.value ().find_function ("counter", "add");
  if (!f.has_value ()) return 1;
  for (std::int64_t increment : {10, 11}) {
    auto value = f.value ().call_i64 ({increment});
    if (!value.has_value ()) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    std::cout << "total = " << value.value () << '\n';
    if (value.value () != (increment == 10 ? 20 : 42)) return 1;
  }
}
