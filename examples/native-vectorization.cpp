/* Licensed under the MIT License. Opt-in scalar SLP vectorization. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::execution;
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("slp");
  b.create_function<std::int64_t (float *, const float *, const float *)> (module, "add4");
  auto destination = b.argument (0), left = b.argument (1), right = b.argument (2);
  std::array<Value, 4> sums;
  auto f32 = Type (Type::Kind::F32);
  // Read the complete pack first: no no-alias promise is required, and output
  // may overlap either input. Every lane keeps its original FP operation order.
  for (unsigned lane = 0; lane < 4; ++lane)
    sums[lane] = b.create_add (b.create_load (b.memory (f32, left, lane * 4)),
                               b.create_load (b.memory (f32, right, lane * 4)));
  for (unsigned lane = 0; lane < 4; ++lane)
    b.create_store (b.memory (f32, destination, lane * 4), sums[lane]);
  b.create_return (b.constant_i64 (0));
  auto built = b.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  transforms::OptimizationOptions options;
  options.vectorization.slp = true;
  options.observer = [] (const transforms::OptimizationSnapshot &s) -> Result<void> {
    if (s.statistics.pass == transforms::PassId::SLPVectorize
        && s.phase == transforms::PassEvent::Phase::After)
      std::cout << "SLP packs: " << s.statistics.transformations << '\n';
    return {};
  };
  auto session = ExecutionSession::create (std::move (program), {.optimization = options});
  if (!session.has_value ()) {
    std::cerr << session.error ().message << '\n';
    return 1;
  }
  std::array<float, 4> x{1, 2, 3, 4}, y{10, 20, 30, 40}, output{};
  auto result
    = session.value ()
        .call_i64 ("slp", "add4",
                   {static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (output.data ())),
                    static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (x.data ())),
                    static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (y.data ()))});
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  for (unsigned lane = 0; lane < 4; ++lane)
    if (output[lane] != x[lane] + y[lane]) return 1;
  std::cout << "add4: " << output[0] << ", " << output[1] << ", " << output[2] << ", " << output[3]
            << '\n';
}
