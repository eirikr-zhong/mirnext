/* Licensed under the MIT License. Build, clone and simplify canonical IR directly. */
#include <mirnext/Transforms/Simplify.hpp>
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main() {
  using namespace mirnext; using namespace mirnext::ir;
  auto failed = [](const Error &error) { std::cerr << error.message << '\n'; return 1; };
  Context context; auto module = context.create_module("simplify"); if (!module) return failed(module.error());
  auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return failed(function.error());
  auto block = (*function)->create_block(); if (!block) return failed(block.error()); IRBuilder builder(**block);
  auto returned = builder.ret((builder.literal(20) + 22) * 1); if (!returned) return failed(returned.error());
  auto original = dump(context); auto copied = clone(context); if (!copied) return failed(copied.error());
  auto &copy_function = *(*copied)->modules().begin()->functions().begin();
  analysis::FunctionAnalysisManager analyses(copy_function); transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass>();
  auto result = passes.run(copy_function, analyses, [](const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After) std::cout << event.pass << ": " << event.instructions_before
      << " -> " << event.instructions_after << " instructions, " << event.transformations << " transformations\n";
    return {};
  }); if (!result) return failed(result.error());
  if (dump(context) != original) return 1;
  auto text = serialization::write_text(**copied); if (!text) return failed(text.error());
  auto binary = serialization::write_binary(**copied); if (!binary) return failed(binary.error());
  auto decoded = serialization::read_binary(*binary); if (!decoded) return failed(decoded.error());
  std::cout << *text; return (*decoded)->verify() ? 0 : 1;
}
