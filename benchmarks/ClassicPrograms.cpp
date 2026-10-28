/* Licensed under the MIT License. Original loop/sieve workloads using native C++ services. */
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "../tests/Fixtures/ClassicPrograms.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
using namespace mirnext;
using namespace mirnext::execution;
namespace {
using Clock = std::chrono::steady_clock;
struct Options {
  bool sieve = false, interpreter = false, native_entry = false, compile_only = false,
       quick = false;
  unsigned functions = 1, passes = 0;
  std::size_t flags = 0;
  std::int64_t iterations = 0;
};
Result<Options> options (int argc, char **argv) {
  Options result;
  for (int n = 1; n < argc; ++n) {
    std::string_view arg (argv[n]);
    if (arg == "--loop")
      result.sieve = false;
    else if (arg == "--sieve")
      result.sieve = true;
    else if (arg == "--interpreter")
      result.interpreter = true;
    else if (arg == "--native-entry")
      result.native_entry = true;
    else if (arg == "--compile-only")
      result.compile_only = true;
    else if (arg == "--quick")
      result.quick = true;
    else if (arg == "--functions" || arg == "--flags" || arg == "--passes"
             || arg == "--iterations") {
      if (++n == argc) return Error{ErrorCode::InvalidArgument, "missing benchmark numeric option"};
      std::string_view text (argv[n]);
      std::uint64_t value = 0;
      auto parsed = std::from_chars (text.data (), text.data () + text.size (), value);
      auto limit = arg == "--functions" ? UINT64_C (10000)
                   : arg == "--flags"   ? UINT64_C (10000000)
                   : arg == "--passes"  ? UINT64_C (1000000)
                                        : UINT64_C (1000000000000);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data () + text.size () || !value
          || value > limit)
        return Error{ErrorCode::InvalidArgument,
                     "invalid benchmark numeric option: " + std::string (arg)};
      if (arg == "--functions") result.functions = static_cast<unsigned> (value);
      if (arg == "--flags") result.flags = static_cast<std::size_t> (value);
      if (arg == "--passes") result.passes = static_cast<unsigned> (value);
      if (arg == "--iterations") result.iterations = static_cast<std::int64_t> (value);
    } else
      return Error{ErrorCode::InvalidArgument, "unknown benchmark option: " + std::string (arg)};
  }
#ifdef _WIN32
  constexpr std::size_t original_flags = 8190;
#else
  constexpr std::size_t original_flags = 819000;
#endif
  if (!result.flags) result.flags = result.quick ? 127 : original_flags;
  if (!result.passes) result.passes = result.quick ? 2 : result.interpreter ? 100 : 1000;
  if (!result.iterations)
    result.iterations = result.quick ? 1024 : result.interpreter ? 10000000 : 100000000;
  if (result.sieve && result.flags < 4)
    return Error{ErrorCode::InvalidArgument, "classic sieve requires at least four flags"};
  return result;
}
double elapsed (Clock::time_point start) {
  return std::chrono::duration<double, std::milli> (Clock::now () - start).count ();
}
std::int64_t prime_count (std::size_t limit) {
  std::vector<bool> composite (limit + 1);
  std::int64_t count = 0;
  for (std::size_t n = 2; n <= limit; ++n)
    if (!composite[n]) {
      ++count;
      if (n <= limit / n)
        for (auto multiple = n * n; multiple <= limit; multiple += n) composite[multiple] = true;
    }
  return count;
}
Result<void> measure (const Options &o, unsigned level) {
  auto start = Clock::now ();
  std::string text;
  if (o.sieve)
    text = test::classic_sieve_text (o.flags, o.passes);
  else {
    auto c = test::classic_loop ();
    if (!(c).has_value ()) return c.error ();
    auto written = serialization::write_text ((c).value ());
    if (!(written).has_value ()) return written.error ();
    text = std::move ((written).value ());
  }
  std::string modules;
  auto module = o.sieve ? "m_sieve" : "m";
  for (unsigned n = 0; n < o.functions; ++n) {
    auto copy = text;
    auto at = copy.find (std::string (module) + ":");
    if (at == std::string::npos)
      return Error{ErrorCode::InvalidArgument, "missing benchmark module declaration"};
    copy.replace (at, std::string_view (module).size (), "benchmark" + std::to_string (n));
    modules += copy;
  }
  auto context = serialization::parse_text (modules);
  if (!(context).has_value ()) return context.error ();
  auto build_ms = elapsed (start);
  ExecutionOptions configuration{.mode
                                 = o.interpreter ? ExecutionMode::Interpreter : ExecutionMode::JIT};
  if (!o.interpreter || level)
    configuration.optimization = transforms::OptimizationOptions{
      .level = static_cast<transforms::OptimizationLevel> (level)};
  configuration.native_entries = o.interpreter && o.native_entry;
  start = Clock::now ();
  auto session = ExecutionSession::create ((context).value (), std::move (configuration));
  if (!(session).has_value ()) return session.error ();
  auto compile_ms = elapsed (start);
  auto function = session.value ().find_function ("benchmark" + std::to_string (o.functions - 1),
                                                  o.sieve ? "sieve" : "loop");
  if (!(function).has_value ()) return function.error ();
  std::cout << "workload=" << (o.sieve ? "sieve" : "loop")
            << " engine=" << (o.interpreter ? "interpreter" : "jit") << " level=" << level
            << " functions=" << o.functions << " build_ms=" << build_ms
            << " load_compile_ms=" << compile_ms
            << " code_bytes=" << session.value ().statistics ().code_bytes;
  if (o.compile_only) {
    std::cout << " compile_only=1\n";
    return {};
  }
  start = Clock::now ();
  std::optional<NativeEntry> entry;
  if (o.native_entry) {
    auto e = function.value ().native_entry ();
    if (!(e).has_value ()) return e.error ();
    entry = std::move ((e).value ());
  }
  auto entry_ms = elapsed (start);
  std::array arguments{o.iterations};
  auto expected = o.sieve ? prime_count (o.flags) : o.iterations;
  start = Clock::now ();
  Result<std::int64_t> value = Error{ErrorCode::InvalidArgument, "missing benchmark invocation"};
  if (entry) {
    ExecutionInvocation invocation;
    value
      = o.sieve
          ? reinterpret_cast<std::int64_t (*) ()> (entry->address ()) ()
          : reinterpret_cast<std::int64_t (*) (std::int64_t)> (entry->address ()) (o.iterations);
    auto status = invocation.status ();
    if (!(status).has_value ()) return status.error ();
  } else
    value = function.value ().call_i64 (o.sieve ? std::span<const std::int64_t>{}
                                                : std::span<const std::int64_t> (arguments));
  auto execute_ms = elapsed (start);
  if (!(value).has_value ()) return value.error ();
  if ((value).value () != expected)
    return Error{ErrorCode::InvalidOperand,
                 "wrong classic benchmark result: " + std::to_string ((value).value ())
                   + ", expected " + std::to_string (expected)};
  std::cout << " entry=" << (o.native_entry ? "native_abi" : "checked_cpp")
            << " iterations=" << (o.sieve ? o.passes : o.iterations)
            << " flags=" << (o.sieve ? o.flags : 0) << " result=" << (value).value ()
            << " entry_prepare_ms=" << entry_ms << " first_call_ms=" << execute_ms << '\n';
  return {};
}
}  // namespace
int main (int argc, char **argv) {
  auto o = options (argc, argv);
  if (!(o).has_value ()) {
    std::cerr << o.error ().message << '\n';
    return 1;
  }
  for (unsigned level = 0; level < 4; ++level) {
    auto result = measure ((o).value (), level);
    if (!(result).has_value ()) {
      std::cerr << result.error ().message << '\n';
      return 1;
    }
  }
}
