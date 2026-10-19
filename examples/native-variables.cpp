/* Licensed under the MIT License. Typed cross-module data on the canonical C++ model. */
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main() {
  using namespace mirnext;
  using namespace mirnext::ir;
  auto failed = [](const Error &error) { std::cerr << error.message << '\n'; return 1; };
  Context context;
  auto consumer = context.create_module("consumer"); if (!consumer) return failed(consumer.error());
  auto provider = context.create_module("provider"); if (!provider) return failed(provider.error());
  auto counter = (*provider)->create_variable("counter", Type::i64(), 20); if (!counter) return failed(counter.error());
  auto increment = (*provider)->create_variable("increment", Type::i64(), 22, false); if (!increment) return failed(increment.error());
  for (auto variable : {*counter, *increment}) {
    auto exported = (*provider)->export_symbol(variable->symbol()); if (!exported) return failed(exported.error());
  }
  auto imported_counter = (*consumer)->import_variable("counter", Type::i64()); if (!imported_counter) return failed(imported_counter.error());
  auto imported_increment = (*consumer)->import_variable("increment", Type::i64(), false); if (!imported_increment) return failed(imported_increment.error());
  auto function = (*consumer)->create_function("entry", {{Type::i64()}, {}}); if (!function) return failed(function.error());
  auto block = (*function)->create_block(); if (!block) return failed(block.error()); IRBuilder builder(**block);
  auto left = builder.load(**imported_counter); if (!left) return failed(left.error());
  auto right = builder.load(**imported_increment); if (!right) return failed(right.error());
  auto sum = builder.binary(Opcode::Add, Operand(**left), Operand(**right)); if (!sum) return failed(sum.error());
  auto stored = builder.store(**imported_counter, Operand(**sum)); if (!stored) return failed(stored.error());
  auto answer = builder.load(**imported_counter); if (!answer) return failed(answer.error());
  auto returned = builder.ret(Operand(**answer)); if (!returned) return failed(returned.error());
  auto binary = serialization::write_binary(context); if (!binary) return failed(binary.error());
  auto decoded = serialization::read_binary(*binary); if (!decoded) return failed(decoded.error());
  auto text = serialization::write_text(context); if (!text) return failed(text.error());
  std::cout << *text; return (*decoded)->verify() ? 0 : 1;
}
