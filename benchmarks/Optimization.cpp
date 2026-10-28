/* Licensed under the MIT License. Complete owning-pipeline costs and allocations. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
bool measuring = false;
std::size_t allocations = 0;
}  // namespace
void *operator new (std::size_t size) {
  if (measuring) ++allocations;
  if (auto memory = std::malloc (size ? size : 1)) return memory;
  std::abort ();
}
void *operator new[] (std::size_t size) { return ::operator new (size); }
void operator delete (void *memory) noexcept { std::free (memory); }
void operator delete[] (void *memory) noexcept { ::operator delete (memory); }
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::transforms;
  constexpr unsigned repetitions = 7;
  for (unsigned count : {64, 512})
    for (auto level : {OptimizationLevel::O0, OptimizationLevel::O2}) {
      Program source;
      auto module = mirnext::ir::detail::access (source).create_module (std::string (80, 'm'));
      if (!(module).has_value ()) return 1;
      for (unsigned n = 0; n < count; ++n) {
        auto f = mirnext::ir::detail::access (((module)).value ())
                   .create_function (std::string (80, 'f') + std::to_string (n),
                                     {{Type::i64 ()}, {Type::i64 ()}});
        if (!(f).has_value ()) return 1;
        auto a = mirnext::ir::detail::access (((f)).value ()).create_block (),
             b = mirnext::ir::detail::access (((f)).value ()).create_block (),
             c = mirnext::ir::detail::access (((f)).value ()).create_block (),
             d = mirnext::ir::detail::access (((f)).value ()).create_block ();
        if (!(a).has_value () || !(b).has_value () || !(c).has_value () || !(d).has_value ())
          return 1;
        auto yes = mirnext::ir::detail::access (((f)).value ()).create_label ((c).value ()),
             done = mirnext::ir::detail::access (((f)).value ()).create_label ((d).value ());
        auto r = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
        if (!(yes).has_value () || !(done).has_value () || !(r).has_value ()) return 1;
        if (!(InstructionBuilder ((a).value ())
                .emit (Opcode::Bt, {Operand ((yes).value ()),
                                    Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                                      f.value ().arguments ())[0])}))
               .has_value ()
            || !(InstructionBuilder ((b).value ()).assign ((r).value (), 20)).has_value ()
            || !(InstructionBuilder ((b).value ()).jump ((done).value ())).has_value ()
            || !(InstructionBuilder ((c).value ()).assign ((r).value (), 22)).has_value ()
            || !(InstructionBuilder ((d).value ()).ret (Operand ((r).value ()))).has_value ())
          return 1;
      }
      std::vector<double> times;
      std::vector<std::size_t> counts;
      std::uint64_t fingerprint = 0;
      std::size_t changes = 0, stages = 0;
      for (unsigned rep = 0; rep < repetitions; ++rep) {
        OptimizationOptions options;
        options.level = level;
        allocations = 0;
        measuring = true;
        auto start = std::chrono::steady_clock::now ();
        auto result = optimize (source, options);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!(result).has_value ()) {
          std::cerr << result.error ().message << '\n';
          return 1;
        }
        if (!(result.value ().verify ()).has_value ()) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        auto word = [&] (std::size_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        for (unsigned char c : result.value ().dump ()) word (c);
        std::size_t transformed = 0;
        for (auto &stage : result.value ().statistics ()) {
          word (static_cast<unsigned> (stage.pass));
          word (stage.instructions_before);
          word (stage.instructions_after);
          word (stage.transformations);
          transformed += stage.transformations;
        }
        if (rep
            && (hash != fingerprint || changes != transformed
                || stages != result.value ().statistics ().size ()))
          return 2;
        fingerprint = hash;
        changes = transformed;
        stages = result.value ().statistics ().size ();
        times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
        counts.push_back (allocations);
      }
      std::ranges::sort (times);
      std::ranges::sort (counts);
      std::cout << "shape=owning-pipeline functions=" << count
                << " level=" << static_cast<unsigned> (level) << " repetitions=" << repetitions
                << " median_ms=" << times[repetitions / 2]
                << " median_allocations=" << counts[repetitions / 2] << " stages=" << stages
                << " changes=" << changes << " fingerprint=" << fingerprint << '\n';
    }
}
