/* Licensed under the MIT License. C++ host calls back into owning interpreted IR. */
#include <mirnext/Execution/Session.hpp>
#include <iostream>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::execution;
namespace {
std::int64_t apply(std::int64_t (*callback)(std::int64_t),std::int64_t value) { return callback(value); }
Result<ExecutionSession> build() {
  Context context; auto m = context.create_module("callbacks"); if (!m) return m.error();
  auto callback = (*m)->create_function("add_two",{{Type::i64()},{Type::i64()}}); if (!callback) return callback.error();
  auto cb = (*callback)->create_block(); if (!cb) return cb.error(); IRBuilder calculate(**cb);
  auto returned = calculate.ret(calculate.expression(*(*callback)->arguments()[0])+2); if (!returned) return returned.error();
  auto host = (*m)->import("apply",Signature{{Type::i64()},{Type::pointer(),Type::i64()}}); if (!host) return host.error();
  auto f = (*m)->create_function("entry",{{Type::i64()},{}}); if (!f) return f.error();
  auto block = (*f)->create_block(); if (!block) return block.error(); IRBuilder b(**block);
  auto called = b.call(**host,{Operand((*callback)->symbol()),40}); if (!called) return called.error();
  auto value = called->value(); if (!value) return value.error(); returned = b.ret(Operand(**value)); if (!returned) return returned.error();
  return ExecutionSession::create(context,{.mode=ExecutionMode::Interpreter,.link={.resolve=[](const ImportRequest &request) -> Result<ExternalSymbol> {
    if (request.name != "apply") return Error{ErrorCode::UnknownName,"unknown host function"};
    return ExternalSymbol{reinterpret_cast<std::uintptr_t>(&apply),Signature{{Type::i64()},{Type::pointer(),Type::i64()}},{}};
  }},.instruction_limit=1000,.call_depth_limit=16,.native_entries=true});
}
}
int main() {
  auto target = target::TargetMachine::native(); if (!target) return 1;
  auto session = build(); if (!session) { std::cerr << session.error().message << '\n'; return 1; }
  // Source IR is gone. apply receives a real native address whose entry executes
  // add_two in the interpreter, within the original invocation's budgets.
  auto f = session->find_function("callbacks","entry"); if (!f) return 1;
  auto value = session->call_i64(*f); if (!value) { std::cerr << value.error().message << '\n'; return 1; }
  std::cout << "callback result = " << *value << '\n'; return *value == 42 ? 0 : 1;
}
