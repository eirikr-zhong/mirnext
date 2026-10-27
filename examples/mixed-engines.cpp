/* Licensed under the MIT License. One symbol image with per-function engines. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
mirnext::Result<mirnext::execution::FunctionHandle> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Context context;
  auto module = context.create_module ("mixed");
  if (!module) return module.error ();
  auto step = (*module)->create_function ("step", {{Type::i64 ()}, {Type::i64 ()}});
  if (!step) return step.error ();
  auto block = (*step)->create_block ();
  if (!block) return block.error ();
  IRBuilder body (**block);
  auto returned = body.ret (body.expression (*(*step)->arguments ()[0]) + 2);
  if (!returned) return returned.error ();
  auto pointer
    = (*module)->create_variable ("target", Type::pointer (), Operand ((*step)->symbol ()));
  if (!pointer) return pointer.error ();
  auto caller = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  if (!caller) return caller.error ();
  auto entry = (*caller)->create_block ();
  if (!entry) return entry.error ();
  IRBuilder builder (**entry);
  auto target = builder.load (**pointer);
  if (!target) return target.error ();
  auto call = builder.call_indirect ((*step)->symbol (), Operand (**target), {std::int64_t{40}});
  if (!call) return call.error ();
  auto value = call->value ();
  if (!value) return value.error ();
  auto ret = builder.ret (Operand (**value));
  if (!ret) return ret.error ();
  auto session
    = ExecutionSession::create (context, {.select_engine
                                          = [] (const Function &f) -> Result<ExecutionEngine> {
                                  return f.symbol ().name () == "step"
                                           ? ExecutionEngine::Interpreter
                                           : ExecutionEngine::LazyJIT;
                                }});
  if (!session) return session.error ();
  if (session->mode () != ExecutionMode::Mixed)
    return Error{ErrorCode::InvalidArgument, "expected mixed execution"};
  // Mutable target data keeps the call indirect through optimization. Every
  // engine still shares the same loaded symbol, relocation and global data.
  return session->find_function ("mixed", "entry");
}
int main () {
  auto function = build ();
  if (!function) {
    std::cerr << function.error ().message << '\n';
    return 1;
  }
  // The handle owns the program after both source Context and session die.
  auto result = function->call_i64 ();
  if (!result) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  std::cout << "mixed result = " << *result << '\n';
  return *result == 42 ? 0 : 1;
}
