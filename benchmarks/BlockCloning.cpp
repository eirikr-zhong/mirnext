/* Licensed under the MIT License. Informational timings without performance thresholds. */
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
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
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
int main () {
  constexpr unsigned width = 128, repeats = 7;
  for (unsigned count : {8, 32, 128}) {
    std::string text = "m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\n";
    for (unsigned n = 0; n < count; ++n)
      text += "beq cold" + std::to_string (n) + ", c, " + std::to_string (n + 1) + "\n";
    text += "hot:\n";
    for (unsigned n = 0; n < width; ++n) text += "add v, v, 1\n";
    text += "blt hot, v, 128\nret v\n";
    for (unsigned n = 0; n < count; ++n)
      text += "cold" + std::to_string (n) + ":\nmov v, 0\njmp hot\n";
    text += "endfunc\nendmodule\n";
    std::vector<double> times;
    std::size_t pass_allocations = 0;
    for (unsigned n = 0; n < repeats; ++n) {
      auto parsed = serialization::parse_text (text);
      if (!parsed) return 1;
      auto &function = *(*parsed)->modules ().begin ()->functions ().begin ();
      FunctionAnalysisManager analyses (function);
      FunctionPassManager prepare;
      prepare.add<PrepareCFGPass> ();
      if (!prepare.run (function, analyses)) return 1;
      auto before = function.instruction_count ();
      allocations = 0;
      measuring = true;
      auto start = std::chrono::steady_clock::now ();
      auto result = BlockCloningPass ().run (function, analyses);
      auto end = std::chrono::steady_clock::now ();
      measuring = false;
      if (n && pass_allocations != allocations) return 1;
      pass_allocations = allocations;
      if (!result || result->transformations != count || !function.verify ()
          || function.instruction_count () != before + count * (width + 1))
        return 1;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "shape=hot-clones roots=" << count << " width=" << width
              << " repetitions=" << repeats << " median_ms=" << times[repeats / 2] << '\n';
    std::cout << "pass_allocations=" << pass_allocations << '\n';
  }
}
