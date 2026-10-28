/* Licensed under the MIT License. Actual native invocation failure recovery. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
#include "mirnext/Execution/detail/Recovery.hpp"
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/Execution/detail/NativeCalls.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/Calls.hpp"
#include <cfenv>
#include <set>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
using namespace mirnext::execution;
namespace {
using Recovery = execution::detail::NativeRecovery;
using Runtime = execution::detail::BBVRuntime;
using Options = execution::detail::BBVRuntimeOptions;
std::unique_ptr<Runtime> runtime (std::string_view text, unsigned level, const Recovery &recovery,
                                  Options options = {}) {
  auto context = serialization::parse_text (text);
  INFO (((context).has_value () ? "" : context.error ().message));
  REQUIRE ((context).has_value ());
  auto optimized
    = transforms::optimize ((context).value (),
                            {.level = static_cast<transforms::OptimizationLevel> (level)});
  REQUIRE ((optimized).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto prepared = BBVPreparation::prepare (*optimized.value ()
                                              .program ()
                                              .modules ()
                                              .begin ()
                                              ->functions ()
                                              .begin (),
                                           (target).value (), {.optimization_level = level});
  REQUIRE ((prepared).has_value ());
  auto program = BBVProgram::create (std::move ((prepared).value ()));
  REQUIRE ((program).has_value ());
  options.code_capacity = 1024 * 1024;
  options.failure_continuation = recovery.recovery_address ();
  if (!options.resolve)
    options.resolve = [] (auto &request) -> Result<std::uint64_t> {
      if (!request.builtin)
        return Error{ErrorCode::UnknownName, "unexpected recovery fixture symbol"};
      auto address = builtin_address (*request.builtin);
      if (!address.has_value ()) return address.error ();
      return address.value ();
    };
  auto result = Runtime::create (std::move ((program).value ()), std::move (options));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
std::int64_t add (std::uintptr_t a, std::uintptr_t b) {
  return *reinterpret_cast<const std::int64_t *> (a) + *reinterpret_cast<const std::int64_t *> (b);
}
std::uint64_t bits (std::uintptr_t a, std::uintptr_t) { return a; }
double floating (std::uintptr_t a, std::uintptr_t) {
  return *reinterpret_cast<const double *> (a) + 0.5;
}
void store (std::uintptr_t a, std::uintptr_t b) {
  *reinterpret_cast<std::int64_t *> (b) = *reinterpret_cast<const std::int64_t *> (a);
}
void floating_effect (std::uintptr_t, std::uintptr_t) {
  std::fesetround (FE_UPWARD);
  std::feraiseexcept (FE_INEXACT);
}
void reported_error (std::uintptr_t, std::uintptr_t) {
  execution::detail::report_execution_error ({ErrorCode::InvalidArgument, "first checked error"});
  execution::detail::report_execution_error ({ErrorCode::InvalidArgument, "second checked error"});
}
struct Lifetime {
  unsigned &destroyed;
  ~Lifetime () { ++destroyed; }
};
constexpr auto branching
  = "m:module\nf:func i64,i64:x,p:out\nlocal p:stack,i64:r\nalloca stack,4096\nmov "
    "i64:(stack),x\nmov i64:(out),x\nbt yes,x\nmov r,i64:(stack)\nadd r,r,17\nret r\nyes:mov "
    "r,i64:(stack)\nadd r,r,41\nret r\nendfunc\nendmodule\n";
}  // namespace
TEST_CASE ("invocation checkpoint layouts preserve native host ABI and reject foreign facts") {
  using R = target::X86Register;
  for (auto abi : {target::ABI::X86SysV, target::ABI::X86Windows}) {
    auto f = codegen::detail::x86_invocation_frame (abi);
    REQUIRE ((f).has_value ());
    bool windows = abi == target::ABI::X86Windows;
    CHECK (f.value ().preserved_count == (windows ? 8 : 6));
    CHECK (f.value ().shadow_bytes == (windows ? 32 : 0));
    CHECK (f.value ().checkpoint_offset == f.value ().shadow_bytes);
    CHECK (f.value ().checkpoint_offset + 8 == f.value ().frame_bytes);
    CHECK ((8 + 16 - f.value ().frame_bytes % 16) % 16 == 0);
    CHECK (f.value ().red_zone_bytes == (windows ? 0 : 128));
    CHECK (f.value ().parameters[0] == (windows ? R::CX : R::DI));
    CHECK (f.value ().parameters[3] == (windows ? R::R9 : R::CX));
    std::set<R> saved;
    for (unsigned n = 0; n < f.value ().preserved_count; ++n) {
      CHECK (saved.insert (f.value ().preserved[n]).second);
      CHECK ((f.value ().preserved[n] == R::BP
              || target::x86_callee_saved_register (abi, f.value ().preserved[n])));
    }
    for (unsigned n = 0; n < 16; ++n)
      if (n != unsigned (R::SP))
        CHECK (saved.contains (static_cast<R> (n))
               == (n == unsigned (R::BP)
                   || target::x86_callee_saved_register (abi, static_cast<R> (n))));
  }
  CHECK_FALSE ((codegen::detail::x86_invocation_frame (target::ABI::AArch64Linux)).has_value ());
  CHECK_FALSE (
    (codegen::detail::x86_invocation_frame (static_cast<target::ABI> (99))).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  CHECK_FALSE ((make_invocation_entries ((target).value (), 0)).has_value ());
  if (target.value ().architecture () == target::Architecture::AArch64)
    CHECK_FALSE ((make_invocation_entries ((target).value (), 1)).has_value ());
  auto recovery = Recovery::create ();
  REQUIRE ((recovery).has_value ());
  CHECK (recovery.value ().recovery_address ());
  CHECK_FALSE ((recovery.value ().invoke<void> (0)).has_value ());
  CHECK_FALSE ((Recovery::create (nullptr)).has_value ());
}
TEST_CASE ("native recovery entries preserve real scalar results and normal floating effects") {
  auto recovery = Recovery::create ();
  REQUIRE ((recovery).has_value ());
  std::int64_t a = -20, b = 62, out = 0;
  double d = 41.5;
  for (unsigned n = 0; n < 20; ++n) {
    auto integer = recovery.value ().invoke<std::int64_t> (reinterpret_cast<std::uintptr_t> (&add),
                                                           reinterpret_cast<std::uintptr_t> (&a),
                                                           reinterpret_cast<std::uintptr_t> (&b));
    REQUIRE ((integer).has_value ());
    CHECK ((integer).value () == 42);
    auto u = recovery.value ().invoke<std::uint64_t> (reinterpret_cast<std::uintptr_t> (&bits),
                                                      UINT64_C (0xdeadbeef12345678));
    REQUIRE ((u).has_value ());
    CHECK ((u).value () == UINT64_C (0xdeadbeef12345678));
    auto fp = recovery.value ().invoke<double> (reinterpret_cast<std::uintptr_t> (&floating),
                                                reinterpret_cast<std::uintptr_t> (&d));
    REQUIRE ((fp).has_value ());
    CHECK ((fp).value () == 42);
    REQUIRE ((recovery.value ().invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                                              reinterpret_cast<std::uintptr_t> (&a),
                                              reinterpret_cast<std::uintptr_t> (&out)))
               .has_value ());
    CHECK (out == a);
  }
  std::fenv_t environment;
  REQUIRE (std::fegetenv (&environment) == 0);
  std::feclearexcept (FE_ALL_EXCEPT);
  std::fesetround (FE_TONEAREST);
  auto effect
    = recovery.value ().invoke<void> (reinterpret_cast<std::uintptr_t> (&floating_effect));
  CHECK ((effect).has_value ());
  CHECK (std::fegetround () == FE_UPWARD);
  CHECK (std::fetestexcept (FE_INEXACT));
  REQUIRE (std::fesetenv (&environment) == 0);
  auto error = recovery.value ().invoke<void> (reinterpret_cast<std::uintptr_t> (&reported_error));
  REQUIRE_FALSE ((error).has_value ());
  CHECK (error.error ().message == "first checked error");
  REQUIRE ((recovery.value ().invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                                            reinterpret_cast<std::uintptr_t> (&b),
                                            reinterpret_cast<std::uintptr_t> (&out)))
             .has_value ());
  CHECK (out == b);
  auto moved = std::move ((recovery).value ());
  CHECK_FALSE (
    (recovery.value ().invoke<void> (reinterpret_cast<std::uintptr_t> (&store))).has_value ());
  REQUIRE ((moved.invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                                reinterpret_cast<std::uintptr_t> (&a),
                                reinterpret_cast<std::uintptr_t> (&out)))
             .has_value ());
  CHECK (out == a);
}
TEST_CASE (
  "checked BBV failures recover before prologues and after dynamic stack allocation at O0-O3") {
  for (unsigned level = 0; level < 4; ++level)
    for (bool at_entry : {true, false}) {
      CAPTURE (level);
      CAPTURE (at_entry);
      auto recovery = Recovery::create ();
      REQUIRE ((recovery).has_value ());
      bool fail = true;
      unsigned destroyed = 0, errors = 0;
      auto r = runtime (branching, level, (recovery).value (),
                        {.observe = [&] (auto &version, auto &) -> Result<void> {
                          Lifetime lifetime{destroyed};
                          bool yes = false;
                          for (auto &label : version.block ().labels ())
                            yes |= label.name () == "yes";
                          if (fail && (at_entry ? version.call_entry () : yes)) {
                            ++errors;
                            std::fesetround (FE_DOWNWARD);
                            std::feraiseexcept (FE_INEXACT);
                            return Error{ErrorCode::InvalidArgument,
                                         "injected live BBV compilation failure"};
                          }
                          return {};
                        }});
      execution::detail::NativeCalls calls ({});
      auto &signature = r->program ().preparation ().function ().signature ();
      std::int64_t out = -1;
      const std::array<ExecutionArgument, 2> args{ExecutionArgument (1),
                                                  ExecutionArgument (ScalarValue::pointer (
                                                    reinterpret_cast<std::uintptr_t> (&out)))};
      // Adapter construction runs before the native checkpoint and may set FP flags.
      // Warm only that adapter: the rejected MIR block remains cold for the retry.
      auto prepared = calls.call (r->entry_address (), signature, args, &(recovery).value ());
      REQUIRE_FALSE ((prepared).has_value ());
      CHECK (prepared.error ().message.find ("injected live BBV compilation failure")
             != std::string::npos);
      CHECK (errors == 1);
      CHECK (destroyed == r->statistics ().compilation_attempts);
      std::fenv_t environment;
      REQUIRE (std::fegetenv (&environment) == 0);
      std::feclearexcept (FE_ALL_EXCEPT);
      std::fesetround (FE_TONEAREST);
      auto failed = calls.call (r->entry_address (), signature, args, &(recovery).value ());
      REQUIRE_FALSE ((failed).has_value ());
      CHECK (failed.error ().message.find ("injected live BBV compilation failure")
             != std::string::npos);
      CHECK (failed.error ().message.find ("m/f [") != std::string::npos);
      CHECK (errors == 2);
      CHECK (destroyed == r->statistics ().compilation_attempts);
      CHECK (out == (at_entry ? -1 : 1));
      CHECK (std::fegetround () == FE_TONEAREST);
      CHECK (std::fetestexcept (FE_INEXACT) == 0);
      REQUIRE (std::fesetenv (&environment) == 0);
      fail = false;
      auto success = calls.call (r->entry_address (), signature, args, &(recovery).value ());
      INFO (((success).has_value () ? "" : success.error ().message));
      REQUIRE ((success).has_value ());
      REQUIRE (success.value ().size () == 1);
      CHECK ((((success).value ())[0].as_i64 ()).value () == 42);
      CHECK (out == 1);
      auto attempts = r->statistics ().compilation_attempts;
      REQUIRE (
        (calls.call (r->entry_address (), signature, args, &(recovery).value ())).has_value ());
      CHECK (r->statistics ().compilation_attempts == attempts);
      CHECK (calls.size () == 1);
    }
}
namespace {
struct Nested {
  const Recovery &recovery;
  Runtime &inner;
  execution::detail::NativeCalls &calls;
  unsigned calls_made = 0, destroyed = 0;
  bool inner_failed = false;
};
thread_local Nested *nested = nullptr;
std::int64_t host_reentry (std::int64_t value) {
  auto &n = *nested;
  Lifetime lifetime{n.destroyed};
  ++n.calls_made;
  auto &signature = n.inner.program ().preparation ().function ().signature ();
  const std::array<ExecutionArgument, 1> arguments{ExecutionArgument (value)};
  auto result = n.calls.call (n.inner.entry_address (), signature, arguments, &n.recovery);
  n.inner_failed = !(result).has_value ();
  if (!(result).has_value ()) return 7;
  return (((result).value ())[0].as_i64 ()).value ();
}
}  // namespace
TEST_CASE (
  "nested checked host reentry preserves C++ destructors and restores the outer recovery "
  "checkpoint") {
  auto recovery = Recovery::create ();
  REQUIRE ((recovery).has_value ());
  bool inner_fail = true, outer_fail = true;
  unsigned observers_destroyed = 0;
  auto inner = runtime (
    "m:module\nf:func i64,i64:x\nlocal p:s\nalloca s,4096\nbt done,x\nret 0\ndone:ret "
    "x\nendfunc\nendmodule\n",
    0, (recovery).value (), {.observe = [&] (auto &version, auto &) -> Result<void> {
      Lifetime lifetime{observers_destroyed};
      if (inner_fail && !version.call_entry ())
        return Error{ErrorCode::InvalidArgument, "inner failure"};
      return {};
    }});
  execution::detail::NativeCalls calls ({});
  Nested state{(recovery).value (), *inner, calls};
  nested = &state;
  auto outer = runtime (
    "m:module\np:proto i64,i64:x\nimport callback\nf:func i64,i64:x\nlocal i64:r\ncall "
    "p,callback,r,x\nbt done,r\nadd r,r,1\ndone:ret r\nendfunc\nendmodule\n",
    0, (recovery).value (),
    {.resolve = [] (auto &) -> Result<std::uint64_t> {
       return reinterpret_cast<std::uintptr_t> (&host_reentry);
     },
     .observe = [&] (auto &version, auto &) -> Result<void> {
       Lifetime lifetime{observers_destroyed};
       if (outer_fail && !version.call_entry () && state.calls_made)
         return Error{ErrorCode::InvalidArgument, "outer failure after host reentry"};
       return {};
     }});
  const std::array<ExecutionArgument, 1> arguments{ExecutionArgument (1)};
  auto &signature = outer->program ().preparation ().function ().signature ();
  auto first = calls.call (outer->entry_address (), signature, arguments, &(recovery).value ());
  REQUIRE_FALSE ((first).has_value ());
  CHECK (first.error ().message.find ("outer failure after host reentry") != std::string::npos);
  CHECK (state.inner_failed);
  CHECK (state.calls_made == 1);
  CHECK (state.destroyed == 1);
  outer_fail = false;
  auto second = calls.call (outer->entry_address (), signature, arguments, &(recovery).value ());
  REQUIRE ((second).has_value ());
  CHECK ((((second).value ())[0].as_i64 ()).value () == 7);
  CHECK (state.inner_failed);
  CHECK (state.calls_made == 2);
  CHECK (state.destroyed == 2);
  inner_fail = false;
  auto third = calls.call (outer->entry_address (), signature, arguments, &(recovery).value ());
  REQUIRE ((third).has_value ());
  CHECK ((((third).value ())[0].as_i64 ()).value () == 1);
  CHECK_FALSE (state.inner_failed);
  CHECK (state.destroyed == 3);
  CHECK (observers_destroyed
         == inner->statistics ().compilation_attempts + outer->statistics ().compilation_attempts);
  nested = nullptr;
}
TEST_CASE ("checked BBV recovery covers mixed block RBLK variadic and multiple-result adapters") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto recovery = Recovery::create ();
  REQUIRE ((recovery).has_value ());
  for (unsigned level = 0; level < 4; ++level)
    for (auto kind : {test::CallExample::Direct, test::CallExample::Block,
                      test::CallExample::ReturnBlock, test::CallExample::Variadic,
                      test::CallExample::VariadicBlock, test::CallExample::MultipleResults}) {
      CAPTURE (level);
      CAPTURE (unsigned (kind));
      auto source = test::call_example (kind);
      auto optimized
        = transforms::optimize (*source,
                                {.level = static_cast<transforms::OptimizationLevel> (level)});
      REQUIRE ((optimized).has_value ());
      auto prepared
        = BBVPreparation::prepare (*optimized.value ().find_function ("calls", "sum").get (),
                                   (target).value (), {.optimization_level = level});
      if (target.value ().abi () == target::ABI::X86Windows
          && kind == test::CallExample::MultipleResults) {
        REQUIRE_FALSE ((prepared).has_value ());
        CHECK (prepared.error ().message.find ("Windows does not support multiple return values")
               != std::string::npos);
        continue;
      }
      REQUIRE ((prepared).has_value ());
      auto program = BBVProgram::create (std::move ((prepared).value ()));
      REQUIRE ((program).has_value ());
      source.reset ();
      optimized = Error{ErrorCode::InvalidArgument, "source owner destroyed before recovery"};
      bool fail = true;
      auto r
        = Runtime::create (std::move ((program).value ()),
                           {.code_capacity = 1024 * 1024,
                            .failure_continuation = recovery.value ().recovery_address (),
                            .resolve = [] (auto &request) -> Result<std::uint64_t> {
                              if (!request.builtin)
                                return Error{ErrorCode::UnknownName,
                                             "unexpected recovery ABI fixture symbol"};
                              auto address = builtin_address (*request.builtin);
                              if (!address.has_value ()) return address.error ();
                              return address.value ();
                            },
                            .observe = [&] (auto &, auto &) -> Result<void> {
                              if (fail)
                                return Error{ErrorCode::InvalidArgument, "recover ABI adapter"};
                              return {};
                            }});
      REQUIRE ((r).has_value ());
      execution::detail::NativeCalls calls ({});
      auto &signature = ((r).value ())->program ().preparation ().function ().signature ();
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
      auto first
        = calls.call (((r).value ())->entry_address (), signature, args, &(recovery).value ());
      REQUIRE_FALSE ((first).has_value ());
      CHECK (first.error ().message.find ("recover ABI adapter") != std::string::npos);
      CHECK (bytes[0] == 20);
      CHECK (((r).value ())->statistics ().compiled_blocks == 0);
      fail = false;
      for (unsigned n = 0; n < 2; ++n) {
        auto values
          = calls.call (((r).value ())->entry_address (), signature, args, &(recovery).value ());
        INFO (((values).has_value () ? "" : values.error ().message));
        REQUIRE ((values).has_value ());
        if (kind == test::CallExample::ReturnBlock) {
          CHECK (values.value ().empty ());
          CHECK (bytes[0] == 42);
        } else if (kind == test::CallExample::MultipleResults) {
          REQUIRE (values.value ().size () == 2);
          CHECK ((((values).value ())[0].as_i64 ()).value () == 20);
          CHECK ((((values).value ())[1].as_i64 ()).value () == 22);
        } else {
          REQUIRE (values.value ().size () == 1);
          CHECK ((((values).value ())[0].as_i64 ()).value () == 42);
        }
      }
      CHECK (((r).value ())->statistics ().compilation_attempts
             == ((r).value ())->statistics ().compiled_blocks + 1);
      CHECK (calls.size () == 1);
    }
}
