/* Licensed under the MIT License. Loop and sieve execution tests. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "../Fixtures/ClassicPrograms.hpp"
#include <array>
#include <bit>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
struct Capture {
  std::string characters, format;
  std::vector<ScalarValue> arguments;
  std::int32_t primes = -1;
};
thread_local Capture *active = nullptr;
struct Activate {
  Capture *previous = active;
  explicit Activate (Capture &c) { active = &c; }
  ~Activate () { active = previous; }
};
std::int32_t print_character (std::int32_t c) {
  active->characters += static_cast<char> (c);
  return 1;
}
void print_integer (std::int64_t v) { active->arguments.emplace_back (v); }
void print_float (float v) { active->arguments.emplace_back (v); }
void print_double (double v) { active->arguments.emplace_back (v); }
void print_sieve (const char *format, std::int32_t count) {
  active->format = format;
  active->primes = count;
}
Result<ExternalSymbol> resolve (Capture &capture, const ImportRequest &request) {
  if (request.name == "print")
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&print_character),
                          Signature{{Type (Type::Kind::I32)}, {Type (Type::Kind::I32)}},
                          {},
                          false,
                          [&capture] (auto args) -> Result<std::vector<ScalarValue>> {
                            auto c = args[0].scalar ()->as_i64 ();
                            if (!c) return c.error ();
                            capture.characters += static_cast<char> (*c);
                            return std::vector<ScalarValue>{ScalarValue (1)};
                          }};
  if (request.name == "printf")
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&print_sieve),
                          Signature{{}, {Type::pointer (), Type (Type::Kind::I32)}},
                          {},
                          false,
                          [&capture] (auto args) -> Result<std::vector<ScalarValue>> {
                            auto p = args[0].scalar ()->as_u64 ();
                            if (!p) return p.error ();
                            auto n = args[1].scalar ()->as_i64 ();
                            if (!n) return n.error ();
                            capture.format = reinterpret_cast<const char *> (*p);
                            capture.primes = static_cast<std::int32_t> (*n);
                            return std::vector<ScalarValue>{};
                          }};
  auto type = request.name == "pri"   ? Type::i64 ()
              : request.name == "prf" ? Type (Type::Kind::F32)
                                      : Type (Type::Kind::F64);
  std::uintptr_t address = request.name == "pri" ? reinterpret_cast<std::uintptr_t> (&print_integer)
                           : request.name == "prf"
                             ? reinterpret_cast<std::uintptr_t> (&print_float)
                             : reinterpret_cast<std::uintptr_t> (&print_double);
  if (request.name != "pri" && request.name != "prf" && request.name != "prd")
    return Error{ErrorCode::UnknownName, "unexpected classic import"};
  return ExternalSymbol{address,
                        Signature{{}, {type}},
                        {},
                        false,
                        [&capture] (auto args) -> Result<std::vector<ScalarValue>> {
                          capture.arguments.push_back (*args[0].scalar ());
                          return std::vector<ScalarValue>{};
                        }};
}
auto parse (std::string_view text) {
  auto result = serialization::parse_text (text);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
ExecutionOptions options (unsigned engine, unsigned level, Capture &capture) {
  ExecutionOptions options;
  if (engine < 2) {
    options.mode = ExecutionMode::Interpreter;
    options.native_entries = true;
  }
  if (engine)
    options.optimization = transforms::OptimizationOptions{
      .level = static_cast<transforms::OptimizationLevel> (level)};
  if (engine == 3) options.link.compilation = JITCompilation::Lazy;
  if (engine == 4) options.link.compilation = JITCompilation::BlockVersioning;
  options.link.resolve = [&capture] (auto &r) { return resolve (capture, r); };
  return options;
}
auto session (const Context &context, ExecutionOptions options) {
  auto result = ExecutionSession::create (context, std::move (options));
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
auto find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE (f);
  return *f;
}
auto integer (const FunctionHandle &f, std::span<const std::int64_t> args = {}) {
  auto r = f.call_i64 (args);
  INFO ((r ? "" : r.error ().message));
  REQUIRE (r);
  return *r;
}
auto entry (const FunctionHandle &f) {
  auto e = f.native_entry ();
  INFO ((e ? "" : e.error ().message));
  REQUIRE (e);
  return *e;
}
std::int64_t primes (std::size_t limit) {
  // Compute the expected sieve count independently, excluding 2.
  std::vector<bool> composite (limit + 1);
  std::int64_t count = 0;
  for (std::size_t n = 2; n <= limit; ++n)
    if (!composite[n]) {
      if (n != 2) ++count;
      if (n <= limit / n)
        for (auto multiple = n * n; multiple <= limit; multiple += n) composite[multiple] = true;
    }
  return count;
}
void check_arguments (const Capture &capture, std::span<const ExecutionArgument> arguments) {
  constexpr std::array order{0, 1, 2, 3, 6, 7, 8, 9, 10, 4, 11, 12, 13, 14, 15, 16, 17, 5, 18};
  REQUIRE (capture.arguments.size () == order.size ());
  for (unsigned n = 0; n < order.size (); ++n) {
    CAPTURE (n);
    auto &actual = capture.arguments[n];
    auto &expected = *arguments[order[n]].scalar ();
    if (n < 9)
      CHECK (*actual.as_i64 () == *expected.as_i64 ());
    else if (n < 17)
      CHECK (std::bit_cast<std::uint32_t> (*actual.as_float ())
             == std::bit_cast<std::uint32_t> (*expected.as_float ()));
    else
      CHECK (std::bit_cast<std::uint64_t> (*actual.as_double ())
             == std::bit_cast<std::uint64_t> (*expected.as_double ()));
  }
}
}  // namespace
TEST_CASE (
  "classic loop and greeting drivers use owning DSL and checked host ABI entries at O0-O3") {
  auto source = test::classic_loop ();
  REQUIRE (source);
  auto greeting = parse (test::classic_hi_text);
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      Capture capture;
      Activate active (capture);
      auto s = session (**source, options (engine, level, capture));
      auto f = find (s, "m", "loop");
      auto e = entry (f);
      for (std::int64_t n : {-10, 0, 1, 257, 8192}) {
        std::array args{n};
        auto expected = n < 0 ? 0 : n;
        CHECK (integer (f, args) == expected);
        ExecutionInvocation invocation;
        CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (e.address ()) (n) == expected);
        CHECK (invocation.status ());
      }
      auto hi = session (*greeting, options (engine, level, capture));
      auto h = find (hi, "m_hi", "hi");
      CHECK (integer (h) == 4);
      CHECK (capture.characters == "hi!\n");
      capture.characters.clear ();
      auto he = entry (h);
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int32_t (*) ()> (he.address ()) () == 4);
      CHECK (invocation.status ());
      CHECK (capture.characters == "hi!\n");
    }
}
TEST_CASE (
  "classic nineteen mixed arguments preserve signed extension stack spills and real host ABI "
  "calls") {
  auto source = parse (test::classic_args_text);
  auto before = dump (*source);
  using Entry
    = std::int64_t (*) (std::int8_t, std::int16_t, std::int32_t, std::int64_t, float, double,
                        std::uint32_t, std::uint8_t, std::uint16_t, std::int32_t, std::int64_t,
                        float, float, float, float, float, float, float, double);
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      Capture capture;
      Activate active (capture);
      auto s = session (*source, options (engine, level, capture));
      auto f = find (s, "m", "f");
      auto e = entry (f);
      for (bool negative : {false, true}) {
        CAPTURE (negative);
        std::int64_t sign = negative ? -1 : 1;
        std::array<ExecutionArgument, 19> args{sign,
                                               2 * sign,
                                               3 * sign,
                                               INT64_C (0x100000004) * sign,
                                               1.0f,
                                               2.0,
                                               negative ? INT64_C (0xffffffff) : 5,
                                               negative ? 255 : 6,
                                               negative ? 65535 : 7,
                                               8 * sign,
                                               INT64_C (0x100000009) * sign,
                                               3.0f,
                                               4.0f,
                                               5.0f,
                                               6.0f,
                                               7.0f,
                                               8.0f,
                                               9.0f,
                                               10.0};
        auto result = f.call (args);
        INFO ((result ? "" : result.error ().message));
        REQUIRE (result);
        REQUIRE (result->size () == 1);
        CHECK (*(*result)[0].as_i64 () == 0);
        check_arguments (capture, args);
        capture.arguments.clear ();
        ExecutionInvocation invocation;
        CHECK (reinterpret_cast<Entry> (
                 e.address ()) (static_cast<std::int8_t> (sign),
                                static_cast<std::int16_t> (2 * sign),
                                static_cast<std::int32_t> (3 * sign), INT64_C (0x100000004) * sign,
                                1.0f, 2.0, negative ? UINT32_MAX : 5, negative ? 255 : 6,
                                negative ? 65535 : 7, static_cast<std::int32_t> (8 * sign),
                                INT64_C (0x100000009) * sign, 3, 4, 5, 6, 7, 8, 9, 10)
               == 0);
        CHECK (invocation.status ());
        check_arguments (capture, args);
        capture.arguments.clear ();
      }
      CHECK (dump (*source) == before);
    }
}
TEST_CASE (
  "classic alias annotated sieve validates repeated memory writes with independent prime counts") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      Capture capture;
      Activate active (capture);
#ifdef _WIN32
      constexpr unsigned original_size = 8190;
#else
      constexpr unsigned original_size = 819000;
#endif
      auto size = engine == 2 ? original_size : 127u;
      auto source = parse (test::classic_sieve_text (size));
      // The original starts at index 2 (number 3), leaves 4 marked, and skips
      // number 2. Thus its count equals pi(size), although the marked set differs.
      auto s = session (*source, options (engine, level, capture));
      auto f = find (s, "m_sieve", "sieve");
      auto expected = primes (size) + 1;
      CHECK (integer (f) == expected);
      auto e = entry (f);
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int64_t (*) ()> (e.address ()) () == expected);
      CHECK (invocation.status ());
    }
}
TEST_CASE ("classic two module readme sieve shares globals imports and actual mixed engine calls") {
  constexpr std::array engines{ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                               ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning};
  auto source = parse (test::classic_sieve_text (257, 2, true));
  auto before = dump (*source);
  for (unsigned level = 0; level < 4; ++level)
    for (auto caller : engines)
      for (auto callee : engines) {
        CAPTURE (level);
        CAPTURE (caller);
        CAPTURE (callee);
        Capture capture;
        Activate active (capture);
        auto configuration = options (2, level, capture);
        configuration.native_entries = true;
        configuration.select_engine = [=] (const Function &f) -> Result<ExecutionEngine> {
          return f.symbol ().name () == "ex100" ? caller : callee;
        };
        auto s = session (*source, std::move (configuration));
        auto f = find (s, "m_ex100", "ex100");
        auto result = f.call ();
        INFO ((result ? "" : result.error ().message));
        REQUIRE (result);
        CHECK (result->empty ());
        CHECK (capture.primes == primes (515));
        CHECK (capture.format == "sieve of 257 200 times = %d\n");
        capture.primes = -1;
        auto e = entry (f);
        ExecutionInvocation invocation;
        reinterpret_cast<void (*) ()> (e.address ()) ();
        CHECK (invocation.status ());
        CHECK (capture.primes == primes (515));
        CHECK (dump (*source) == before);
      }
}
