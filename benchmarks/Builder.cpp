/* Licensed under the MIT License. Construction time and allocation comparison. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/Builder.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
std::size_t allocations = 0;
}
void *operator new (std::size_t size) {
  if (auto p = std::malloc (size ? size : 1)) {
    ++allocations;
    return p;
  }
  std::abort ();
}
void *operator new[] (std::size_t size) { return ::operator new (size); }
void operator delete (void *p) noexcept { std::free (p); }
void operator delete[] (void *p) noexcept { std::free (p); }

namespace {
using namespace mirnext;
using namespace mirnext::ir;
struct Sample {
  double ns;
  std::size_t allocations;
};
using Clock = std::chrono::steady_clock;
Sample checked (unsigned count, bool calls) {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("bench");
  MIRNEXT_RESULT_FAULT (m);
  auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
  MIRNEXT_RESULT_FAULT (f);
  auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
  MIRNEXT_RESULT_FAULT (block);
  auto callee = mirnext::ir::detail::access (((m)).value ())
                  .import ("callee", signature<std::int64_t (std::int64_t)> ());
  MIRNEXT_RESULT_FAULT (callee);
  InstructionBuilder builder ((block).value ());
  auto current = builder.literal (0);
  const auto before = allocations;
  auto start = Clock::now ();
  for (unsigned i = 0; i < count; ++i) {
    if (calls) {
      // Compare typed inputs on both paths, including constant validation.
      auto r = builder.call ((callee).value (), {builder.literal (42)});
      MIRNEXT_RESULT_FAULT (r);
    } else
      current = current + 1;
  }
  auto end = Clock::now ();
  auto used = allocations - before;
  MIRNEXT_RESULT_FAULT (builder.ret ());
  MIRNEXT_RESULT_FAULT (context.verify ());
  if (f.value ().instruction_count () != count + 1) std::abort ();
  return {std::chrono::duration<double, std::nano> (end - start).count (), used};
}
Sample explicit_builder (unsigned count, bool calls) {
  Program program;
  MIRBuilder builder (program);
  auto *m = builder.create_module ("bench");
  auto *f = builder.create_function<void ()> (m, "entry");
  auto *callee = builder.create_import<std::int64_t (std::int64_t)> (m, "callee");
  auto current = builder.constant_i64 (0);
  const auto before = allocations;
  auto start = Clock::now ();
  for (unsigned i = 0; i < count; ++i) {
    if (calls)
      builder.create_call (callee, {builder.constant_i64 (42)});
    else
      current = builder.create_add (current, builder.constant_i64 (1));
  }
  auto end = Clock::now ();
  auto used = allocations - before;
  builder.create_return ();
  auto context = builder.finish ();
  MIRNEXT_RESULT_FAULT (context);
  if (f->instruction_count () != count + 1) std::abort ();
  return {std::chrono::duration<double, std::nano> (end - start).count (), used};
}
}  // namespace
int main () {
  std::cout
    << "kind,count,checked_ns_per_op,explicit_ns_per_op,checked_allocations,explicit_allocations\n";
  for (bool calls : {false, true})
    for (unsigned count : {4096, 16384}) {
      std::array<Sample, 5> old, modern;
      for (unsigned trial = 0; trial < old.size (); ++trial) {
        old[trial] = checked (count, calls);
        modern[trial] = explicit_builder (count, calls);
        // Timing is informational; equivalent construction must not add per-op allocations.
        if (modern[trial].allocations != old[trial].allocations) {
          std::cerr << (calls ? "call" : "expression") << " count=" << count
                    << " checked allocations=" << old[trial].allocations
                    << " explicit allocations=" << modern[trial].allocations << '\n';
          return 1;
        }
      }
      auto order = [] (Sample a, Sample b) { return a.ns < b.ns; };
      std::sort (old.begin (), old.end (), order);
      std::sort (modern.begin (), modern.end (), order);
      std::cout << (calls ? "call" : "expression") << ',' << count << ',' << old[2].ns / count
                << ',' << modern[2].ns / count << ',' << old[2].allocations << ','
                << modern[2].allocations << '\n';
    }
}
