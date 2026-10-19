/* Licensed under the MIT License. Whole-program passes on independently owned IR. */
#include <mirnext/Transforms/Simplify.hpp>
#include <mirnext/Transforms/Inline.hpp>
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main() {
  using namespace mirnext; using namespace mirnext::ir;
  auto failed = [](const Error &error) { std::cerr << error.message << '\n'; return 1; };
  Context context; auto module = context.create_module("inline"); if (!module) return failed(module.error());
  auto leaf = (*module)->create_function("add_two", {{Type::i64()}, {Type::i64()}}); if (!leaf) return failed(leaf.error());
  auto block = (*leaf)->create_block(); if (!block) return failed(block.error()); IRBuilder body(**block);
  auto returned = body.ret(body.expression(*(*leaf)->arguments()[0]) + 2); if (!returned) return failed(returned.error());
  auto entry = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!entry) return failed(entry.error());
  block = (*entry)->create_block(); if (!block) return failed(block.error()); IRBuilder caller(**block);
  auto call = caller.call(**leaf, {40}); if (!call) return failed(call.error());
  auto value = call->value(); if (!value) return failed(value.error());
  returned = caller.ret(Operand(**value)); if (!returned) return failed(returned.error());
  auto original = dump(context); auto copied = clone(context); if (!copied) return failed(copied.error());
  transforms::ModulePassManager passes; passes.add_function<transforms::SimplifyPass>(); passes.add_function<transforms::InlinePass>();
  auto result = passes.run(**copied, [](const transforms::ModulePassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After) std::cout << "module " << event.module.name()
      << " [" << event.pass << "]: " << event.transformations << " transformations\n";
    return {};
  }, [](const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After) std::cout << event.function.symbol().name() << " [" << event.pass
      << "]: " << event.instructions_before << " -> " << event.instructions_after << '\n';
    return {};
  }); if (!result) return failed(result.error());
  if (dump(context) != original) return 1;
  auto text = serialization::write_text(**copied); if (!text) return failed(text.error());
  auto binary = serialization::write_binary(**copied); if (!binary) return failed(binary.error());
  auto decoded = serialization::read_binary(*binary); if (!decoded) return failed(decoded.error());
  std::cout << *text; return (*decoded)->verify() ? 0 : 1;
}
