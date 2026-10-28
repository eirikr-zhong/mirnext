/* Licensed under the MIT License. Address lowering rewrites canonical instructions. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("address");
  builder.create_function<std::int64_t ()> (module, "entry");
  auto value = builder.create_register (Type::i64 (), "value");
  builder.create_assign (value, builder.constant_i64 (7));
  auto pointer = builder.create_address_of (value);
  auto byte = builder.memory (Type (Type::Kind::I8), pointer);
  builder.create_store (byte, builder.constant (Type (Type::Kind::I8), Operand (255)));
  builder.create_return (builder.create_convert (builder.create_load (byte), Type::i64 ()));
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::PrepareCFGPass> ();
  passes.add<transforms::BuildSSAPass> (false);
  passes.add<transforms::AddressTransformPass> ();
  passes.add<transforms::BuildSSAPass> ();
  passes.add<transforms::ConventionalSSAPass> ();
  passes.add<transforms::DestroySSAPass> ();
  auto status = passes.run (context, [] (const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After)
      std::cout << event.pass << '\n' << dump (event.function);
    return {};
  });
  if (!status.has_value ()) {
    std::cerr << status.error ().message << '\n';
    return 1;
  }
  auto binary = serialization::write_binary (context);
  if (!binary.has_value ()) {
    std::cerr << binary.error ().message << '\n';
    return 1;
  }
}
