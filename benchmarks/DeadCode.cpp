/* Licensed under the MIT License. Isolated SSA DCE timings and fingerprints. */
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
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
  constexpr unsigned repeats = 7;
  for (bool chain : {false, true})
    for (unsigned count : {1024, 8192}) {
      std::string text = "m: module\nentry: func i64, i64:x\nlocal i64:a0";
      for (unsigned p = 1; p < count; ++p) text += ", i64:a" + std::to_string (p);
      text += '\n';
      for (unsigned p = 0; p < count; ++p) {
        auto input = chain && p ? "a" + std::to_string (p - 1) : "x";
        text += "add a" + std::to_string (p) + ", " + input + ", " + input + '\n';
      }
      text += "ret x\nendfunc\nendmodule\n";
      std::vector<double> times;
      std::vector<std::size_t> counts;
      std::size_t changes = 0;
      std::uint64_t fingerprint = 0;
      for (unsigned rep = 0; rep < repeats; ++rep) {
        auto parsed = serialization::parse_text (text);
        if (!parsed) return 1;
        auto &function = *(*parsed)->modules ().begin ()->functions ().begin ();
        FunctionAnalysisManager analyses (function);
        FunctionPassManager prepare;
        prepare.add<PrepareCFGPass> ();
        prepare.add<BuildSSAPass> ();
        auto ready = prepare.run (function, analyses);
        if (!ready) {
          std::cerr << ready.error ().message << '\n';
          return 1;
        }
        measuring = true;
        allocations = 0;
        auto start = std::chrono::steady_clock::now ();
        auto result = SSADeadCodePass ().run (function, analyses);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!result || !function.verify ()) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : dump (function)) hash = (hash ^ c) * 1099511628211ULL;
        for (unsigned char c : dump_ssa (function)) hash = (hash ^ c) * 1099511628211ULL;
        if (rep && (hash != fingerprint || result->transformations != changes)) return 2;
        fingerprint = hash;
        changes = result->transformations;
        times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
        counts.push_back (allocations);
      }
      std::ranges::sort (times);
      std::ranges::sort (counts);
      std::cout << "shape=dead-ssa chain=" << chain << " definitions=" << count
                << " repetitions=" << repeats << " median_ms=" << times[repeats / 2]
                << " median_allocations=" << counts[repeats / 2] << " changes=" << changes
                << " fingerprint=" << fingerprint << '\n';
    }
}
