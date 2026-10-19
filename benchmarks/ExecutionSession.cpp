/* Licensed under the MIT License. First entry versus cached checked native calls. */
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
  Context c;
  auto m = c.create_module ("benchmark");
  if (!m) return m.error ();
  auto f = (*m)->create_function ("sum", {{Type::i64 ()}, std::vector<Type> (count, Type::i64 ())});
  if (!f) return f.error ();
  auto b = (*f)->create_block ();
  if (!b) return b.error ();
  IRBuilder body (**b);
  auto sum = body.literal (Type::i64 (), 0);
  for (auto a : (*f)->arguments ()) sum = sum + body.expression (*a);
  auto returned = body.ret (sum);
  if (!returned) return returned.error ();
  constexpr unsigned iterations = 100000;
  std::vector<std::int64_t> arguments (count, 3);
  std::vector<ExecutionArgument> generic_arguments (count, ExecutionArgument (3));
  std::array<double, 7> linking, first, cached, handle_cached, generic_first, generic_cached,
    handle_generic_cached;
  for (unsigned trial = 0; trial < first.size (); ++trial) {
    auto start = Clock::now ();
    auto s = ExecutionSession::create (c, {.link = {.compilation = policy}});
    linking[trial] = elapsed (start);
    if (!s) return s.error ();
    if (s->statistics ().compiled_functions != (policy == JITCompilation::Eager ? 1u : 0u))
      return Error{ErrorCode::InvalidOperand, "wrong initial compilation state"};
    auto entry = s->find_function ("benchmark", "sum");
    if (!entry) return entry.error ();
    start = Clock::now ();
    auto result = s->call_i64 (*entry, arguments);
    first[trial] = elapsed (start);
    if (!result) return result.error ();
    if (*result != count * 3) return Error{ErrorCode::InvalidOperand, "wrong first call result"};
    std::int64_t total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto value = s->call_i64 (*entry, arguments);
      if (!value) return value.error ();
      total += *value;
    }
    cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s->host_entry_count () != 1)
      return Error{ErrorCode::InvalidOperand,
                   "cached calls changed results or generated another wrapper"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto value = entry->call_i64 (arguments);
      if (!value) return value.error ();
      total += *value;
    }
    handle_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s->host_entry_count () != 1)
      return Error{ErrorCode::InvalidOperand, "handle calls did not share the scalar adapter"};
    start = Clock::now ();
    auto generic = s->call (*entry, generic_arguments);
    generic_first[trial] = elapsed (start);
    if (!generic || generic->size () != 1)
      return Error{ErrorCode::InvalidOperand, "generic first call failed"};
    auto scalar = (*generic)[0].as_i64 ();
    if (!scalar || *scalar != count * 3)
      return Error{ErrorCode::InvalidOperand, "wrong generic first result"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = s->call (*entry, generic_arguments);
      if (!values || values->size () != 1)
        return Error{ErrorCode::InvalidOperand, "generic cached call failed"};
      auto value = (*values)[0].as_i64 ();
      if (!value) return value.error ();
      total += *value;
    }
    generic_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s->host_entry_count () != 2)
      return Error{ErrorCode::InvalidOperand,
                   "generic cached calls changed results or generated another adapter"};
    total = 0;
    start = Clock::now ();
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = entry->call (generic_arguments);
      if (!values || values->size () != 1)
        return Error{ErrorCode::InvalidOperand, "generic handle call failed"};
      auto value = (*values)[0].as_i64 ();
      if (!value) return value.error ();
      total += *value;
    }
    handle_generic_cached[trial] = elapsed (start) / iterations;
    if (total != std::int64_t (count) * 3 * iterations || s->host_entry_count () != 2)
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
  Context c;
  auto m = c.create_module ("benchmark");
  if (!m) return m.error ();
  auto f = (*m)->create_function ("first", {{Type::i64 ()}, {Type::i64 ()}, true});
  if (!f) return f.error ();
  auto block = (*f)->create_block ();
  if (!block) return block.error ();
  IRBuilder b (**block);
  auto returned = b.ret (Operand (*(*f)->arguments ()[0]));
  if (!returned) return returned.error ();
  constexpr unsigned iterations = 100000;
  std::vector<ExecutionArgument> args (count, ExecutionArgument (42));
  std::array<double, 7> cached;
  for (auto &trial : cached) {
    auto s = ExecutionSession::create (c, {.link = {.compilation = policy}});
    if (!s) return s.error ();
    auto f = s->find_function ("benchmark", "first");
    if (!f) return f.error ();
    auto warm = s->call (*f, args);
    if (!warm) return warm.error ();
    auto start = Clock::now ();
    std::int64_t total = 0;
    for (unsigned n = 0; n < iterations; ++n) {
      auto values = s->call (*f, args);
      if (!values || values->size () != 1)
        return Error{ErrorCode::InvalidOperand, "variadic cached call failed"};
      auto value = (*values)[0].as_i64 ();
      if (!value) return value.error ();
      total += *value;
    }
    trial = elapsed (start) / iterations;
    if (total != std::int64_t (42) * iterations || s->host_entry_count () != 1)
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
  if (!t) return 1;
  if (t->architecture () != target::Architecture::AArch64) {
    std::cout << "Native session benchmark currently requires AArch64.\n";
    return 0;
  }
  for (auto policy : {JITCompilation::Eager, JITCompilation::Lazy})
    for (unsigned count : {1u, 17u}) {
      auto result = measure (count, policy);
      if (!result) {
        std::cerr << result.error ().message << '\n';
        return 1;
      }
      result = measure_variadic (count, policy);
      if (!result) {
        std::cerr << result.error ().message << '\n';
        return 1;
      }
    }
}
