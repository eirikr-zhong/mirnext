/* Licensed under the MIT License. Complete owning-pipeline costs and allocations. */
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
      Context source;
      auto module = source.create_module (std::string (80, 'm'));
      if (!module) return 1;
      for (unsigned n = 0; n < count; ++n) {
        auto f = (*module)->create_function (std::string (80, 'f') + std::to_string (n),
                                             {{Type::i64 ()}, {Type::i64 ()}});
        if (!f) return 1;
        auto a = (*f)->create_block (), b = (*f)->create_block (), c = (*f)->create_block (),
             d = (*f)->create_block ();
        if (!a || !b || !c || !d) return 1;
        auto yes = (*f)->create_label (**c), done = (*f)->create_label (**d);
        auto r = (*f)->create_register (Type::i64 ());
        if (!yes || !done || !r) return 1;
        if (!IRBuilder (**a).emit (Opcode::Bt, {Operand (**yes), Operand (*(*f)->arguments ()[0])})
            || !IRBuilder (**b).assign (**r, 20) || !IRBuilder (**b).jump (**done)
            || !IRBuilder (**c).assign (**r, 22) || !IRBuilder (**d).ret (Operand (**r)))
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
        if (!result) {
          std::cerr << result.error ().message << '\n';
          return 1;
        }
        if (!result->verify ()) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        auto word = [&] (std::size_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        for (unsigned char c : result->dump ()) word (c);
        std::size_t transformed = 0;
        for (auto &stage : result->statistics ()) {
          word (static_cast<unsigned> (stage.pass));
          word (stage.instructions_before);
          word (stage.instructions_after);
          word (stage.transformations);
          transformed += stage.transformations;
        }
        if (rep
            && (hash != fingerprint || changes != transformed
                || stages != result->statistics ().size ()))
          return 2;
        fingerprint = hash;
        changes = transformed;
        stages = result->statistics ().size ();
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
