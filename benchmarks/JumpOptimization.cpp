/* Licensed under the MIT License. Isolated jump cleanup timings and ordinary allocations. */
#include "mirnext/Transforms/JumpOptimization.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
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
  using namespace mirnext::analysis;
  using namespace mirnext::transforms;
  constexpr unsigned repetitions = 7;
  for (bool chains : {false, true})
    for (unsigned count : {1024, 8192}) {
      std::vector<double> times;
      std::vector<std::size_t> counts;
      std::uint64_t fingerprint = 0;
      std::size_t changes = 0;
      for (unsigned rep = 0; rep < repetitions; ++rep) {
        Context context;
        auto module = context.create_module ("branches");
        if (!module) return 1;
        auto function
          = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 (), Type::i64 ()}});
        if (!function) return 1;
        std::vector<BasicBlock *> blocks;
        std::vector<Label *> labels;
        for (unsigned n = 0; n < count; ++n) {
          auto block = (*function)->create_block ();
          if (!block) return 1;
          blocks.push_back (*block);
          if (n) {
            auto label = (*function)->create_label (**block);
            if (!label) return 1;
            labels.push_back (*label);
          } else
            labels.push_back (nullptr);
        }
        auto condition = (*function)->arguments ()[0], sum = (*function)->arguments ()[1];
        for (unsigned n = 0; n < count; ++n) {
          IRBuilder builder (*blocks[n]);
          if (chains && n % 4 == 2 && n + 2 < count) {
            if (!builder.jump (*labels[n + 2])) return 1;
            continue;
          }
          if (!builder.emit (Opcode::Add,
                             {Operand (*sum), Operand (*sum), Operand (std::int64_t{1})}))
            return 1;
          if (n + 2 < count) {
            if (!builder.emit (Opcode::Bt, {Operand (*labels[n + 2]), Operand (*condition)}))
              return 1;
          } else if (!builder.ret (Operand (*sum)))
            return 1;
        }
        FunctionAnalysisManager analyses (**function);
        if (!analyses.get<FunctionGraphAnalysis> ()) return 1;
        allocations = 0;
        measuring = true;
        auto start = std::chrono::steady_clock::now ();
        auto result = JumpOptimizationPass ().run (**function, analyses);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!result || !(*function)->verify ()) return 1;
        auto graph = analyses.get<FunctionGraphAnalysis> ();
        if (!graph) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        auto word = [&] (std::size_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        for (unsigned char c : dump (**function)) word (c);
        for (std::size_t b = 0; b < (*graph)->blocks.size (); ++b) {
          word (b);
          word ((*graph)->blocks[b].address_taken);
          for (auto id : (*graph)->incoming[b]) word ((*graph)->edges[id].source);
          word (SIZE_MAX);
          for (auto id : (*graph)->outgoing[b]) {
            auto &e = (*graph)->edges[id];
            word (e.destination);
            word (static_cast<unsigned> (e.kind));
            word (e.operand);
          }
          word (SIZE_MAX);
        }
        if (rep && (hash != fingerprint || result->transformations != changes)) return 2;
        fingerprint = hash;
        changes = result->transformations;
        times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
        counts.push_back (allocations);
      }
      std::ranges::sort (times);
      std::ranges::sort (counts);
      std::cout << "shape=jump-cleanup chains=" << chains << " blocks=" << count
                << " repetitions=" << repetitions << " median_ms=" << times[repetitions / 2]
                << " median_allocations=" << counts[repetitions / 2] << " changes=" << changes
                << " fingerprint=" << fingerprint << '\n';
    }
}
