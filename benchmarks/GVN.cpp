/* Licensed under the MIT License. Isolated GVN timings and checked output fingerprints. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
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
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::analysis;
  using namespace mirnext::transforms;
  constexpr unsigned width = 8, repeats = 7;
  for (bool writes : {false, true})
    for (unsigned count : {8, 32, 128}) {
      std::string text
        = "m: module\nentry: func i64, i64:p, i64:condition\nlocal i64:sum, i64:value\nmov sum, "
          "0\n";
      for (unsigned n = 0; n < count; ++n) {
        for (unsigned k = 0; k < width; ++k) {
          auto offset = std::to_string ((n * width + k) * 8), name = "a" + std::to_string (k);
          text += "mov value, i64:" + offset + "(p):" + name + "\nadd sum, sum, value\n";
          if (writes && k == 0) text += "mov i64:" + offset + "(p):" + name + ", sum\n";
        }
        text += "bt done, condition\n";
      }
      text += "done:\nret sum\nendfunc\nendmodule\n";
      std::vector<double> times;
      std::vector<std::size_t> allocation_counts;
      std::size_t changes = 0;
      std::uint64_t fingerprint = 0;
      for (unsigned repetition = 0; repetition < repeats; ++repetition) {
        auto parsed = serialization::parse_text (text);
        if (!(parsed).has_value ()) {
          std::cerr << parsed.error ().message << '\n';
          return 1;
        }
        auto &function = *parsed.value ().modules ().begin ()->functions ().begin ();
        FunctionAnalysisManager analyses (function);
        FunctionPassManager prepare;
        prepare.add<SimplifyPass> ();
        prepare.add<PrepareCFGPass> ();
        prepare.add<BuildSSAPass> ();
        auto prepared = prepare.run (function, analyses);
        if (!(prepared).has_value ()) {
          std::cerr << prepared.error ().message << '\n';
          return 1;
        }
        allocations = 0;
        measuring = true;
        auto start = std::chrono::steady_clock::now ();
        auto result = ::mirnext::transforms::detail::run_pass (GVNPass (), function, analyses);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!(result).has_value ()) {
          std::cerr << result.error ().message << '\n';
          return 1;
        }
        if (!(function.verify ()).has_value ()) return 1;
        auto snapshot = dump (function);
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : snapshot) hash = (hash ^ c) * 1099511628211ULL;
        if (repetition && (fingerprint != hash || changes != result.value ().transformations)) {
          std::cerr << "unstable output blocks=" << count << " fingerprint=" << fingerprint << '/'
                    << hash << " changes=" << changes << '/' << result.value ().transformations
                    << '\n';
          return 1;
        }
        allocation_counts.push_back (allocations);
        fingerprint = hash;
        changes = result.value ().transformations;
        times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
      }
      std::ranges::sort (times);
      std::ranges::sort (allocation_counts);
      std::cout << "shape=memory-availability writes=" << writes << " blocks=" << count
                << " width=" << width << " repetitions=" << repeats
                << " median_ms=" << times[repeats / 2]
                << " median_allocations=" << allocation_counts[repeats / 2]
                << " changes=" << changes << " fingerprint=" << fingerprint << '\n';
    }
}
