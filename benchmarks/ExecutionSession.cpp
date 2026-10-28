/* Licensed under the MIT License. First entry versus cached checked native calls. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Execution/Session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed (Clock::time_point start) {
  return std::chrono::duration<double, std::nano> (Clock::now () - start).count ();
}
Result<void> measure (unsigned count, JITCompilation policy) {
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("benchmark");
  if (!(m).has_value ()) return m.error ();
  auto f = mirnext::ir::detail::access (((m)).value ())
             .create_function ("sum", {{Type::i64 ()}, std::vector<Type> (count, Type::i64 ())});
  if (!(f).has_value ()) return f.error ();
  auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
  if (!(b).has_value ()) return b.error ();
  InstructionBuilder body ((b).value ());
  auto sum = body.literal (Type::i64 (), 0);
  for (auto a : mirnext::ir::detail::ReferenceAccess::pointers (f.value ().arguments ()))
    sum = sum + body.expression (*a);
  auto returned = body.ret (sum);
  if (!(returned).has_value ()) return returned.error ();
  constexpr unsigned iterations = 100000;
  std::vector<std::int64_t> arguments (count, 3);
  std::vector<ExecutionArgument> generic_arguments (count, ExecutionArgument (3));
  std::array<double, 7> linking, first, cached, handle_cached, generic_first, generic_cached,
    handle_generic_cached;
  for (unsigned trial = 0; trial < first.size (); ++trial) {
    auto start = Clock::now ();
    auto s = ExecutionSession::create (c, {.link = {.compilation = policy}});
    linking[trial] = elapsed (start);
    if (!(s).has_value ()) return s.error ();
    if (s.value ().statistics ().compiled_functions != (policy == JITCompilation::Eager ? 1u : 0u))
      return Error{ErrorCode::InvalidOperand, "wrong initial compilation state"};
    auto entry = s.value ().find_function ("benchmark", "sum");
    if (!(entry).has_value ()) return entry.error ();
    start = Clock::now ();
    auto result = s.value ().call_i64 ((entry).value (), arguments);
    first[trial] = elapsed (start);
    if (!(result).has_value ()) return result.error ();
    if ((result).value () != count * 3)
      return Error{ErrorCode::InvalidOperand, "wrong first call result"};
    std::int64_t total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto value = s.value ().call_i64 ((entry).value (), arguments);
      if (!(value).has_value ()) return value.error ();
      total += (value).value ();
    }
    cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s.value ().host_entry_count () != 1)
      return Error{ErrorCode::InvalidOperand,
                   "cached calls changed results or generated another wrapper"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto value = entry.value ().call_i64 (arguments);
      if (!(value).has_value ()) return value.error ();
      total += (value).value ();
    }
    handle_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s.value ().host_entry_count () != 1)
      return Error{ErrorCode::InvalidOperand, "handle calls did not share the scalar adapter"};
    start = Clock::now ();
    auto generic = s.value ().call ((entry).value (), generic_arguments);
    generic_first[trial] = elapsed (start);
    if (!(generic).has_value () || generic.value ().size () != 1)
      return Error{ErrorCode::InvalidOperand, "generic first call failed"};
    auto scalar = ((generic).value ())[0].as_i64 ();
    if (!(scalar).has_value () || (scalar).value () != count * 3)
      return Error{ErrorCode::InvalidOperand, "wrong generic first result"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = s.value ().call ((entry).value (), generic_arguments);
      if (!(values).has_value () || values.value ().size () != 1)
        return Error{ErrorCode::InvalidOperand, "generic cached call failed"};
      auto value = ((values).value ())[0].as_i64 ();
      if (!(value).has_value ()) return value.error ();
      total += (value).value ();
    }
    generic_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s.value ().host_entry_count () != 2)
      return Error{ErrorCode::InvalidOperand,
                   "generic cached calls changed results or generated another adapter"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = entry.value ().call (generic_arguments);
      if (!(values).has_value () || values.value ().size () != 1)
        return Error{ErrorCode::InvalidOperand, "generic handle call failed"};
      auto value = ((values).value ())[0].as_i64 ();
      if (!(value).has_value ()) return value.error ();
      total += (value).value ();
    }
    handle_generic_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s.value ().host_entry_count () != 2)
      return Error{ErrorCode::InvalidOperand, "generic handle calls did not share the adapter"};
  }
  auto median = [] (auto values) {
    std::sort (values.begin (), values.end ());
    return values[values.size () / 2];
  };
  std::cout << "compilation=" << (policy == JITCompilation::Eager ? "eager" : "lazy")
            << " arguments=" << count << " session_create_ms=" << median (linking) / 1e6
            << " first_call_us=" << median (first) / 1e3
            << " cached_checked_call_ns=" << median (cached)
            << " handle_cached_call_ns=" << median (handle_cached)
            << " generic_first_call_us=" << median (generic_first) / 1e3
            << " generic_cached_call_ns=" << median (generic_cached)
            << " handle_generic_cached_call_ns=" << median (handle_generic_cached) << '\n';
  return {};
}
Result<void> measure_variadic (unsigned count, JITCompilation policy) {
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("benchmark");
  if (!(m).has_value ()) return m.error ();
  auto f = mirnext::ir::detail::access (((m)).value ())
             .create_function ("first", {{Type::i64 ()}, {Type::i64 ()}, true});
  if (!(f).has_value ()) return f.error ();
  auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
  if (!(block).has_value ()) return block.error ();
  InstructionBuilder b ((block).value ());
  auto returned = b.ret (
    Operand (*mirnext::ir::detail::ReferenceAccess::pointers (f.value ().arguments ())[0]));
  if (!(returned).has_value ()) return returned.error ();
  constexpr unsigned iterations = 100000;
  std::vector<ExecutionArgument> args (count, ExecutionArgument (42));
  std::array<double, 7> cached;
  for (auto &trial : cached) {
    auto s = ExecutionSession::create (c, {.link = {.compilation = policy}});
    if (!(s).has_value ()) return s.error ();
    auto f = s.value ().find_function ("benchmark", "first");
    if (!(f).has_value ()) return f.error ();
    auto warm = s.value ().call ((f).value (), args);
    if (!(warm).has_value ()) return warm.error ();
    auto start = Clock::now ();
    std::int64_t total = 0;
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = s.value ().call ((f).value (), args);
      if (!(values).has_value () || values.value ().size () != 1)
        return Error{ErrorCode::InvalidOperand, "variadic cached call failed"};
      auto value = ((values).value ())[0].as_i64 ();
      if (!(value).has_value ()) return value.error ();
      total += (value).value ();
    }
    trial = elapsed (start) / iterations;
    if (total != std::int64_t (42) * iterations || s.value ().host_entry_count () != 1)
      return Error{ErrorCode::InvalidOperand, "variadic cache result mismatch"};
  }
  std::sort (cached.begin (), cached.end ());
  std::cout << "compilation=" << (policy == JITCompilation::Eager ? "eager" : "lazy")
            << " variadic_arguments=" << count
            << " generic_cached_call_ns=" << cached[cached.size () / 2] << '\n';
  return {};
}
}  // namespace
int main () {
  auto t = target::TargetMachine::native ();
  if (!(t).has_value ()) return 1;
  if (t.value ().architecture () != target::Architecture::AArch64) {
    std::cout << "Native session benchmark currently requires AArch64.\n";
    return 0;
  }
  for (auto policy : {JITCompilation::Eager, JITCompilation::Lazy})
    for (unsigned count : {1u, 17u}) {
      auto result = measure (count, policy);
      if (!(result).has_value ()) {
        std::cerr << result.error ().message << '\n';
        return 1;
      }
      result = measure_variadic (count, policy);
      if (!(result).has_value ()) {
        std::cerr << result.error ().message << '\n';
        return 1;
      }
    }
}
