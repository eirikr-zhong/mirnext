/* Licensed under the MIT License. Portable v128 through the explicit MIRBuilder. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>

int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Program program;
  MIRBuilder builder (program);
  auto module = builder.create_module ("simd");
  auto type = Type::vector128 (Type::VectorElement::I32, 4);
  builder.create_function<std::int64_t (const std::int32_t *)> (module, "example");
  auto loaded = builder.create_load (builder.memory (type, builder.argument (0)));
  auto tens = builder.create_vector_splat (type, builder.constant_i64 (10));
  auto sum = builder.create_add (loaded, tens);
  sum = builder.create_mul (sum, builder.create_vector_splat (type, builder.constant_i64 (3)));
  auto reversed = builder.create_shuffle (sum, sum, {3, 2, 1, 0});
  auto lane = builder.create_convert (builder.create_extract_lane (reversed, 0), Type::i64 ());
  builder.create_return (lane);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  auto session = ExecutionSession::create (std::move (program));
  if (!session.has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  std::array<std::int32_t, 4> input{1, 2, 3, 4};
  auto result = session.value ().call_i64 ("simd", "example",
                                           {std::bit_cast<std::int64_t> (
                                             reinterpret_cast<std::uintptr_t> (input.data ()))});
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  std::cout << "v128 result = " << result.value () << '\n';
  return result.value () == 42 ? 0 : 1;
}
