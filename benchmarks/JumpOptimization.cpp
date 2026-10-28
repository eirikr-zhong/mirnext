/* Licensed under the MIT License. Isolated jump cleanup timings and ordinary allocations. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
        Program context;
        auto module = mirnext::ir::detail::access (context).create_module ("branches");
        if (!(module).has_value ()) return 1;
        auto function
          = mirnext::ir::detail::access (((module)).value ())
              .create_function ("entry", {{Type::i64 ()}, {Type::i64 (), Type::i64 ()}});
        if (!(function).has_value ()) return 1;
        std::vector<BasicBlock *> blocks;
        std::vector<Label *> labels;
        for (unsigned n = 0; n < count; ++n) {
          auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
          if (!(block).has_value ()) return 1;
          blocks.push_back (&(block).value ());
          if (n) {
            auto label
              = mirnext::ir::detail::access (((function)).value ()).create_label ((block).value ());
            if (!(label).has_value ()) return 1;
            labels.push_back (&(label).value ());
          } else
            labels.push_back (nullptr);
        }
        auto condition
          = mirnext::ir::detail::ReferenceAccess::pointers (function.value ().arguments ())[0],
          sum = mirnext::ir::detail::ReferenceAccess::pointers (function.value ().arguments ())[1];
        for (unsigned n = 0; n < count; ++n) {
          InstructionBuilder builder (*blocks[n]);
          if (chains && n % 4 == 2 && n + 2 < count) {
            if (!(builder.jump (*labels[n + 2])).has_value ()) return 1;
            continue;
          }
          if (!(builder.emit (Opcode::Add,
                              {Operand (*sum), Operand (*sum), Operand (std::int64_t{1})}))
                 .has_value ())
            return 1;
          if (n + 2 < count) {
            if (!(builder.emit (Opcode::Bt, {Operand (*labels[n + 2]), Operand (*condition)}))
                   .has_value ())
              return 1;
          } else if (!(builder.ret (Operand (*sum))).has_value ())
            return 1;
        }
        FunctionAnalysisManager analyses ((function).value ());
        if (!(analyses.get<FunctionGraphAnalysis> ()).has_value ()) return 1;
        allocations = 0;
        measuring = true;
        auto start = std::chrono::steady_clock::now ();
        auto result = ::mirnext::transforms::detail::run_pass (JumpOptimizationPass (),
                                                               (function).value (), analyses);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!(result).has_value () || !(function.value ().verify ()).has_value ()) return 1;
        auto graph = analyses.get<FunctionGraphAnalysis> ();
        if (!(graph).has_value ()) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        auto word = [&] (std::size_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        for (unsigned char c : dump ((function).value ())) word (c);
        for (std::size_t b = 0; b < graph.value ().blocks.size (); ++b) {
          word (b);
          word (graph.value ().blocks[b].address_taken);
          for (auto id : graph.value ().incoming[b]) word (graph.value ().edges[id].source);
          word (SIZE_MAX);
          for (auto id : graph.value ().outgoing[b]) {
            auto &e = graph.value ().edges[id];
            word (e.destination);
            word (static_cast<unsigned> (e.kind));
            word (e.operand);
          }
          word (SIZE_MAX);
        }
        if (rep && (hash != fingerprint || result.value ().transformations != changes)) return 2;
        fingerprint = hash;
        changes = result.value ().transformations;
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
