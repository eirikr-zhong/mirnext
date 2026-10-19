/* Licensed under the MIT License. Native block-argument DSL, independent of the old runtime. */
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  auto failed = [] (const Error &error) {
    std::cerr << error.message << '\n';
    return 1;
  };
  Context context;
  auto module = context.create_module ("example");
  if (!module) return failed (module.error ());
  auto sum = (*module)->create_function ("sum", {{Type::i64 ()}, {Type::block (16)}});
  if (!sum) return failed (sum.error ());
  auto body = (*sum)->create_block ();
  if (!body) return failed (body.error ());
  IRBuilder calculate (**body);
  auto pair = (*sum)->arguments ()[0];
  auto left = calculate.load (Memory{Type::i64 (), pair});
  if (!left) return failed (left.error ());
  auto right = calculate.load (Memory{Type::i64 (), pair, nullptr, 8});
  if (!right) return failed (right.error ());
  auto total = calculate.binary (Opcode::Add, Operand (**left), Operand (**right));
  if (!total) return failed (total.error ());
  auto returned = calculate.ret (Operand (**total));
  if (!returned) return failed (returned.error ());
  auto entry = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  if (!entry) return failed (entry.error ());
  auto block = (*entry)->create_block ();
  if (!block) return failed (block.error ());
  IRBuilder builder (**block);
  auto scope = builder.begin_stack_scope ();
  if (!scope) return failed (scope.error ());
  auto buffer = builder.allocate_stack (16);
  if (!buffer) return failed (buffer.error ());
  auto a = builder.store (Memory{Type::i64 (), *buffer}, 20);
  if (!a) return failed (a.error ());
  auto b = builder.store (Memory{Type::i64 (), *buffer, nullptr, 8}, 22);
  if (!b) return failed (b.error ());
  auto call = builder.call (**sum, {BlockArgument (**buffer, 16)});
  if (!call) return failed (call.error ());
  auto answer = call->value ();
  if (!answer) return failed (answer.error ());
  returned = builder.ret (Operand (**answer));
  if (!returned) return failed (returned.error ());
  auto ended = builder.end_stack_scope (**scope);
  if (!ended) return failed (ended.error ());
  auto text = serialization::write_text (context);
  if (!text) return failed (text.error ());
  auto binary = serialization::write_binary (context);
  if (!binary) return failed (binary.error ());
  auto decoded = serialization::read_binary (*binary);
  if (!decoded) return failed (decoded.error ());
  std::cout << *text;
  return (*decoded)->verify () ? 0 : 1;
}
