/* Licensed under the MIT License. Raw ABI entries with nested recovery. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/detail/NativeBoundary.hpp"
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/Execution/detail/NativeCalls.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/Calls.hpp"
#include <cstdarg>
#include <cfenv>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
using namespace mirnext::execution;
namespace {
using Boundary = execution::detail::NativeBoundary;
using Runtime = execution::detail::BBVRuntime;
using Options = execution::detail::BBVRuntimeOptions;
auto state () { return execution::detail::native_boundary_state_statistics (); }
std::uintptr_t gate (Boundary &boundary, std::uintptr_t target, unsigned x87 = 0) {
  const std::array targets{execution::detail::NativeBoundaryTarget{target, x87}};
  auto entries = boundary.add_entries (targets);
  REQUIRE ((entries).has_value ());
  REQUIRE (entries.value ().size () == 1);
  return ((entries).value ())[0];
}
std::unique_ptr<Runtime> runtime (Program &source, std::string_view module, std::string_view name,
                                  unsigned level, const Boundary &boundary, Options options = {}) {
  auto optimized
    = transforms::optimize (source, {.level = static_cast<transforms::OptimizationLevel> (level)});
  REQUIRE ((optimized).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto preparation
    = BBVPreparation::prepare (*optimized.value ().find_function (module, name).get (),
                               (target).value (), {.optimization_level = level});
  INFO (((preparation).has_value () ? "" : preparation.error ().message));
  REQUIRE ((preparation).has_value ());
  auto program = BBVProgram::create (std::move ((preparation).value ()));
  REQUIRE ((program).has_value ());
  options.code_capacity = 1024 * 1024;
  options.failure_continuation = boundary.recovery ().recovery_address ();
  if (!options.resolve)
    options.resolve = [] (auto &request) -> Result<std::uint64_t> {
      if (!request.builtin)
        return Error{ErrorCode::UnknownName, "unexpected boundary fixture symbol"};
      auto address = builtin_address (*request.builtin);
      if (!address.has_value ()) return address.error ();
      return address.value ();
    };
  auto result = Runtime::create (std::move ((program).value ()), std::move (options));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
std::unique_ptr<Runtime> runtime (std::string_view text, unsigned level, const Boundary &boundary,
                                  Options options = {}) {
  auto source = serialization::parse_text (text);
  INFO (((source).has_value () ? "" : source.error ().message));
  REQUIRE ((source).has_value ());
  return runtime ((source).value (), "m", "f", level, boundary, std::move (options));
}
std::int64_t integers (std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d,
                       std::int64_t e, std::int64_t f, std::int64_t g, std::int64_t h,
                       std::int64_t i, std::int64_t j) {
  CHECK (state ().depth == 1);
  return a + b + c + d + e + f + g + h + i + j;
}
double floating (double a, double b, double c, double d, double e, double f, double g, double h,
                 double i, double j) {
  CHECK (state ().depth == 1);
  return a + b + c + d + e + f + g + h + i + j;
}
double variadic (int count, ...) {
  CHECK (state ().depth == 1);
  va_list args;
  va_start (args, count);
  double sum = 0;
  for (int n = 0; n < count; ++n) {
    sum += va_arg (args, int);
    sum += va_arg (args, double);
  }
  va_end (args);
  return sum;
}
struct Pair {
  double a, b;
};
Pair pair (Pair value, double add) {
  CHECK (state ().depth == 1);
  return {value.a + add, value.b - add};
}
double variadic_blocks (int count, ...) {
  CHECK (state ().depth == 1);
  va_list args;
  va_start (args, count);
  double sum = 0;
  for (int n = 0; n < count; ++n) {
    auto p = va_arg (args, Pair);
    sum += p.a + p.b;
  }
  va_end (args);
  return sum;
}
struct Large {
  std::array<std::uint64_t, 6> words;
};
Large aggregate (Large value, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                 std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h,
                 std::uint64_t i) {
  CHECK (state ().depth == 1);
  value.words[0] += a + b + c + d + e + f + g + h + i;
  return value;
}
long double extended (long double value) {
  CHECK (state ().depth == 1);
  return value + 0.5L;
}
void floating_effect () {
  CHECK (state ().depth == 1);
  std::fesetround (FE_UPWARD);
  std::feraiseexcept (FE_INEXACT);
}
unsigned x87_results () {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  return target.value ().abi () == target::ABI::X86SysV && sizeof (long double) > 8 ? 1 : 0;
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
TEST_CASE (
  "raw native boundary frames preserve original argument stacks and validate gate metadata") {
  for (auto abi : {target::ABI::X86SysV, target::ABI::X86Windows}) {
    auto f = codegen::detail::x86_boundary_frame (abi);
    REQUIRE ((f).has_value ());
    bool windows = abi == target::ABI::X86Windows;
    CHECK (f.value ().checkpoint_offset == (windows ? 32 : 0));
    CHECK (f.value ().checkpoint_offset % 16 == 0);
    CHECK (f.value ().frame_bytes == f.value ().checkpoint_offset + sizeof (NativeInvocationState));
    CHECK (f.value ().frame_bytes % 16 == 0);
    CHECK (f.value ().red_zone_bytes == (windows ? 0 : 128));
    for (auto original : {UINT64_C (0x100000000), UINT64_C (0x100000008)}) {
      auto flags_sp = original - f.value ().red_zone_bytes - 8;
      auto frame_sp = (flags_sp & ~UINT64_C (15)) - f.value ().frame_bytes;
      CHECK (frame_sp % 16 == 0);
      CHECK (frame_sp + f.value ().frame_bytes <= flags_sp);
      CHECK (flags_sp + f.value ().red_zone_bytes + 8 == original);
    }
  }
  CHECK_FALSE ((codegen::detail::x86_boundary_frame (target::ABI::AArch64Linux)).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  CHECK_FALSE ((make_native_boundary_entries ((target).value (), 0, 0)).has_value ());
  CHECK_FALSE ((make_native_boundary_gate ((target).value (), 0, 0)).has_value ());
  if (target.value ().architecture () == target::Architecture::AArch64) {
    CHECK_FALSE ((make_native_boundary_entries ((target).value (), 1, 1)).has_value ());
    CHECK_FALSE ((make_native_boundary_gate ((target).value (), 1, 1)).has_value ());
  }
  auto boundary = Boundary::create ();
  REQUIRE ((boundary).has_value ());
  auto empty = boundary.value ().add_entries ({});
  REQUIRE ((empty).has_value ());
  CHECK (empty.value ().empty ());
  const std::array bad{execution::detail::NativeBoundaryTarget{0, 0}};
  CHECK_FALSE ((boundary.value ().add_entries (bad)).has_value ());
  const std::array bad_x87{
    execution::detail::NativeBoundaryTarget{reinterpret_cast<std::uintptr_t> (&integers), 3}};
  CHECK_FALSE ((boundary.value ().add_entries (bad_x87)).has_value ());
  CHECK_FALSE ((Boundary::create (nullptr)).has_value ());
  CHECK (state ().depth == 0);
}
TEST_CASE (
  "raw native gates forward exhausted register banks aggregates and unknown scalar block varargs") {
  auto boundary = Boundary::create ();
  REQUIRE ((boundary).has_value ());
  auto integer = reinterpret_cast<decltype (&integers)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&integers)));
  auto fp = reinterpret_cast<decltype (&floating)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&floating)));
  auto va = reinterpret_cast<decltype (&variadic)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&variadic)));
  auto block_va = reinterpret_cast<decltype (&variadic_blocks)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&variadic_blocks)));
  auto small = reinterpret_cast<decltype (&pair)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&pair)));
  auto large = reinterpret_cast<decltype (&aggregate)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&aggregate)));
  auto ld = reinterpret_cast<decltype (&extended)> (
    gate ((boundary).value (), reinterpret_cast<std::uintptr_t> (&extended), x87_results ()));
  auto cached = state ().cached_frames;
  for (unsigned n = 0; n < 20; ++n) {
    CHECK (integer (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
    CHECK (fp (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
    CHECK (va (10, 1, 0.5, 2, 1.5, 3, 2.5, 4, 3.5, 5, 4.5, 6, 5.5, 7, 6.5, 8, 7.5, 9, 8.5, 10, 9.5)
           == 105);
    CHECK (block_va (6, Pair{1, 2}, Pair{3, 4}, Pair{5, 6}, Pair{7, 8}, Pair{9, 10}, Pair{11, 12})
           == 78);
    auto p = small ({20, 22}, 2);
    CHECK (p.a == 22);
    CHECK (p.b == 20);
    auto l = large ({{1, 2, 3, 4, 5, 6}}, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    CHECK (l.words == (std::array<std::uint64_t, 6>{46, 2, 3, 4, 5, 6}));
    CHECK (ld (41.5L) == 42);
    CHECK (state ().depth == 0);
    if (!n) cached = state ().cached_frames;
    CHECK (state ().cached_frames == cached);
  }
  auto moved = std::move ((boundary).value ());
  CHECK (integer (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
  CHECK_FALSE ((boundary.value ().add_entries ({})).has_value ());
  CHECK (moved.code_batches () == 9);
  CHECK (moved.code_bytes () > 7 * native_boundary_gate_bytes);
  std::fenv_t env;
  REQUIRE (std::fegetenv (&env) == 0);
  std::feclearexcept (FE_ALL_EXCEPT);
  std::fesetround (FE_TONEAREST);
  auto effect = reinterpret_cast<void (*) ()> (
    gate (moved, reinterpret_cast<std::uintptr_t> (&floating_effect)));
  effect ();
  CHECK (std::fegetround () == FE_UPWARD);
  CHECK (std::fetestexcept (FE_INEXACT));
  REQUIRE (std::fesetenv (&env) == 0);
}
TEST_CASE ("raw x86 native boundary return-slot restoration preserves result flags") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  if (target.value ().architecture () != target::Architecture::X86_64) return;
  auto boundary = Boundary::create ();
  REQUIRE ((boundary).has_value ());
  const bool windows = target.value ().abi () == target::ABI::X86Windows;
  // Load requested flags from the first argument, then return through the
  // real leave callback. Two patterns detect a return PC substituted as flags.
  const std::array<std::uint8_t, 3> callee{std::uint8_t (windows ? 0x51 : 0x57), 0x9d, 0xc3};
  auto reserved = CodeMemory ().reserve (std::array<std::size_t, 2>{callee.size (), 26});
  REQUIRE ((reserved).has_value ());
  auto address = reserved.value ().address (0);
  REQUIRE ((address).has_value ());
  const auto entry = gate ((boundary).value (), (address).value ());
  // Save host flags, provide aligned home space, call the gate, and capture
  // flags immediately. Restore host flags before returning even on failure.
  const auto shadow = windows ? 32u : 0u;
  std::vector<std::uint8_t> probe{0x9c, 0x48, 0x83, 0xec, std::uint8_t (shadow), 0x49, 0xbb};
  for (unsigned n = 0; n < 8; ++n) probe.push_back (std::uint8_t (entry >> (n * 8)));
  probe.insert (probe.end (), {0x41, 0xff, 0xd3, 0x9c, 0x58, 0x48, 0x83, 0xc4,
                               std::uint8_t (shadow), 0x9d, 0xc3});
  const std::array<std::span<const std::uint8_t>, 2> bytes{callee, probe};
  auto code = reserved.value ().publish (bytes);
  REQUIRE ((code).has_value ());
  auto call = reinterpret_cast<std::uint64_t (*) (std::uint64_t)> (((code).value ())[1].address ());
  for (unsigned n = 0; n < 8; ++n)
    for (auto requested : {0x202u, 0xad7u}) {
      auto flags = call (requested);
      CHECK ((flags & 0x408d5) == (requested & 0x408d5));
      CHECK (state ().depth == 0);
    }
}
TEST_CASE (
  "raw BBV entries recover before prologues and inside dynamic frames then retry at O0-O3") {
  for (unsigned level = 0; level < 4; ++level)
    for (bool at_entry : {true, false}) {
      CAPTURE (level);
      CAPTURE (at_entry);
      auto boundary = Boundary::create ();
      REQUIRE ((boundary).has_value ());
      bool fail = true;
      unsigned destroyed = 0;
      auto r = runtime (branching, level, (boundary).value (),
                        {.observe = [&] (auto &version, auto &) -> Result<void> {
                          Lifetime lifetime{destroyed};
                          CHECK (state ().depth == 1);
                          bool yes = false;
                          for (auto &label : version.block ().labels ())
                            yes |= label.name () == "yes";
                          if (fail && (at_entry ? version.call_entry () : yes))
                            return Error{ErrorCode::InvalidArgument, "raw BBV cold failure"};
                          return {};
                        }});
      auto entry = gate ((boundary).value (), r->entry_address ());
      auto call = reinterpret_cast<std::int64_t (*) (std::int64_t, std::int64_t *)> (entry);
      std::int64_t out = -1;
      {
        ExecutionInvocation invocation;
        CHECK (call (1, &out) == 0);
        auto status = invocation.status ();
        REQUIRE_FALSE ((status).has_value ());
        CHECK (status.error ().message.find ("raw BBV cold failure") != std::string::npos);
      }
      CHECK (out == (at_entry ? -1 : 1));
      CHECK (state ().depth == 0);
      CHECK (destroyed == r->statistics ().compilation_attempts);
      fail = false;
      {
        ExecutionInvocation invocation;
        CHECK (call (1, &out) == 42);
        REQUIRE ((invocation.status ()).has_value ());
        CHECK (call (0, &out) == 17);
        REQUIRE ((invocation.status ()).has_value ());
      }
      auto count = r->statistics ().compilation_attempts, frames = state ().cached_frames;
      for (unsigned n = 0; n < 10; ++n) CHECK (call (1, &out) == 42);
      CHECK (r->statistics ().compilation_attempts == count);
      CHECK (state ().cached_frames == frames);
      CHECK (state ().depth == 0);
    }
}
namespace {
struct Nested {
  std::int64_t (*entry) (std::int64_t);
  unsigned called = 0, destroyed = 0;
  bool failed = false;
};
thread_local Nested *nested = nullptr;
std::int64_t host_reentry (std::int64_t value) {
  auto &n = *nested;
  Lifetime lifetime{n.destroyed};
  ++n.called;
  CHECK (state ().depth == 1);
  ExecutionInvocation invocation;
  auto result = n.entry (value);
  auto status = invocation.status ();
  n.failed = !(status).has_value ();
  CHECK (state ().depth == 1);
  return (status).has_value () ? result : 7;
}
}  // namespace
TEST_CASE (
  "nested raw callback reentry recovers locally and leaves outer C++ destructors and checkpoints "
  "intact") {
  auto boundary = Boundary::create ();
  REQUIRE ((boundary).has_value ());
  bool inner_fail = true, outer_fail = true;
  auto inner = runtime (
    "m:module\nf:func i64,i64:x\nlocal p:s\nalloca s,4096\nbt done,x\nret 0\ndone:ret "
    "x\nendfunc\nendmodule\n",
    0, (boundary).value (), {.observe = [&] (auto &version, auto &) -> Result<void> {
      CHECK (state ().depth == 2);
      if (inner_fail && !version.call_entry ())
        return Error{ErrorCode::InvalidArgument, "inner raw failure"};
      return {};
    }});
  Nested nesting{reinterpret_cast<std::int64_t (*) (std::int64_t)> (
    gate ((boundary).value (), inner->entry_address ()))};
  nested = &nesting;
  auto outer = runtime (
    "m:module\np:proto i64,i64:x\nimport callback\nf:func i64,i64:x\nlocal i64:r\ncall "
    "p,callback,r,x\nbt done,r\nadd r,r,1\ndone:ret r\nendfunc\nendmodule\n",
    0, (boundary).value (),
    {.resolve = [] (auto &) -> Result<std::uint64_t> {
       return reinterpret_cast<std::uintptr_t> (&host_reentry);
     },
     .observe = [&] (auto &version, auto &) -> Result<void> {
       CHECK (state ().depth == 1);
       if (outer_fail && !version.call_entry () && nesting.called)
         return Error{ErrorCode::InvalidArgument, "outer raw failure"};
       return {};
     }});
  auto call = reinterpret_cast<std::int64_t (*) (std::int64_t)> (
    gate ((boundary).value (), outer->entry_address ()));
  {
    ExecutionInvocation invocation;
    CHECK (call (1) == 0);
    auto status = invocation.status ();
    REQUIRE_FALSE ((status).has_value ());
    CHECK (status.error ().message.find ("outer raw failure") != std::string::npos);
  }
  CHECK (nesting.called == 1);
  CHECK (nesting.destroyed == 1);
  CHECK (nesting.failed);
  CHECK (state ().depth == 0);
  outer_fail = false;
  {
    ExecutionInvocation invocation;
    CHECK (call (1) == 7);
    REQUIRE ((invocation.status ()).has_value ());
  }
  CHECK (nesting.called == 2);
  CHECK (nesting.destroyed == 2);
  CHECK (nesting.failed);
  CHECK (state ().depth == 0);
  inner_fail = false;
  {
    ExecutionInvocation invocation;
    CHECK (call (1) == 1);
    REQUIRE ((invocation.status ()).has_value ());
  }
  CHECK (nesting.destroyed == 3);
  CHECK_FALSE (nesting.failed);
  CHECK (state ().depth == 0);
  nested = nullptr;
}
TEST_CASE (
  "raw BBV variadic block RBLK and long double entries recover their actual native result banks") {
  auto boundary = Boundary::create ();
  REQUIRE ((boundary).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (auto kind : {test::CallExample::Block, test::CallExample::ReturnBlock,
                    test::CallExample::Variadic, test::CallExample::VariadicBlock}) {
    auto source = test::call_example (kind);
    bool fail = true;
    auto r = runtime (*source, "calls", "sum", 0, (boundary).value (),
                      {.observe = [&] (auto &, auto &) -> Result<void> {
                        if (fail) return Error{ErrorCode::InvalidArgument, "raw ABI failure"};
                        return {};
                      }});
    source.reset ();
    auto entry = gate ((boundary).value (), r->entry_address ());
    std::array<std::int64_t, 2> pair{20, 22};
    execution::detail::NativeCalls calls ({});
    auto &signature = r->program ().preparation ().function ().signature ();
    std::vector<ExecutionArgument> arguments;
    if (kind == test::CallExample::Block)
      arguments.push_back (ExecutionArgument::block (std::as_bytes (std::span (pair))));
    else if (kind == test::CallExample::ReturnBlock)
      arguments.push_back (
        ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (pair))));
    else if (kind == test::CallExample::Variadic) {
      arguments.emplace_back (0);
      arguments.emplace_back (20);
      arguments.emplace_back (22);
    } else {
      arguments.emplace_back (0);
      arguments.push_back (ExecutionArgument::block (std::as_bytes (std::span (pair))));
    }
    auto call = [&] () -> std::int64_t {
      // MIR BLK classifications/RBLK are explicit ABI contracts, not arbitrary
      // C++ struct signatures. The typed adapter supplies their exact layout.
      auto values = calls.call (entry, signature, arguments);
      INFO (((values).has_value () ? "" : values.error ().message));
      REQUIRE ((values).has_value ());
      return kind == test::CallExample::ReturnBlock ? pair[0]
                                                    : (((values).value ())[0].as_i64 ()).value ();
    };
    auto failed = calls.call (entry, signature, arguments);
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("raw ABI failure") != std::string::npos);
    CHECK (pair[0] == 20);
    CHECK (state ().depth == 0);
    fail = false;
    {
      ExecutionInvocation invocation;
      CHECK (call () == 42);
      REQUIRE ((invocation.status ()).has_value ());
      CHECK (call () == 42);
      REQUIRE ((invocation.status ()).has_value ());
    }
    CHECK (state ().depth == 0);
  }
  Program source;
  auto module = mirnext::ir::detail::access (source).create_module ("m");
  REQUIRE ((module).has_value ());
  const Type ld (Type::Kind::LongDouble);
  auto function
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{ld}, {ld}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  // Pass the value through the actual native argument/result banks. This
  // isolated runtime fixture has no loaded data image for literal symbols.
  InstructionBuilder b ((block).value ());
  REQUIRE ((b.ret (Operand (
              *mirnext::ir::detail::ReferenceAccess::pointers (function.value ().arguments ())[0])))
             .has_value ());
  bool fail = true;
  auto r = runtime (source, "m", "f", 0, (boundary).value (),
                    {.observe = [&] (auto &, auto &) -> Result<void> {
                      if (fail) return Error{ErrorCode::InvalidArgument, "raw long double failure"};
                      return {};
                    }});
  auto call = reinterpret_cast<long double (*) (long double)> (
    gate ((boundary).value (), r->entry_address (), x87_results ()));
  for (unsigned n = 0; n < 12; ++n) {
    ExecutionInvocation invocation;
    CHECK (call (42.0L) == 0);
    REQUIRE_FALSE ((invocation.status ()).has_value ());
    CHECK (state ().depth == 0);
  }
  fail = false;
  for (unsigned n = 0; n < 12; ++n) {
    ExecutionInvocation invocation;
    auto value = call (42.0L);
    auto status = invocation.status ();
    INFO (((status).has_value () ? "" : status.error ().message));
    CHECK (value == 42);
    REQUIRE ((status).has_value ());
    CHECK (state ().depth == 0);
  }
}
