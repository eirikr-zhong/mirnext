/* Licensed under the MIT License. MIR corpus execution tests. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "NativeTarget.hpp"
#include <bit>
#include <cstdarg>
#include <fstream>
#include <iterator>
#include <limits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
struct Event {
  std::string format;
  std::vector<ScalarValue> values;
};
struct Capture {
  std::vector<Event> events;
  std::vector<std::int64_t> exits;
  unsigned aborts = 0;
};
thread_local Capture *active_capture = nullptr;
struct Activate {
  Capture *previous;
  explicit Activate (Capture &capture) : previous (active_capture) { active_capture = &capture; }
  ~Activate () { active_capture = previous; }
};
// Test host services observe calls instead of terminating or printing. Their
// signatures retain the original MIR variadic ABI, including long double.
extern "C" void corpus_printf (const char *format, ...) {
  Event event{format, {}};
  va_list args;
  va_start (args, format);
  for (auto p = format; *p; ++p)
    if (*p == '%') {
      if (*++p == '%') continue;
      bool extended = false;
      while (*p && std::string_view ("diuoxXfFeEgGaAcsp").find (*p) == std::string_view::npos) {
        extended |= *p == 'L';
        ++p;
      }
      if (!*p) break;
      if (std::string_view ("fFeEgGaA").find (*p) != std::string_view::npos) {
        if (extended)
          event.values.emplace_back (va_arg (args, long double));
        else
          event.values.emplace_back (va_arg (args, double));
      } else if (*p == 'd' || *p == 'i')
        event.values.emplace_back (va_arg (args, std::int64_t));
      else
        event.values.emplace_back (va_arg (args, std::uint64_t));
    }
  va_end (args);
  active_capture->events.push_back (std::move (event));
}
extern "C" void corpus_exit (std::int64_t code) { active_capture->exits.push_back (code); }
extern "C" void corpus_abort () { ++active_capture->aborts; }
Result<ExternalSymbol> resolve (Capture &capture, const ImportRequest &request) {
  if (request.name == "printf")
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&corpus_printf),
                          Signature{{}, {Type::pointer ()}, true},
                          {},
                          false,
                          [&capture] (std::span<const ExecutionArgument> args)
                            -> Result<std::vector<ScalarValue>> {
                            if (args.empty () || !args[0].scalar ().get ())
                              return Error{ErrorCode::InvalidArgument,
                                           "missing corpus printf format"};
                            auto address = args[0].scalar ().get ()->as_u64 ();
                            if (!(address).has_value ()) return address.error ();
                            Event event{reinterpret_cast<const char *> ((address).value ()), {}};
                            for (auto &arg : args.subspan (1)) {
                              if (!arg.scalar ().get ())
                                return Error{ErrorCode::InvalidArgument,
                                             "non-scalar corpus printf argument"};
                              event.values.push_back (*arg.scalar ().get ());
                            }
                            capture.events.push_back (std::move (event));
                            return std::vector<ScalarValue>{};
                          }};
  if (request.name == "exit")
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&corpus_exit),
                          Signature{{}, {Type::i64 ()}},
                          {},
                          false,
                          [&capture] (std::span<const ExecutionArgument> args)
                            -> Result<std::vector<ScalarValue>> {
                            if (args.size () != 1 || !args[0].scalar ().get ())
                              return Error{ErrorCode::InvalidArgument, "missing corpus exit code"};
                            auto value = args[0].scalar ().get ()->as_i64 ();
                            if (!(value).has_value ()) return value.error ();
                            capture.exits.push_back ((value).value ());
                            return std::vector<ScalarValue>{};
                          }};
  if (request.name == "abort")
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&corpus_abort),
                          Signature{},
                          {},
                          false,
                          [&capture] (std::span<const ExecutionArgument>)
                            -> Result<std::vector<ScalarValue>> {
                            ++capture.aborts;
                            return std::vector<ScalarValue>{};
                          }};
  return Error{ErrorCode::UnknownName,
               "unexpected original corpus import: " + std::string (request.name)};
}
std::unique_ptr<Program> original (unsigned number) {
  auto path = std::string (MIRNEXT_TEST_SOURCE_DIR) + "/tests/Fixtures/Original/test"
              + std::to_string (number) + ".mir";
  std::ifstream file (path);
  REQUIRE (file);
  std::string text ((std::istreambuf_iterator<char> (file)), {});
  auto parsed = serialization::parse_text (text, {.source_name = path});
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  return std::make_unique<Program> (std::move ((parsed).value ()));
}
ExecutionOptions options (Capture &capture, unsigned engine, unsigned level,
                          std::string_view entry) {
  ExecutionOptions options;
  if (engine < 2 || engine >= 5) options.instruction_limit = 1000000;
  if (engine < 2) options.mode = ExecutionMode::Interpreter;
  if (engine)
    options.optimization = transforms::OptimizationOptions{
      .level = static_cast<transforms::OptimizationLevel> (level)};
  if (engine == 3) options.link.compilation = JITCompilation::Lazy;
  if (engine == 4) options.link.compilation = JITCompilation::BlockVersioning;
  if (engine >= 5)
    options.select_engine = [engine, entry] (const Function &f) -> Result<ExecutionEngine> {
      return (f.symbol ().name () == entry) == (engine == 5) ? ExecutionEngine::Interpreter
                                                             : ExecutionEngine::BlockVersioning;
    };
  options.link.resolve = [&capture] (auto &request) { return resolve (capture, request); };
  return options;
}
void same (const ScalarValue &value, const ScalarValue &expected) {
  // Original copy propagation treats D2LD/LD2D as copies when both host
  // representations are double. A variadic consumer interprets those bits
  // using its format, just as the native va_arg service does. Keep strict
  // distinct types on platforms with extended long double.
  std::optional<ScalarValue> normalized;
  if constexpr (sizeof (long double) == sizeof (double)) {
    auto a = value.type ().kind (), b = expected.type ().kind ();
    if ((a == Type::Kind::F64 && b == Type::Kind::LongDouble)
        || (a == Type::Kind::LongDouble && b == Type::Kind::F64)) {
      auto converted = ScalarValue::from_storage (expected.type (), value.storage ());
      REQUIRE ((converted).has_value ());
      normalized = (converted).value ();
    }
  }
  const auto &actual = normalized ? *normalized : value;
  CAPTURE (static_cast<unsigned> (actual.type ().kind ()));
  CAPTURE (static_cast<unsigned> (expected.type ().kind ()));
  if (expected.type ().is_integer ()) {
    auto a = actual.as_u64 (), b = expected.as_u64 ();
    REQUIRE ((a).has_value ());
    REQUIRE ((b).has_value ());
    CHECK ((a).value () == (b).value ());
  } else if (expected.type ().kind () == Type::Kind::F32) {
    auto a = actual.as_float (), b = expected.as_float ();
    REQUIRE ((a).has_value ());
    REQUIRE ((b).has_value ());
    CHECK (std::bit_cast<std::uint32_t> ((a).value ())
           == std::bit_cast<std::uint32_t> ((b).value ()));
  } else if (expected.type ().kind () == Type::Kind::F64) {
    auto a = actual.as_double (), b = expected.as_double ();
    REQUIRE ((a).has_value ());
    REQUIRE ((b).has_value ());
    CHECK (std::bit_cast<std::uint64_t> ((a).value ())
           == std::bit_cast<std::uint64_t> ((b).value ()));
  } else {
    auto a = actual.as_long_double (), b = expected.as_long_double ();
    REQUIRE ((a).has_value ());
    REQUIRE ((b).has_value ());
    CHECK ((a).value () == (b).value ());
  }
}
void event (const Capture &capture, std::size_t index, std::string_view format,
            std::initializer_list<ScalarValue> values) {
  CAPTURE (index);
  CAPTURE (format);
  REQUIRE (index < capture.events.size ());
  auto &actual = capture.events[index];
  CHECK (actual.format == format);
  REQUIRE (actual.values.size () == values.size ());
  std::size_t n = 0;
  for (auto &value : values) same (actual.values[n++], value);
}
}  // namespace
TEST_CASE ("original MIR corpus executes with native C++ services and checked expectations") {
  for (unsigned number = 1; number <= 16; ++number) {
    CAPTURE (number);
    auto source = original (number);
    auto snapshot = dump (*source);
    // test3 intentionally loops forever; it is handled separately below.
    if (number == 3) continue;
    auto &module = *source->modules ().begin ();
    std::string entry = number == 4  ? "call"
                        : number < 8 ? std::string (module.functions ().begin ()->symbol ().name ())
                                     : "main";
    for (unsigned level = 0; level < 4; ++level)
      for (unsigned engine = 0; engine < 7; ++engine) {
        if (!engine && level) continue;
        CAPTURE (level);
        CAPTURE (engine);
        Capture capture;
        Activate active (capture);
        auto input = clone (*source);
        REQUIRE ((input).has_value ());
        if (number == 10) {
          // The original million-iteration case remains compiled/executed by
          // eager JIT. Other engines retain the same IR/stack edges with 32
          // iterations, rather than turning a correctness suite into a timer.
          if (engine != 2)
            for (auto &f : input.value ().modules ().begin ()->functions ())
              for (auto &b : f.blocks ())
                for (auto &i : b.instructions ())
                  if (i.opcode () == Opcode::Blt
                      && mirnext::ir::detail::operand_access (i.operands ().back ())
                           .get<std::int64_t> ()
                      && *mirnext::ir::detail::operand_access (i.operands ().back ())
                             .get<std::int64_t> ()
                           == 1000000)
                    REQUIRE (
                      (InstructionRewriter (f).replace_operand (i, 2, Operand (32))).has_value ());
        }
        auto configuration = options (capture, engine, level, entry);
        if (test::check_native_result_limit ((input).value (), configuration)) continue;
        auto session = ExecutionSession::create ((input).value (), std::move (configuration));
        INFO (((session).has_value () ? "" : session.error ().message));
        REQUIRE ((session).has_value ());
        auto function = session.value ().find_function (module.name (), entry);
        REQUIRE ((function).has_value ());
        auto call = [&] (std::span<const ExecutionArgument> args = {}) {
          auto result = function.value ().call (args);
          INFO (((result).has_value () ? "" : result.error ().message));
          REQUIRE ((result).has_value ());
          return std::move ((result).value ());
        };
        if (number == 1)
          for (std::int64_t value : {-17, 0, 1, 42}) {
            std::array args{ExecutionArgument (value)};
            auto result = call (args);
            REQUIRE (result.size () == 1);
            same (result[0], ScalarValue (4 * value));
          }
        else if (number == 5)
          for (auto size : {0, 32, 8192}) {
            std::array args{ExecutionArgument (size)};
            auto result = call (args);
            REQUIRE (result.size () == 1);
            auto pointer = result[0].as_u64 ();
            REQUIRE ((pointer).has_value ());
            CHECK ((pointer).value () != 0);
          }
        else if (number == 6 || number == 7)
          for (auto value :
               {UINT64_C (0), UINT64_C (1), UINT64_C (0x8000000000000001), UINT64_MAX}) {
            std::array args{ExecutionArgument (value)};
            auto result = call (args);
            REQUIRE (result.size () == 1);
            same (result[0], number == 6 ? ScalarValue (static_cast<float> (value))
                                         : ScalarValue (static_cast<double> (value)));
          }
        else {
          auto result = call ();
          if (number == 2) {
            REQUIRE (result.size () == 1);
            same (result[0], ScalarValue (5));
          } else if (number == 4 || number >= 11) {
            REQUIRE (result.size () == 1);
            same (result[0], ScalarValue (0));
          } else
            CHECK (result.empty ());
        }
        CHECK (capture.aborts == 0);
        CHECK (capture.exits
               == (number >= 8 && number <= 10 ? std::vector<std::int64_t>{0}
                                               : std::vector<std::int64_t>{}));
        if (number == 8) {
          REQUIRE (capture.events.size () == 1);
          event (capture, 0, "Hello %ld != %ld, %g != %g\n", {42, 24, 4.2, 2.4});
        }
        if (number == 9) {
          REQUIRE (capture.events.size () == 4);
          event (capture, 0, "int %ld\n", {42});
          event (capture, 1, "double %g\n", {4.2});
          event (capture, 2, "int %ld\n", {43});
          event (capture, 3, "double %g\n", {4.3});
        }
        if (number == 10) {
          REQUIRE (capture.events.size () == 1);
          event (capture, 0, "result = %ld\n", {6});
        }
        if (number == 11) {
          REQUIRE (capture.events.size () == 22);
          event (capture, 0, "10.56: double = %.25E, long double = %.25LE\n", {10.56, 10.56L});
          long double large = 1e201L;
          event (capture, 1, "10.0E400: double = %E, long double = %LE\n",
                 {std::numeric_limits<double>::infinity (), large * large});
          event (capture, 15, "%.10E\n",
                 {static_cast<double> (static_cast<float> (10.33333333333333333333333L))});
          event (capture, 16, "%.18E\n", {static_cast<double> (10.33333333333333333333333L)});
          event (capture, 17, "%ld\n", {-10});
          event (capture, 18, "%.10LE\n", {-10.0L});
          event (capture, 19, "%.10LE\n", {10.0L});
          event (capture, 20, "%.25LE\n", {static_cast<long double> (10.3333333333333f)});
          event (capture, 21, "%.38LE\n", {static_cast<long double> (10.333333333333333333333333)});
        }
        if (number == 12) {
          REQUIRE (capture.events.size () == 2);
          event (capture, 0, "arg=%ld\n", {42});
          event (capture, 1, "i=%ld, f=%e\n", {42, static_cast<double> (4.2f)});
        }
        if (number == 13) {
          REQUIRE (capture.events.size () == 2);
          auto address = reinterpret_cast<std::uintptr_t> (&corpus_printf);
          event (capture, 0, "printf addr = %lx\n", {ScalarValue (std::uint64_t (address))});
          event (capture, 1, "expr: (printf addr + 0x10) = %lx\n",
                 {ScalarValue (std::uint64_t (address + 16))});
        }
        if (number == 14) {
          REQUIRE (capture.events.size () == 10);
          for (unsigned n = 0; n < 10; ++n)
            event (capture, n, "printf switch (%ld) = %ld\n", {n, n + 42});
        }
        if (number >= 15) {
          REQUIRE (capture.events.size () == 1);
          event (capture, 0, "printf %ld,%ld\n", {24, 42});
        }
        CHECK (dump (*source) == snapshot);
      }
  }
}
TEST_CASE (
  "original nonterminating SCCP regression compiles and interpreter budgets diagnose execution") {
  auto source = original (3);
  for (unsigned level = 0; level < 4; ++level) {
    Capture capture;
    auto configuration = options (capture, 2, level, "ccp2");
    auto compiled = ExecutionSession::create (*source, configuration);
    INFO (((compiled).has_value () ? "" : compiled.error ().message));
    REQUIRE ((compiled).has_value ());
    auto function = compiled.value ().find_function ("m3", "ccp2");
    REQUIRE ((function).has_value ());
    REQUIRE ((function.value ().artifact ()).has_value ());
    CHECK ((function.value ().artifact ().value ().verify ()).has_value ());
    for (unsigned engine : {0u, 1u}) {
      configuration = options (capture, engine, level, "ccp2");
      configuration.instruction_limit = 64;
      auto interpreted = ExecutionSession::create (*source, std::move (configuration));
      REQUIRE ((interpreted).has_value ());
      auto f = interpreted.value ().find_function ("m3", "ccp2");
      REQUIRE ((f).has_value ());
      auto result = f.value ().call ();
      REQUIRE_FALSE ((result).has_value ());
      CHECK (result.error ().message.find ("instruction limit") != std::string::npos);
    }
  }
}
