/* Licensed under the MIT License. Checked calls without a surviving session. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "mirnext/Execution/Function.hpp"
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Session.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include <array>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
bool native () { return test::native_execution_available (); }
auto parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
ExecutionSession session (const Program &c, ExecutionOptions options) {
  auto s = ExecutionSession::create (c, std::move (options));
  INFO (((s).has_value () ? "" : s.error ().message));
  REQUIRE ((s).has_value ());
  return std::move ((s).value ());
}
FunctionHandle find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE ((f).has_value ());
  return (f).value ();
}
auto call (const FunctionHandle &f, std::span<const ExecutionArgument> args = {}) {
  auto r = f.call (args);
  INFO (((r).has_value () ? "" : r.error ().message));
  REQUIRE ((r).has_value ());
  return std::move ((r).value ());
}
}  // namespace
TEST_CASE (
  "owning callable handles execute every ABI fixture after source and session destruction at "
  "O0-O3") {
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter})
    for (unsigned level = 0; level < 4; ++level)
      for (unsigned n = 0; n <= unsigned (test::CallExample::Inline); ++n) {
        if (mode == ExecutionMode::JIT && !native ()) continue;
        CAPTURE (mode);
        CAPTURE (level);
        CAPTURE (n);
        auto kind = static_cast<test::CallExample> (n);
        auto c = test::call_example (kind);
        if (test::check_native_result_limit (*c, {.mode = mode,
                                                  .optimization = transforms::OptimizationOptions{
                                                    .level
                                                    = static_cast<transforms::OptimizationLevel> (
                                                      level)}}))
          continue;
        auto retained = [&] {
          auto s = session (*c, {.mode = mode,
                                 .optimization = transforms::OptimizationOptions{
                                   .level = static_cast<transforms::OptimizationLevel> (level)}});
          return find (s, "calls", "sum");
        }();
        c.reset ();
        CHECK (retained.valid ());
        CHECK (retained.mode () == mode);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          std::array<std::int64_t, 2> bytes{20, 22};
          std::vector<ExecutionArgument> args;
          if (kind == test::CallExample::Block)
            args.push_back (ExecutionArgument::block (std::as_bytes (std::span (bytes))));
          else if (kind == test::CallExample::ReturnBlock)
            args.push_back (
              ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (bytes))));
          else if (kind == test::CallExample::VariadicBlock) {
            args.emplace_back (0);
            args.push_back (ExecutionArgument::block (std::as_bytes (std::span (bytes))));
          } else {
            if (kind == test::CallExample::Variadic) args.emplace_back (0);
            args.emplace_back (20);
            args.emplace_back (22);
          }
          auto values = call (retained, args);
          if (kind == test::CallExample::ReturnBlock) {
            CHECK (values.empty ());
            CHECK (bytes[0] == 42);
          } else if (kind == test::CallExample::MultipleResults) {
            REQUIRE (values.size () == 2);
            CHECK ((values[0].as_i64 ()).value () == 20);
            CHECK ((values[1].as_i64 ()).value () == 22);
          } else {
            REQUIRE (values.size () == 1);
            CHECK ((values[0].as_i64 ()).value () == 42);
          }
        }
      }
}
TEST_CASE (
  "owning callable handles retain cold compilation services and share hot adapters with the "
  "session") {
  if (!native ()) return;
  struct Events {
    unsigned scalar = 0, generic = 0;
  };
  for (bool warm : {false, true}) {
    CAPTURE (warm);
    std::weak_ptr<Events> observer;
    std::optional<FunctionHandle> retained;
    std::optional<mirnext::execution::DataHandle> data;
    {
      auto events = std::make_shared<Events> ();
      observer = events;
      auto c = test::variable_example (test::VariableExample::Initialized);
      auto s
        = session (*c,
                   {.link = {.compilation_observer = [events] (const auto &event) -> Result<void> {
                      if (event.stage == codegen::CompilationStage::PrepareMachineIR) {
                        if (event.function.module_name () == "$host") ++events->scalar;
                        if (event.function.module_name () == "$native-call") ++events->generic;
                      }
                      return {};
                    }}});
      retained = find (s, "consumer", "entry");
      auto found = s.find_data ("consumer", "value");
      REQUIRE ((found).has_value ());
      data = std::move ((found).value ());
      if (warm) {
        auto scalar = s.call_i64 (*retained);
        REQUIRE ((scalar).has_value ());
        CHECK ((scalar).value () == 42);
        CHECK ((call (*retained)[0].as_i64 ()).value () == 64);
        CHECK (s.host_entry_count () == 2);
      }
    }
    REQUIRE_FALSE (observer.expired ());
    auto copy = *retained;
    auto scalar = copy.call_i64 ();
    REQUIRE ((scalar).has_value ());
    CHECK ((scalar).value () == (warm ? 86 : 42));
    CHECK ((call (*retained)[0].as_i64 ()).value () == (warm ? 108 : 64));
    scalar = retained->call_i64 ();
    REQUIRE ((scalar).has_value ());
    CHECK ((scalar).value () == (warm ? 130 : 86));
    CHECK (observer.lock ()->scalar == 1);
    CHECK (observer.lock ()->generic == 1);
    {
      auto entry = retained->native_entry ();
      REQUIRE ((entry).has_value ());
      CHECK (entry.value ().name () == "entry");
    }
    retained.reset ();
    CHECK (copy.valid ());
    // A data handle retains the same complete runtime and compilation services.
    CHECK_FALSE (observer.expired ());
    {
      auto moved = std::move (copy);
      CHECK (moved.valid ());
      CHECK_FALSE (copy.valid ());
    }
    CHECK_FALSE (observer.expired ());
    data.reset ();
    CHECK (observer.expired ());
  }
}
TEST_CASE (
  "owning callable handles report moved-from values invalid signatures and nonstandard entries") {
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter}) {
    if (mode == ExecutionMode::JIT && !native ()) continue;
    auto c = parse (
      "m:module\nf:func i64,i64:x\nret x\nendfunc\nj:func\nlocal i64:continuation\njret "
      "continuation\nendfunc\nendmodule\n");
    auto s = session (*c, {.mode = mode});
    auto original = find (s, "m", "f");
    auto moved = std::move (original);
    CHECK_FALSE (original.valid ());
    CHECK (moved.valid ());
    CHECK_FALSE (s.owns (original));
    CHECK_FALSE ((original.call ()).has_value ());
    CHECK_FALSE ((original.call_i64 ()).has_value ());
    CHECK_FALSE ((original.call_u64 ()).has_value ());
    CHECK_FALSE ((original.call_double ()).has_value ());
    CHECK_FALSE ((original.native_entry ()).has_value ());
    CHECK_FALSE ((original.compile ()).has_value ());
    CHECK_FALSE ((moved.call ()).has_value ());
    CHECK_FALSE ((moved.call_i64 ()).has_value ());
    CHECK_FALSE ((moved.call_double ()).has_value ());
    std::array args{ExecutionArgument (2.5)};
    CHECK_FALSE ((moved.call (args)).has_value ());
    auto special = find (s, "m", "j");
    CHECK_FALSE ((special.call ()).has_value ());
    CHECK_FALSE ((special.call_i64 ()).has_value ());
    CHECK_FALSE ((special.native_entry ()).has_value ());
    CHECK (s.host_entry_count () == 0);
    std::array<std::int64_t, 1> value{42};
    auto result = moved.call_i64 (value);
    REQUIRE ((result).has_value ());
    CHECK ((result).value () == 42);
    if (mode == ExecutionMode::Interpreter) CHECK_FALSE ((moved.native_entry ()).has_value ());
    if (mode == ExecutionMode::Interpreter) {
      CHECK (moved.compilation_state () == FunctionCompilationState::Interpreted);
      CHECK_FALSE ((moved.compile ()).has_value ());
    }
  }
}
TEST_CASE (
  "owning callable handles retry failed cold compilation and publication after session "
  "destruction") {
  if (!native ()) return;
  struct Provider final : CodeMemoryProvider {
    std::shared_ptr<CodeMemoryProvider> native = native_code_memory_provider ();
    bool fail = false;
    unsigned maps = 0, unmaps = 0;
    std::size_t page_size () const override { return native->page_size (); }
    Result<MappedCodeMemory> allocate (std::size_t n) override {
      ++maps;
      return native->allocate (n);
    }
    Result<void> release (MappedCodeMemory m) override {
      ++unmaps;
      return native->release (m);
    }
    Result<void> make_writable (MappedCodeMemory m) override {
      if (fail)
        return Error{ErrorCode::InvalidArgument, "injected retained callable publication failure"};
      return native->make_writable (m);
    }
    Result<void> make_executable (MappedCodeMemory m) override {
      return native->make_executable (m);
    }
    Result<void> flush (MappedCodeMemory m) override { return native->flush (m); }
  };
  for (bool generic : {false, true}) {
    CAPTURE (generic);
    auto provider = std::make_shared<Provider> ();
    bool reject = false;
    unsigned attempts = 0;
    std::optional<FunctionHandle> retained;
    std::optional<mirnext::execution::DataHandle> data;
    {
      auto c = test::variable_example (test::VariableExample::Initialized);
      auto s
        = session (*c, {.link = {.memory_provider = provider,
                                 .compilation_observer = [&] (const auto &event) -> Result<void> {
                                   if (event.stage == codegen::CompilationStage::PrepareMachineIR
                                       && event.function.module_name ()
                                            == (generic ? "$native-call" : "$host")) {
                                     ++attempts;
                                     if (reject)
                                       return Error{ErrorCode::UnknownName,
                                                    "injected retained callable observer failure"};
                                   }
                                   return {};
                                 }}});
      retained = find (s, "consumer", "entry");
      auto found = s.find_data ("consumer", "value");
      REQUIRE ((found).has_value ());
      data = std::move ((found).value ());
    }
    auto invoke = [&] () -> Result<std::int64_t> {
      if (!generic) return retained->call_i64 ();
      auto result = retained->call ();
      if (!(result).has_value ()) return result.error ();
      return ((result).value ())[0].as_i64 ();
    };
    auto value = [&] {
      std::int64_t v;
      std::memcpy (&v, data->bytes ().data (), sizeof v);
      return v;
    };
    CHECK (value () == 20);
    CHECK (provider->maps == 1);
    CHECK (provider->unmaps == 0);
    reject = true;
    auto failed = invoke ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().code == ErrorCode::UnknownName);
    CHECK (failed.error ().message.find ("observer failure") != std::string::npos);
    CHECK (attempts == 1);
    CHECK (provider->maps == 1);
    CHECK (value () == 20);
    reject = false;
    provider->fail = true;
    failed = invoke ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("publication failure") != std::string::npos);
    CHECK (attempts == 2);
    CHECK (provider->maps == 2);
    CHECK (provider->unmaps == 1);
    CHECK (value () == 20);
    provider->fail = false;
    auto recovered = invoke ();
    REQUIRE ((recovered).has_value ());
    CHECK ((recovered).value () == 42);
    CHECK (attempts == 3);
    recovered = invoke ();
    REQUIRE ((recovered).has_value ());
    CHECK ((recovered).value () == 64);
    CHECK (attempts == 3);
    CHECK (provider->maps == 3);
    retained.reset ();
    CHECK (provider->unmaps == 1);
    data.reset ();
    CHECK (provider->unmaps == provider->maps);
  }
}
TEST_CASE (
  "owning interpreted callable reentry shares its budget after session destruction and recovers") {
  std::optional<FunctionHandle> retained;
  {
    auto c = parse (
      "m:module\nimport host\np:proto i64,i64:n\nf:func i64,i64:n\nlocal i64:r\nbeq done,n,0\nsub "
      "n,n,1\ncall p,host,r,n\nret r\ndone:ret 42\nendfunc\nendmodule\n");
    auto s
      = session (*c, {.mode = ExecutionMode::Interpreter,
                      .link = {.resolve = [&] (const auto &) -> Result<ExternalSymbol> {
                        ExternalSymbol symbol{0, Signature{{Type::i64 ()}, {Type::i64 ()}}, {}};
                        symbol.interpret = [&] (auto args) -> Result<std::vector<ScalarValue>> {
                          return retained->call (args);
                        };
                        return symbol;
                      }},
                      .call_depth_limit = 4,
                      .native_calls = false});
    retained = find (s, "m", "f");
  }
  std::array<std::int64_t, 1> deep{10};
  auto failed = retained->call_i64 (deep);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("depth limit") != std::string::npos);
  deep[0] = 1;
  auto recovered = retained->call_i64 (deep);
  REQUIRE ((recovered).has_value ());
  CHECK ((recovered).value () == 42);
  CHECK_FALSE ((retained->native_entry ()).has_value ());
}
