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
  INFO ((context ? "" : context.error ().message));
  REQUIRE (context);
  auto optimized
    = transforms::optimize (**context,
                            {.level = static_cast<transforms::OptimizationLevel> (level)});
  REQUIRE (optimized);
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  auto prepared
    = BBVPreparation::prepare (*optimized->context ().modules ().begin ()->functions ().begin (),
                               *target, {.optimization_level = level});
  REQUIRE (prepared);
  auto program = BBVProgram::create (std::move (*prepared));
  REQUIRE (program);
  options.code_capacity = 1024 * 1024;
  options.failure_continuation = recovery.recovery_address ();
  if (!options.resolve)
    options.resolve = [] (auto &request) -> Result<std::uint64_t> {
      if (!request.builtin)
        return Error{ErrorCode::UnknownName, "unexpected recovery fixture symbol"};
      auto address = builtin_address (*request.builtin);
      if (!address) return address.error ();
      return *address;
    };
  auto result = Runtime::create (std::move (*program), std::move (options));
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
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
    REQUIRE (f);
    bool windows = abi == target::ABI::X86Windows;
    CHECK (f->preserved_count == (windows ? 8 : 6));
    CHECK (f->shadow_bytes == (windows ? 32 : 0));
    CHECK (f->checkpoint_offset == f->shadow_bytes);
    CHECK (f->checkpoint_offset + 8 == f->frame_bytes);
    CHECK ((8 + 16 - f->frame_bytes % 16) % 16 == 0);
    CHECK (f->red_zone_bytes == (windows ? 0 : 128));
    CHECK (f->parameters[0] == (windows ? R::CX : R::DI));
    CHECK (f->parameters[3] == (windows ? R::R9 : R::CX));
    std::set<R> saved;
    for (unsigned n = 0; n < f->preserved_count; ++n) {
      CHECK (saved.insert (f->preserved[n]).second);
      CHECK (
        (f->preserved[n] == R::BP || target::x86_callee_saved_register (abi, f->preserved[n])));
    }
    for (unsigned n = 0; n < 16; ++n)
      if (n != unsigned (R::SP))
        CHECK (saved.contains (static_cast<R> (n))
               == (n == unsigned (R::BP)
                   || target::x86_callee_saved_register (abi, static_cast<R> (n))));
  }
  CHECK_FALSE (codegen::detail::x86_invocation_frame (target::ABI::AArch64Linux));
  CHECK_FALSE (codegen::detail::x86_invocation_frame (static_cast<target::ABI> (99)));
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  CHECK_FALSE (make_invocation_entries (*target, 0));
  if (target->architecture () == target::Architecture::AArch64)
    CHECK_FALSE (make_invocation_entries (*target, 1));
  auto recovery = Recovery::create ();
  REQUIRE (recovery);
  CHECK (recovery->recovery_address ());
  CHECK_FALSE (recovery->invoke<void> (0));
  CHECK_FALSE (Recovery::create (nullptr));
}
TEST_CASE ("native recovery entries preserve real scalar results and normal floating effects") {
  auto recovery = Recovery::create ();
  REQUIRE (recovery);
  std::int64_t a = -20, b = 62, out = 0;
  double d = 41.5;
  for (unsigned n = 0; n < 20; ++n) {
    auto integer = recovery->invoke<std::int64_t> (reinterpret_cast<std::uintptr_t> (&add),
                                                   reinterpret_cast<std::uintptr_t> (&a),
                                                   reinterpret_cast<std::uintptr_t> (&b));
    REQUIRE (integer);
    CHECK (*integer == 42);
    auto u = recovery->invoke<std::uint64_t> (reinterpret_cast<std::uintptr_t> (&bits),
                                              UINT64_C (0xdeadbeef12345678));
    REQUIRE (u);
    CHECK (*u == UINT64_C (0xdeadbeef12345678));
    auto fp = recovery->invoke<double> (reinterpret_cast<std::uintptr_t> (&floating),
                                        reinterpret_cast<std::uintptr_t> (&d));
    REQUIRE (fp);
    CHECK (*fp == 42);
    REQUIRE (recovery->invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                                     reinterpret_cast<std::uintptr_t> (&a),
                                     reinterpret_cast<std::uintptr_t> (&out)));
    CHECK (out == a);
  }
  std::fenv_t environment;
  REQUIRE (std::fegetenv (&environment) == 0);
  std::feclearexcept (FE_ALL_EXCEPT);
  std::fesetround (FE_TONEAREST);
  auto effect = recovery->invoke<void> (reinterpret_cast<std::uintptr_t> (&floating_effect));
  CHECK (effect);
  CHECK (std::fegetround () == FE_UPWARD);
  CHECK (std::fetestexcept (FE_INEXACT));
  REQUIRE (std::fesetenv (&environment) == 0);
  auto error = recovery->invoke<void> (reinterpret_cast<std::uintptr_t> (&reported_error));
  REQUIRE_FALSE (error);
  CHECK (error.error ().message == "first checked error");
  REQUIRE (recovery->invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                                   reinterpret_cast<std::uintptr_t> (&b),
                                   reinterpret_cast<std::uintptr_t> (&out)));
  CHECK (out == b);
  auto moved = std::move (*recovery);
  CHECK_FALSE (recovery->invoke<void> (reinterpret_cast<std::uintptr_t> (&store)));
  REQUIRE (moved.invoke<void> (reinterpret_cast<std::uintptr_t> (&store),
                               reinterpret_cast<std::uintptr_t> (&a),
                               reinterpret_cast<std::uintptr_t> (&out)));
  CHECK (out == a);
}
TEST_CASE (
  "checked BBV failures recover before prologues and after dynamic stack allocation at O0-O3") {
  for (unsigned level = 0; level < 4; ++level)
    for (bool at_entry : {true, false}) {
      CAPTURE (level);
      CAPTURE (at_entry);
      auto recovery = Recovery::create ();
      REQUIRE (recovery);
      bool fail = true;
      unsigned destroyed = 0, errors = 0;
      auto r = runtime (branching, level, *recovery,
                        {.observe = [&] (auto &version, auto &) -> Result<void> {
                          Lifetime lifetime{destroyed};
                          bool yes = false;
                          for (auto label : version.block ().labels ())
                            yes |= label->name () == "yes";
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
      auto prepared = calls.call (r->entry_address (), signature, args, &*recovery);
      REQUIRE_FALSE (prepared);
      CHECK (prepared.error ().message.find ("injected live BBV compilation failure")
             != std::string::npos);
      CHECK (errors == 1);
      CHECK (destroyed == r->statistics ().compilation_attempts);
      std::fenv_t environment;
      REQUIRE (std::fegetenv (&environment) == 0);
      std::feclearexcept (FE_ALL_EXCEPT);
      std::fesetround (FE_TONEAREST);
      auto failed = calls.call (r->entry_address (), signature, args, &*recovery);
      REQUIRE_FALSE (failed);
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
      auto success = calls.call (r->entry_address (), signature, args, &*recovery);
      INFO ((success ? "" : success.error ().message));
      REQUIRE (success);
      REQUIRE (success->size () == 1);
      CHECK (*(*success)[0].as_i64 () == 42);
      CHECK (out == 1);
      auto attempts = r->statistics ().compilation_attempts;
      REQUIRE (calls.call (r->entry_address (), signature, args, &*recovery));
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
  n.inner_failed = !result;
  if (!result) return 7;
  return *(*result)[0].as_i64 ();
}
}  // namespace
TEST_CASE (
  "nested checked host reentry preserves C++ destructors and restores the outer recovery "
  "checkpoint") {
  auto recovery = Recovery::create ();
  REQUIRE (recovery);
  bool inner_fail = true, outer_fail = true;
  unsigned observers_destroyed = 0;
  auto inner = runtime (
    "m:module\nf:func i64,i64:x\nlocal p:s\nalloca s,4096\nbt done,x\nret 0\ndone:ret "
    "x\nendfunc\nendmodule\n",
    0, *recovery, {.observe = [&] (auto &version, auto &) -> Result<void> {
      Lifetime lifetime{observers_destroyed};
      if (inner_fail && !version.call_entry ())
        return Error{ErrorCode::InvalidArgument, "inner failure"};
      return {};
    }});
  execution::detail::NativeCalls calls ({});
  Nested state{*recovery, *inner, calls};
  nested = &state;
  auto outer = runtime (
    "m:module\np:proto i64,i64:x\nimport callback\nf:func i64,i64:x\nlocal i64:r\ncall "
    "p,callback,r,x\nbt done,r\nadd r,r,1\ndone:ret r\nendfunc\nendmodule\n",
    0, *recovery,
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
  auto first = calls.call (outer->entry_address (), signature, arguments, &*recovery);
  REQUIRE_FALSE (first);
  CHECK (first.error ().message.find ("outer failure after host reentry") != std::string::npos);
  CHECK (state.inner_failed);
  CHECK (state.calls_made == 1);
  CHECK (state.destroyed == 1);
  outer_fail = false;
  auto second = calls.call (outer->entry_address (), signature, arguments, &*recovery);
  REQUIRE (second);
  CHECK (*(*second)[0].as_i64 () == 7);
  CHECK (state.inner_failed);
  CHECK (state.calls_made == 2);
  CHECK (state.destroyed == 2);
  inner_fail = false;
  auto third = calls.call (outer->entry_address (), signature, arguments, &*recovery);
  REQUIRE (third);
  CHECK (*(*third)[0].as_i64 () == 1);
  CHECK_FALSE (state.inner_failed);
  CHECK (state.destroyed == 3);
  CHECK (observers_destroyed
         == inner->statistics ().compilation_attempts + outer->statistics ().compilation_attempts);
  nested = nullptr;
}
TEST_CASE ("checked BBV recovery covers mixed block RBLK variadic and multiple-result adapters") {
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  auto recovery = Recovery::create ();
  REQUIRE (recovery);
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
      REQUIRE (optimized);
      auto prepared = BBVPreparation::prepare (*optimized->find_function ("calls", "sum"), *target,
                                               {.optimization_level = level});
      if (target->abi () == target::ABI::X86Windows && kind == test::CallExample::MultipleResults) {
        REQUIRE_FALSE (prepared);
        CHECK (prepared.error ().message.find ("Windows does not support multiple return values")
               != std::string::npos);
        continue;
      }
      REQUIRE (prepared);
      auto program = BBVProgram::create (std::move (*prepared));
      REQUIRE (program);
      source.reset ();
      optimized = Error{ErrorCode::InvalidArgument, "source owner destroyed before recovery"};
      bool fail = true;
      auto r
        = Runtime::create (std::move (*program),
                           {.code_capacity = 1024 * 1024,
                            .failure_continuation = recovery->recovery_address (),
                            .resolve = [] (auto &request) -> Result<std::uint64_t> {
                              if (!request.builtin)
                                return Error{ErrorCode::UnknownName,
                                             "unexpected recovery ABI fixture symbol"};
                              auto address = builtin_address (*request.builtin);
                              if (!address) return address.error ();
                              return *address;
                            },
                            .observe = [&] (auto &, auto &) -> Result<void> {
                              if (fail)
                                return Error{ErrorCode::InvalidArgument, "recover ABI adapter"};
                              return {};
                            }});
      REQUIRE (r);
      execution::detail::NativeCalls calls ({});
      auto &signature = (*r)->program ().preparation ().function ().signature ();
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
      auto first = calls.call ((*r)->entry_address (), signature, args, &*recovery);
      REQUIRE_FALSE (first);
      CHECK (first.error ().message.find ("recover ABI adapter") != std::string::npos);
      CHECK (bytes[0] == 20);
      CHECK ((*r)->statistics ().compiled_blocks == 0);
      fail = false;
      for (unsigned n = 0; n < 2; ++n) {
        auto values = calls.call ((*r)->entry_address (), signature, args, &*recovery);
        INFO ((values ? "" : values.error ().message));
        REQUIRE (values);
        if (kind == test::CallExample::ReturnBlock) {
          CHECK (values->empty ());
          CHECK (bytes[0] == 42);
        } else if (kind == test::CallExample::MultipleResults) {
          REQUIRE (values->size () == 2);
          CHECK (*(*values)[0].as_i64 () == 20);
          CHECK (*(*values)[1].as_i64 () == 22);
        } else {
          REQUIRE (values->size () == 1);
          CHECK (*(*values)[0].as_i64 () == 42);
        }
      }
      CHECK ((*r)->statistics ().compilation_attempts == (*r)->statistics ().compiled_blocks + 1);
      CHECK (calls.size () == 1);
    }
}
