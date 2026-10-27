/* Licensed under the MIT License. Canonical interpreter preparation and dispatch. */
#include "mirnext/Execution/Interpreter.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
using namespace mirnext;
using namespace mirnext::execution;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed (Clock::time_point start) {
  return std::chrono::duration<double, std::nano> (Clock::now () - start).count ();
}
Result<void> measure (std::int64_t count, bool native_entry = false) {
  auto source = serialization::parse_text (
    "benchmark:module\nsum:func i64,i64:n\nlocal i64:r,i64:index\nmov r,0\nmov index,0\nbeq "
    "done,n,0\nloop:add r,r,index\nadd index,index,1\nblt loop,index,n\ndone:ret "
    "r\nendfunc\nendmodule\n");
  if (!source) return source.error ();
  constexpr unsigned iterations = 500;
  std::array<double, 7> preparation, calls;
  std::array arguments{count};
  auto expected = count * (count - 1) / 2;
  for (unsigned trial = 0; trial < calls.size (); ++trial) {
    auto start = Clock::now ();
    auto p = InterpretedProgram::create (**source, {.native_entries = native_entry});
    preparation[trial] = elapsed (start);
    if (!p) return p.error ();
    auto f = p->find_function ("benchmark", "sum");
    if (!f) return f.error ();
    std::optional<NativeEntry> entry;
    if (native_entry) {
      auto e = p->native_entry (*f);
      if (!e) return e.error ();
      entry.emplace (std::move (*e));
    }
    auto call = [&] () -> Result<std::int64_t> {
      if (!entry) return p->call_i64 (*f, arguments);
      ExecutionInvocation invocation;
      auto value = reinterpret_cast<std::int64_t (*) (std::int64_t)> (entry->address ()) (count);
      auto status = invocation.status ();
      if (!status) return status.error ();
      return value;
    };
    auto first = call ();
    if (!first) return first.error ();
    if (*first != expected)
      return Error{ErrorCode::InvalidOperand, "wrong first interpreter result"};
    std::int64_t total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto value = call ();
      if (!value) return value.error ();
      total += *value;
    }
    calls[trial] = elapsed (start) / iterations;
    if (total != expected * iterations)
      return Error{ErrorCode::InvalidOperand, "wrong repeated interpreter results"};
    if (p->native_entry_count () != (native_entry ? 1u : 0u))
      return Error{ErrorCode::InvalidOperand, "unexpected interpreter entry regeneration"};
  }
  auto median = [] (auto values) {
    std::sort (values.begin (), values.end ());
    return values[values.size () / 2];
  };
  std::cout << "entry=" << (native_entry ? "native_abi" : "checked_cpp")
            << " loop_iterations=" << count << " prepare_us=" << median (preparation) / 1e3
            << " call_us=" << median (calls) / 1e3 << '\n';
  return {};
}
}  // namespace
int main () {
  for (auto count : {0, 64, 1024}) {
    auto result = measure (count);
    if (!result) {
      std::cerr << result.error ().message << '\n';
      return 1;
    }
  }
  auto target = target::TargetMachine::native ();
  if (!target) return 1;
  if (target->architecture () == target::Architecture::AArch64)
    for (auto count : {0, 64, 1024}) {
      auto result = measure (count, true);
      if (!result) {
        std::cerr << result.error ().message << '\n';
        return 1;
      }
    }
}
