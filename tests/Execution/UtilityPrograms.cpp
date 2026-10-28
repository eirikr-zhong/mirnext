/* Licensed under the MIT License. Utility program semantics. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "../Fixtures/ClassicPrograms.hpp"
#include "../Fixtures/UtilityPrograms.hpp"
#include <fstream>
#include <iterator>
#include <cstdarg>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
auto parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
auto make (const Program &c, ExecutionOptions options) {
  auto s = ExecutionSession::create (c, std::move (options));
  INFO (((s).has_value () ? "" : s.error ().message));
  REQUIRE ((s).has_value ());
  return std::move ((s).value ());
}
auto find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE ((f).has_value ());
  return (f).value ();
}
ExecutionOptions options (unsigned engine, unsigned level) {
  ExecutionOptions o;
  if (engine < 2) {
    o.mode = ExecutionMode::Interpreter;
    o.native_entries = true;
  }
  if (engine)
    o.optimization = transforms::OptimizationOptions{
      .level = static_cast<transforms::OptimizationLevel> (level)};
  if (engine == 3) o.link.compilation = JITCompilation::Lazy;
  if (engine == 4) o.link.compilation = JITCompilation::BlockVersioning;
  return o;
}
void roundtrip (const Program &source) {
  auto text = serialization::write_text (source);
  REQUIRE ((text).has_value ());
  auto parsed = parse ((text).value ());
  auto second = serialization::write_text (*parsed);
  REQUIRE ((second).has_value ());
  CHECK ((second).value () == (text).value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto bytes = serialization::write_binary (source, encoding);
    REQUIRE ((bytes).has_value ());
    auto read = serialization::read_binary ((bytes).value (), {.encoding = encoding});
    REQUIRE ((read).has_value ());
    auto written = serialization::write_binary ((read).value (), encoding);
    REQUIRE ((written).has_value ());
    CHECK ((written).value () == (bytes).value ());
  }
}
thread_local std::vector<std::string> *messages = nullptr;
std::int32_t capture_message (const char *text) {
  messages->emplace_back (text);
  return static_cast<std::int32_t> (std::string_view (text).size ());
}
struct Capture {
  std::vector<std::string> values;
  std::vector<std::string> *previous = messages;
  Capture () { messages = &values; }
  ~Capture () { messages = previous; }
};
thread_local unsigned foreign_calls = 0;
std::int64_t foreign_variadic (std::int64_t fixed, ...) {
  ++foreign_calls;
  va_list list;
  va_start (list, fixed);
  auto a = va_arg (list, std::int64_t);
  auto b = va_arg (list, double);
  va_end (list);
  return fixed + a + static_cast<std::int64_t> (b);
}
struct Pair {
  std::int64_t a, b;
};
// Match the native C++ aggregate's SysV INTEGER/INTEGER classification.
constexpr auto pair_class = Type::BlockClass::Class1;
std::int64_t foreign_pair (Pair pair) {
  ++foreign_calls;
  auto result = pair.a + pair.b;
  pair.a = 99;
  return result;
}
using Reentry = std::int64_t (*) (std::uintptr_t, std::int64_t);
std::int64_t foreign_reentry (Reentry callback, std::uintptr_t host, std::int64_t n) {
  ++foreign_calls;
  return callback (host, n);
}
constexpr auto reentry_text
  = "m:module\np:proto i64,p:callback,p:host,i64:n\nf:func i64,p:host,i64:n\nlocal i64:r\nbeq "
    "done,n,0\nsub n,n,1\ncall p,host,r,f,host,n\nret r\ndone:ret 42\nendfunc\nendmodule\n";
}  // namespace
TEST_CASE (
  "utility scanner and memory simplification retain comments aliases and unusual addresses without "
  "execution") {
  std::ifstream file (std::string (MIRNEXT_TEST_SOURCE_DIR) + "/tests/Fixtures/scanner.mir");
  REQUIRE (file);
  std::string input ((std::istreambuf_iterator<char> (file)), {});
  auto scanned = parse (input);
  input.clear ();
  input.shrink_to_fit ();
  auto text = serialization::write_text (*scanned);
  REQUIRE ((text).has_value ());
  CHECK (text.value ().find ("# a comment") == text.value ().npos);
  CHECK (text.value ().find ("noalias1") != text.value ().npos);
  CHECK (text.value ().find ("noalias2") != text.value ().npos);
  roundtrip (*scanned);
  auto unterminated = serialization::parse_text (test::utility_unterminated_memory_text ());
  REQUIRE_FALSE ((unterminated).has_value ());
  CHECK (unterminated.error ().message.find ("non-void function falls through")
         != std::string::npos);
  auto memory = parse (test::utility_memory_text);
  roundtrip (*memory);
  bool absolute = false, index_only = false, multi_memory = false;
  for (auto &b : memory->modules ().begin ()->functions ().begin ()->blocks ())
    for (auto &i : b.instructions ()) {
      unsigned count = 0;
      for (auto &o : i.operands ())
        if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ()) {
          ++count;
          absolute |= !mirnext::ir::detail::NodeAccess::base (*(m))
                      && !mirnext::ir::detail::NodeAccess::index (*(m));
          index_only |= !mirnext::ir::detail::NodeAccess::base (*(m))
                        && mirnext::ir::detail::NodeAccess::index (*(m));
        }
      multi_memory |= count == 3;
    }
  CHECK (absolute);
  CHECK (index_only);
  CHECK (multi_memory);
  // The original scanner has an inclusive bound past its allocation, and the
  // memory sample intentionally uses arbitrary addresses. Both are format/pass
  // fixtures only; executable sieve coverage uses the safe owning API below.
  for (auto source : {scanned.get (), memory.get ()}) {
    auto original = dump (*source);
    auto copied = clone (*source);
    REQUIRE ((copied).has_value ());
    transforms::FunctionPassManager passes;
    passes.add<transforms::SimplifyPass> ();
    auto result = passes.run ((copied).value ());
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    REQUIRE ((copied.value ().verify ()).has_value ());
    roundtrip ((copied).value ());
    CHECK (dump (*source) == original);
  }
}
TEST_CASE ("utility API sieve has equivalent owning text binary and executable results at O0-O3") {
  auto source = std::make_unique<Program> ();
  auto built = test::append_api_sieve (*source, "api", 127, 2);
  INFO (((built).has_value () ? "" : built.error ().message));
  REQUIRE ((built).has_value ());
  REQUIRE ((source->verify ()).has_value ());
  roundtrip (*source);
  auto bytes = serialization::write_binary (*source);
  REQUIRE ((bytes).has_value ());
  auto read = serialization::read_binary ((bytes).value ());
  REQUIRE ((read).has_value ());
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      for (auto context : {source.get (), &(read).value ()}) {
        auto s = make (*context, options (engine, level));
        auto f = find (s, "api", "sieve");
        auto value = f.call_i64 ();
        REQUIRE ((value).has_value ());
        CHECK ((value).value () == 31);
      }
    }
}
TEST_CASE ("utility hello callback and embedded strings use checked DSL and real host ABI calls") {
  using Callback = std::int32_t (*) (const char *);
  auto source = test::utility_hello ();
  REQUIRE ((source).has_value ());
  auto original = dump ((source).value ());
  roundtrip ((source).value ());
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      Capture capture;
      auto s = make ((source).value (), options (engine, level));
      auto f = find (s, "hello", "hello");
      auto entry = f.native_entry ();
      REQUIRE ((entry).has_value ());
      for (std::int32_t id : {0, 1}) {
        auto text = id ? "goodbye" : "hello";
        std::array<ExecutionArgument, 3>
          args{std::uint64_t (reinterpret_cast<std::uintptr_t> (text)),
               std::uint64_t (reinterpret_cast<std::uintptr_t> (&capture_message)), id};
        auto result = f.call (args);
        INFO (((result).has_value () ? "" : result.error ().message));
        REQUIRE ((result).has_value ());
        REQUIRE (result.value ().size () == 1);
        CHECK ((((result).value ())[0].as_i64 ()).value () == 10);
        ExecutionInvocation invocation;
        CHECK (reinterpret_cast<std::int32_t (*) (const char *, Callback, std::int32_t)> (
                 entry.value ().address ()) (text, &capture_message, id)
               == 10);
        CHECK ((invocation.status ()).has_value ());
        CHECK (capture.values
               == std::vector<std::string>{text, id ? "all" : "world", text, id ? "all" : "world"});
        capture.values.clear ();
      }
      CHECK (dump ((source).value ()) == original);
    }
}
TEST_CASE (
  "utility issue219 inline imports load successive owning snapshots and preserve old function "
  "handles") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      auto context = parse (test::utility_add_text);
      auto first = make (*context, options (engine, level));
      auto add = find (first, "m_add", "add");
      auto definition
        = context->find_module ("m_add").get ()->find_symbol ("add").get ()->function ().get ();
      auto module = mirnext::ir::detail::access (*(context)).create_module ("m_1p1");
      REQUIRE ((module).has_value ());
      auto imported = mirnext::ir::detail::access (((module)).value ())
                        .import ("add", definition->signature ());
      REQUIRE ((imported).has_value ());
      auto f = mirnext::ir::detail::access (((module)).value ())
                 .create_function ("p2", {{Type (Type::Kind::I32)}, {}});
      REQUIRE ((f).has_value ());
      REQUIRE (
        (mirnext::ir::detail::access (((module)).value ()).export_symbol (f.value ().symbol ()))
          .has_value ());
      auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
      REQUIRE ((block).has_value ());
      InstructionBuilder b ((block).value ());
      auto called = b.inline_call ((imported).value (), {1, 1});
      REQUIRE ((called).has_value ());
      auto value = called.value ().value ();
      REQUIRE ((value).has_value ());
      REQUIRE ((b.ret (Operand ((value).value ()))).has_value ());
      CHECK (context->find_module ("m_add").get ()->find_symbol ("add").get ()->function ().get ()
             == definition);
      REQUIRE ((context->verify ()).has_value ());
      auto original = dump (*context);
      auto combined = make (*context, options (engine, level));
      CHECK (dump (*context) == original);
      context.reset ();
      auto p2 = find (combined, "m_1p1", "p2");
      for (unsigned n = 0; n < 2; ++n) {
        auto result = p2.call_i64 ();
        REQUIRE ((result).has_value ());
        CHECK ((result).value () == 2);
      }
      std::array<std::int64_t, 2> args{40, 2};
      auto result = add.call_i64 (args);
      REQUIRE ((result).has_value ());
      CHECK ((result).value () == 42);
    }
}
TEST_CASE (
  "utility owning entries replace raw thunk unwrapping and survive explicit compilation and source "
  "destruction") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned engine = 0; engine < 5; ++engine) {
      if (!engine && level) continue;
      CAPTURE (level);
      CAPTURE (engine);
      std::optional<NativeEntry> retained;
      {
        auto source = parse (test::classic_sieve_text (127, 2));
        auto s = make (*source, options (engine, level));
        auto f = find (s, "m_sieve", "sieve");
        auto before = f.native_entry ();
        REQUIRE ((before).has_value ());
        auto address = before.value ().address ();
        if (engine >= 2) REQUIRE ((f.compile ()).has_value ());
        auto after = f.native_entry ();
        REQUIRE ((after).has_value ());
        CHECK (after.value ().address () == address);
        retained = (after).value ();
        if (engine == 2 || engine == 3) {
          REQUIRE ((f.artifact ()).has_value ());
          CHECK ((f.artifact ().value ().verify ()).has_value ());
        }
      }
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        ExecutionInvocation invocation;
        CHECK (reinterpret_cast<std::int64_t (*) ()> (retained->address ()) () == 31);
        CHECK ((invocation.status ()).has_value ());
      }
    }
}
TEST_CASE (
  "utility unregistered foreign pointers support variadics blocks and cached adapters without "
  "imported names") {
  for (unsigned level = 0; level < 4; ++level) {
    auto c = parse (
      "m:module\np:proto i64,i64:fixed,...\nf:func i64,p:target\nlocal i64:r\ncall "
      "p,target,r,10,20,12.0\nret r\nendfunc\nendmodule\n");
    auto configuration = options (1, level);
    configuration.native_entries = false;
    auto s = make (*c, configuration);
    auto f = find (s, "m", "f");
    CHECK (s.host_entry_count () == 0);
    foreign_calls = 0;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      std::array<std::int64_t, 1> args{
        static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (&foreign_variadic))};
      auto result = f.call_i64 (args);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      CHECK ((result).value () == 42);
      CHECK (s.host_entry_count () == 1);
    }
    CHECK (foreign_calls == 3);
    Program blocks;
    auto module = mirnext::ir::detail::access (blocks).create_module ("blocks");
    REQUIRE ((module).has_value ());
    auto prototype
      = mirnext::ir::detail::access (((module)).value ())
          .prototype ("p", {{Type::i64 ()}, {Type::block (sizeof (Pair), pair_class)}});
    REQUIRE ((prototype).has_value ());
    auto fn
      = mirnext::ir::detail::access (((module)).value ())
          .create_function ("f", {{Type::i64 ()},
                                  {Type::pointer (), Type::block (sizeof (Pair), pair_class)}});
    REQUIRE ((fn).has_value ());
    auto block = mirnext::ir::detail::access (((fn)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    InstructionBuilder b ((block).value ());
    auto args = mirnext::ir::detail::ReferenceAccess::pointers (fn.value ().arguments ());
    auto called
      = b.call_indirect ((prototype).value (), Operand (*args[0]),
                         {mirnext::ir::detail::BlockArgumentState (*args[1], sizeof (Pair),
                                                                   pair_class)});
    REQUIRE ((called).has_value ());
    auto value = called.value ().value ();
    REQUIRE ((value).has_value ());
    REQUIRE ((b.ret (Operand ((value).value ()))).has_value ());
    auto program = make (blocks, configuration);
    auto bf = find (program, "blocks", "f");
    Pair pair{20, 22};
    std::array<ExecutionArgument, 2>
      arguments{std::uint64_t (reinterpret_cast<std::uintptr_t> (&foreign_pair)),
                ExecutionArgument::block (std::as_bytes (std::span (&pair, 1)), pair_class)};
    auto result = bf.call (arguments);
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    CHECK ((((result).value ())[0].as_i64 ()).value () == 42);
    CHECK (pair.a == 20);
  }
}
TEST_CASE (
  "utility foreign pointer errors preserve disabled null signature and jump convention "
  "diagnostics") {
  auto c = parse (
    "m:module\np:proto i64\nq:proto\nwrong:func\nret\nendfunc\nget:func p\nret "
    "wrong\nendfunc\nf:func i64,p:target\nlocal i64:r\ncall p,target,r\nret r\nendfunc\nj:func "
    "i64,p:target\njcall q,target\nret 0\nendfunc\nendmodule\n");
  auto configured = options (0, 0);
  configured.native_calls = false;
  configured.native_entries = false;
  auto disabled = make (*c, configured);
  auto f = find (disabled, "m", "f");
  foreign_calls = 0;
  std::array<std::int64_t, 1> args{
    static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (&foreign_variadic))};
  auto result = f.call_i64 (args);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("native foreign calls are disabled") != std::string::npos);
  args[0] = 0;
  result = f.call_i64 (args);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("null native call target") != std::string::npos);
  auto address = find (disabled, "m", "get").call_i64 ();
  REQUIRE ((address).has_value ());
  args[0] = (address).value ();
  result = f.call_i64 (args);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("signature or convention mismatch") != std::string::npos);
  args[0] = static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (&foreign_variadic));
  result = find (disabled, "m", "j").call_i64 (args);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("external jcall requires a nonstandard ABI entry")
         != std::string::npos);
  CHECK (foreign_calls == 0);
  CHECK (disabled.host_entry_count () == 0);
}
TEST_CASE (
  "utility cold foreign pointers protect virtual identities share reentry budgets and recover") {
  auto c = parse (reentry_text);
  auto unexposed = options (0, 0);
  unexposed.native_entries = false;
  auto s = make (*c, unexposed);
  std::array<std::int64_t, 2> args{static_cast<std::int64_t> (
                                     reinterpret_cast<std::uintptr_t> (&foreign_reentry)),
                                   1};
  auto rejected = find (s, "m", "f").call_i64 (args);
  REQUIRE_FALSE ((rejected).has_value ());
  CHECK (rejected.error ().message.find ("requires native_entries") != std::string::npos);
  CHECK (s.host_entry_count () == 0);
  for (bool instruction_budget : {false, true}) {
    auto configuration = options (0, 0);
    configuration.instruction_limit = instruction_budget ? 12 : 0;
    configuration.call_depth_limit = instruction_budget ? 0 : 4;
    auto program = make (*c, configuration);
    auto f = find (program, "m", "f");
    args[1] = 20;
    auto failed = f.call_i64 (args);
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find (instruction_budget ? "instruction limit" : "depth limit")
           != std::string::npos);
    args[1] = 1;
    auto recovered = f.call_i64 (args);
    INFO (((recovered).has_value () ? "" : recovered.error ().message));
    REQUIRE ((recovered).has_value ());
    CHECK ((recovered).value () == 42);
  }
}
TEST_CASE (
  "utility retained foreign pointer calls preserve cold services retry observation failures and "
  "cache success") {
  struct Events {
    unsigned attempts = 0;
    bool reject = true;
  };
  auto events = std::make_shared<Events> ();
  std::weak_ptr<Events> observer = events;
  std::optional<FunctionHandle> retained;
  {
    auto c = parse (
      "m:module\np:proto i64,i64:fixed,...\nf:func i64,p:target\nlocal i64:r\ncall "
      "p,target,r,10,20,12.0\nret r\nendfunc\nendmodule\n");
    auto configuration = options (0, 0);
    configuration.native_entries = false;
    configuration.link.compilation_observer = [events] (const auto &snapshot) -> Result<void> {
      if (snapshot.function.module_name () == "$native-call"
          && snapshot.stage == codegen::CompilationStage::PrepareMachineIR) {
        ++events->attempts;
        if (events->reject)
          return Error{ErrorCode::InvalidArgument, "injected cold foreign adapter failure"};
      }
      return {};
    };
    auto s = make (*c, std::move (configuration));
    CHECK (s.host_entry_count () == 0);
    retained = find (s, "m", "f");
  }
  events.reset ();
  REQUIRE_FALSE (observer.expired ());
  std::array<std::int64_t, 1> args{
    static_cast<std::int64_t> (reinterpret_cast<std::uintptr_t> (&foreign_variadic))};
  auto failed = retained->call_i64 (args);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("injected cold foreign adapter failure")
         != std::string::npos);
  CHECK (observer.lock ()->attempts == 1);
  observer.lock ()->reject = false;
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto value = retained->call_i64 (args);
    REQUIRE ((value).has_value ());
    CHECK ((value).value () == 42);
  }
  CHECK (observer.lock ()->attempts == 2);
  retained.reset ();
  CHECK (observer.expired ());
}
