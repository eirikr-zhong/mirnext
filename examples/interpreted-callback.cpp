/* Licensed under the MIT License. C++ host calls back into owning interpreted IR. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
std::int64_t apply (std::int64_t (*callback) (std::int64_t), std::int64_t value) {
  return callback (value);
}
Result<ExecutionSession> build () {
  Program program;
  MIRBuilder builder (program);
  auto *m = builder.create_module ("callbacks");
  auto *callback = builder.create_function<std::int64_t (std::int64_t)> (m, "add_two");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (2)));
  auto *host = builder.create_import<std::int64_t (void *, std::int64_t)> (m, "apply");
  builder.create_function<std::int64_t ()> (m, "entry");
  auto answer
    = builder.create_call (host, {builder.create_address_of (callback), builder.constant_i64 (40)});
  builder.create_return (answer.value ());
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  return ExecutionSession::create (std::move (program),
                                   {.mode = ExecutionMode::Interpreter,
                                    .link = {.resolve = [] (const ImportRequest &request)
                                               -> Result<ExternalSymbol> {
                                      if (request.name != "apply")
                                        return Error{ErrorCode::UnknownName,
                                                     "unknown host function"};
                                      return ExternalSymbol{reinterpret_cast<std::uintptr_t> (
                                                              &apply),
                                                            Signature{{Type::i64 ()},
                                                                      {Type::pointer (),
                                                                       Type::i64 ()}},
                                                            {}};
                                    }},
                                    .instruction_limit = 1000,
                                    .call_depth_limit = 16,
                                    .native_entries = true});
}
}  // namespace
int main () {
  auto target = target::TargetMachine::native ();
  if (!target.has_value ()) return 1;
  auto session = build ();
  if (!session.has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  // Source IR is gone. apply receives a real native address whose entry executes
  // add_two in the interpreter, within the original invocation's budgets.
  auto f = session.value ().find_function ("callbacks", "entry");
  if (!f.has_value ()) return 1;
  auto value = session.value ().call_i64 (f.value ());
  if (!value.has_value ()) {
    std::cerr << value.error ().message << '\n';
    return 1;
  }
  std::cout << "callback result = " << value.value () << '\n';
  return value.value () == 42 ? 0 : 1;
}
