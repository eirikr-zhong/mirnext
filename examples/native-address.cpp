/* Licensed under the MIT License. Address lowering rewrites canonical instructions. */
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main() {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context; auto module = context.create_module("address"); if (!module) return 1;
  auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return 1;
  auto block = (*function)->create_block(); if (!block) return 1;
  auto variable = (*function)->create_register(Type::i64(), "value"); if (!variable) return 1;
  IRBuilder body(**block); if (!body.assign(**variable, 7)) return 1;
  auto pointer = body.address_of(**variable); if (!pointer) return 1;
  const Type byte(Type::Kind::I8);
  if (!body.store(Memory{byte, *pointer}, Operand(255))) return 1;
  auto loaded = body.load(Memory{byte, *pointer}); if (!loaded || !body.ret(Operand(**loaded))) return 1;
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass>(); passes.add<transforms::PrepareCFGPass>();
  passes.add<transforms::BuildSSAPass>(false); passes.add<transforms::AddressTransformPass>();
  passes.add<transforms::BuildSSAPass>(); passes.add<transforms::ConventionalSSAPass>(); passes.add<transforms::DestroySSAPass>();
  auto status = passes.run(context, [](const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After) std::cout << event.pass << '\n' << dump(event.function);
    return {};
  });
  if (!status) { std::cerr << status.error().message << '\n'; return 1; }
  auto binary = serialization::write_binary(context);
  if (!binary) { std::cerr << binary.error().message << '\n'; return 1; }
}
