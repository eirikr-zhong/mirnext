/* Licensed under the MIT License. Build, clone and simplify canonical IR directly. */
#include "mirnext/IR/Builder.hpp"
#include <mirnext/Transforms/Simplify.hpp>
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
  auto *module = builder.create_module ("simplify");
  builder.create_function<std::int64_t ()> (module, "entry");
  auto sum = builder.create_add (builder.constant_i64 (20), builder.constant_i64 (22));
  builder.create_return (builder.create_mul (sum, builder.constant_i64 (1)));
  auto built = builder.finish ();
  if (!built.has_value ()) return failed (built.error ());
  auto original = dump (context);
  auto copied = clone (context);
  if (!copied.has_value ()) return failed (copied.error ());
  auto &copy_function = *copied.value ().modules ().begin ()->functions ().begin ();
  analysis::FunctionAnalysisManager analyses (copy_function);
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  auto result
    = passes.run (copy_function, analyses, [] (const transforms::PassEvent &event) -> Result<void> {
        if (event.phase == transforms::PassEvent::Phase::After)
          std::cout << event.pass << ": " << event.instructions_before << " -> "
                    << event.instructions_after << " instructions, " << event.transformations
                    << " transformations\n";
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
