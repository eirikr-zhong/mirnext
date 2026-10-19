/* Licensed under the MIT License. Isolated DSE timings with stable SSA fingerprints. */
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/Simplify.hpp"
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
  constexpr unsigned width = 8, repeats = 7;
  for (bool distinct : {false, true})
    for (unsigned count : {8, 32, 128}) {
      std::string text
        = "m: module\nentry: func i64, i64:p, i64:condition\nlocal i64:sum, i64:value\nmov sum, "
          "0\n";
      for (unsigned n = 0; n < count; ++n) {
        for (unsigned k = 0; k < width; ++k) {
          auto offset = std::to_string (((distinct ? n * width : 0) + k) * 8),
               name = "a" + std::to_string (k);
          text += "mov value, i64:" + offset + "(p):" + name
                  + "\nadd sum, sum, value\nmov i64:" + offset + "(p):" + name + ", sum\n";
        }
        text += "bt done, condition\n";
      }
      text += "done:\nret sum\nendfunc\nendmodule\n";
      std::vector<double> times;
      std::vector<std::size_t> counts;
      std::size_t changes = 0;
      std::uint64_t fingerprint = 0;
      for (unsigned repetition = 0; repetition < repeats; ++repetition) {
        auto parsed = serialization::parse_text (text);
        if (!parsed) return 1;
        auto &function = *(*parsed)->modules ().begin ()->functions ().begin ();
        FunctionAnalysisManager analyses (function);
        FunctionPassManager prepare;
        prepare.add<SimplifyPass> ();
        prepare.add<PrepareCFGPass> ();
        prepare.add<BuildSSAPass> ();
        prepare.add<GVNPass> ();
        prepare.add<CopyPropagationPass> ();
        auto ready = prepare.run (function, analyses);
        if (!ready) {
          std::cerr << ready.error ().message << '\n';
          return 1;
        }
        allocations = 0;
        measuring = true;
        auto start = std::chrono::steady_clock::now ();
        auto result = DeadStoresPass ().run (function, analyses);
        auto end = std::chrono::steady_clock::now ();
        measuring = false;
        if (!result) {
          std::cerr << result.error ().message << '\n';
          return 1;
        }
        if (!function.verify ()) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : dump (function)) hash = (hash ^ c) * 1099511628211ULL;
        for (auto &use : function.ssa_form ()->uses ())
          if (use.alive ()) {
            hash = (hash ^ use.value ().operand ()) * 1099511628211ULL;
            for (unsigned char c : use.value ().reg ().name ())
              hash = (hash ^ c) * 1099511628211ULL;
          }
        if (repetition && (fingerprint != hash || changes != result->transformations)) return 2;
        fingerprint = hash;
        changes = result->transformations;
        counts.push_back (allocations);
        times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
      }
      std::ranges::sort (times);
      std::ranges::sort (counts);
      std::cout << "shape=memory-liveness distinct=" << distinct << " blocks=" << count
                << " width=" << width << " repetitions=" << repeats
                << " median_ms=" << times[repeats / 2]
                << " median_allocations=" << counts[repeats / 2] << " changes=" << changes
                << " fingerprint=" << fingerprint << '\n';
    }
}
