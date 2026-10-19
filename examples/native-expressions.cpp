/* Licensed under the MIT License. Typed expressions emit directly into the canonical IR. */
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main() {
  using namespace mirnext;
  using namespace mirnext::ir;
  auto failed = [](const Error &error) { std::cerr << error.message << '\n'; return 1; };
  Context context; auto module = context.create_module("expressions"); if (!module) return failed(module.error());
  auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return failed(function.error());
  auto block = (*function)->create_block(); if (!block) return failed(block.error()); IRBuilder builder(**block);
  auto value = (*module)->create_variable("value", Type::i64(), 20); if (!value) return failed(value.error());
  auto answer = (builder.expression(**value) + 1) * 2;
  auto stored = builder.store(**value, answer); if (!stored) return failed(stored.error());
  auto returned = builder.ret(answer); if (!returned) return failed(returned.error());
  auto text = serialization::write_text(context); if (!text) return failed(text.error());
  auto binary = serialization::write_binary(context); if (!binary) return failed(binary.error());
  auto decoded = serialization::read_binary(*binary); if (!decoded) return failed(decoded.error());
  std::cout << *text; return (*decoded)->verify() ? 0 : 1;
}
