/* Licensed under the MIT License. Whole-program passes on independently owned IR. */
#include "mirnext/IR/Builder.hpp"
#include <mirnext/Transforms/Simplify.hpp>
#include <mirnext/Transforms/Inline.hpp>
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  auto failed = [] (const Error &error) {
    std::cerr << error.message << '\n';
    return 1;
  };
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("inline");
  auto *leaf = builder.create_function<std::int64_t (std::int64_t)> (module, "add_two");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (2)));
  builder.create_function<std::int64_t ()> (module, "entry");
  auto answer = builder.create_call (leaf, {builder.constant_i64 (40)});
  builder.create_return (answer.value ());
  auto built = builder.finish ();
  if (!built.has_value ()) return failed (built.error ());
  auto original = dump (context);
  auto copied = clone (context);
  if (!copied.has_value ()) return failed (copied.error ());
  transforms::ModulePassManager passes;
  passes.add_function<transforms::SimplifyPass> ();
  passes.add_function<transforms::InlinePass> ();
  auto result = passes.run (
    copied.value (),
    [] (const transforms::ModulePassEvent &event) -> Result<void> {
      if (event.phase == transforms::PassEvent::Phase::After)
        std::cout << "module " << event.module.name () << " [" << event.pass
                  << "]: " << event.transformations << " transformations\n";
      return {};
    },
    [] (const transforms::PassEvent &event) -> Result<void> {
      if (event.phase == transforms::PassEvent::Phase::After)
        std::cout << event.function.symbol ().name () << " [" << event.pass
                  << "]: " << event.instructions_before << " -> " << event.instructions_after
                  << '\n';
      return {};
    });
  if (!result.has_value ()) return failed (result.error ());
  if (dump (context) != original) return 1;
  auto text = serialization::write_text (copied.value ());
  if (!text.has_value ()) return failed (text.error ());
  auto binary = serialization::write_binary (copied.value ());
  if (!binary.has_value ()) return failed (binary.error ());
  auto decoded = serialization::read_binary (binary.value ());
  if (!decoded.has_value ()) return failed (decoded.error ());
  std::cout << text.value ();
  return (decoded.value ().verify ()).has_value () ? 0 : 1;
}
