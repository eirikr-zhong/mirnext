/* Licensed under the MIT License. Owning callable with persistent data. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
mirnext::Result<mirnext::execution::FunctionHandle> build() {
  using namespace mirnext; using namespace mirnext::ir;
  Context context;
  MIRNEXT_TRY_VALUE(module, context.create_module("counter"));
  MIRNEXT_TRY_VALUE(total, module->create_variable("total",Type::i64(),0));
  MIRNEXT_TRY_VALUE(add, module->create_function("add",{{Type::i64()},{Type::i64()}}));
  MIRNEXT_TRY_VALUE(block, add->create_block()); IRBuilder body(*block);
  MIRNEXT_TRY_VALUE(previous, body.load(*total));
  auto next = body.expression(*previous) + body.expression(*add->arguments()[0]);
  MIRNEXT_RESULT_RET(body.store(*total,next));
  MIRNEXT_RESULT_RET(body.ret(next));
  MIRNEXT_TRY_VALUE(session, execution::ExecutionSession::create(context,{.link={.compilation=execution::JITCompilation::Lazy}}));
  return session.find_function("counter","add");
}
int main() {
  auto target = mirnext::target::TargetMachine::native(); if (!target) return 1;
  auto add = build(); if (!add) { std::cerr << add.error().message << '\n'; return 1; }
  // Both source IR and session are destroyed. The callable retains data, code
  // and the services used to compile its body and ABI adapter on first call.
  for (std::int64_t increment : {20,22}) {
    std::array arguments{increment}; auto value = add->call_i64(arguments);
    if (!value) { std::cerr << value.error().message << '\n'; return 1; }
    std::cout << "total = " << *value << '\n';
    if (*value != (increment == 20 ? 20 : 42)) return 1;
  }
  return 0;
}
