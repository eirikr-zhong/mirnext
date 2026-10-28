/* Licensed under the MIT License. Public owning session block execution. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Execution/detail/NativeBoundary.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include <bit>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
auto parse (std::string text) {
  if (target::TargetMachine::native ().value ().architecture () == target::Architecture::X86_64)
    for (auto [from, to] :
         {std::pair{":r5", ":r9"}, std::pair{":r19", ":r12"}, std::pair{":v20", ":xmm3"}})
      for (auto p = text.find (from); p != std::string::npos;
           p = text.find (from, p + std::string_view (to).size ()))
        text.replace (p, std::string_view (from).size (), to);
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::move ((c).value ());
}
ExecutionSession session (const Program &c, unsigned level = 2, LinkOptions options = {}) {
  options.compilation = JITCompilation::BlockVersioning;
  auto result = ExecutionSession::
    create (c,
            {.optimization
             = transforms::OptimizationOptions{.level = static_cast<transforms::OptimizationLevel> (
                                                 level)},
             .link = std::move (options)});
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
FunctionHandle find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE ((f).has_value ());
  return (f).value ();
}
auto call (const FunctionHandle &f, std::span<const ExecutionArgument> args = {}) {
  auto values = f.call (args);
  INFO (((values).has_value () ? "" : values.error ().message));
  REQUIRE ((values).has_value ());
  return std::move ((values).value ());
}
auto i64 (const FunctionHandle &f, std::span<const std::int64_t> args = {}) {
  auto value = f.call_i64 (args);
  INFO (((value).has_value () ? "" : value.error ().message));
  REQUIRE ((value).has_value ());
  return (value).value ();
}
template <class T>
T read (const mirnext::execution::DataHandle &data) {
  REQUIRE (data.bytes ().size () == sizeof (T));
  T value;
  std::memcpy (&value, data.bytes ().data (), sizeof value);
  return value;
}
constexpr auto branching
  = "m:module\nf:func i64,i64:x,p:out\nlocal p:stack,i64:r\nalloca stack,4096\nmov i64:(out),x\nbt "
    "yes,x\nmov r,17\njmp done\nyes:add r,x,41\ndone:ret r\nendfunc\nunused:func i64\nret "
    "99\nendfunc\nendmodule\n";
}  // namespace
TEST_CASE (
  "BBV session keeps blocks cold and retains entries independently of source and session") {
  std::optional<FunctionHandle> retained;
  std::optional<NativeEntry> entry;
  unsigned observed = 0;
  {
    auto c = parse (branching);
    const auto before = dump (c);
    auto s = session (c, 0, {.block_versioning = {.observer = [&] (auto &event) -> Result<void> {
                               CHECK (event.compiled.verify ().has_value ());
                               ++observed;
                               return {};
                             }}});
    retained = find (s, "m", "f");
    auto unused = find (s, "m", "unused");
    CHECK (dump (c) == before);
    CHECK (s.statistics ().compiled_blocks == 0);
    CHECK (observed == 0);
    CHECK (retained->compilation_state () == FunctionCompilationState::Uncompiled);
    REQUIRE (retained->block_statistics ());
    CHECK_FALSE ((retained->artifact ()).has_value ());
    auto native = retained->native_entry ();
    REQUIRE ((native).has_value ());
    entry = std::move ((native).value ());
    auto address = entry->address ();
    REQUIRE ((retained->compile ()).has_value ());
    CHECK (observed == 1);
    CHECK (retained->block_statistics ()->compiled_blocks == 1);
    REQUIRE ((retained->compile ()).has_value ());
    CHECK (observed == 1);
    CHECK (entry->address () == address);
    CHECK (unused.compilation_state () == FunctionCompilationState::Uncompiled);
    CHECK (s.statistics ().compiled_functions == 1);
  }
  std::int64_t out = -1;
  std::array args{ExecutionArgument (1), ExecutionArgument (ScalarValue::pointer (
                                           reinterpret_cast<std::uintptr_t> (&out)))};
  CHECK ((call (*retained, args)[0].as_i64 ()).value () == 42);
  CHECK (out == 1);
  auto compiled = observed;
  CHECK (retained->compilation_state () == FunctionCompilationState::PartiallyCompiled);
  CHECK_FALSE ((retained->artifact ()).has_value ());
  auto address = entry->address ();
  retained.reset ();
  ExecutionInvocation invocation;
  CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t, std::int64_t *)> (address) (1, &out)
         == 42);
  REQUIRE ((invocation.status ()).has_value ());
  CHECK (observed == compiled);
}
TEST_CASE ("BBV session executes every call ABI fixture at O0-O3 without whole function fallback") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned n = 0; n <= unsigned (test::CallExample::Inline); ++n) {
      CAPTURE (level);
      CAPTURE (n);
      auto kind = static_cast<test::CallExample> (n);
      auto c = test::call_example (kind);
      if (target::TargetMachine::native ().value ().abi () == target::ABI::X86Windows
          && kind == test::CallExample::MultipleResults) {
        auto rejected
          = ExecutionSession::create (*c,
                                      {.link = {.compilation = JITCompilation::BlockVersioning}});
        REQUIRE_FALSE ((rejected).has_value ());
        CHECK (rejected.error ().message.find ("Windows does not support multiple return values")
               != std::string::npos);
        continue;
      }
      auto s = session (*c, level);
      c.reset ();
      auto f = find (s, "calls", "sum");
      CHECK (f.block_statistics ()->compiled_blocks == 0);
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        std::array<std::int64_t, 2> pair{20, 22};
        std::vector<ExecutionArgument> args;
        if (kind == test::CallExample::Block)
          args.push_back (ExecutionArgument::block (std::as_bytes (std::span (pair))));
        else if (kind == test::CallExample::ReturnBlock)
          args.push_back (
            ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (pair))));
        else if (kind == test::CallExample::VariadicBlock) {
          args.emplace_back (0);
          args.push_back (ExecutionArgument::block (std::as_bytes (std::span (pair))));
        } else {
          if (kind == test::CallExample::Variadic) args.emplace_back (0);
          args.emplace_back (20);
          args.emplace_back (22);
        }
        auto values = call (f, args);
        if (kind == test::CallExample::ReturnBlock) {
          CHECK (values.empty ());
          CHECK (pair[0] == 42);
        } else if (kind == test::CallExample::MultipleResults) {
          REQUIRE (values.size () == 2);
          CHECK ((values[0].as_i64 ()).value () == 20);
          CHECK ((values[1].as_i64 ()).value () == 22);
        } else {
          REQUIRE (values.size () == 1);
          CHECK ((values[0].as_i64 ()).value () == 42);
        }
        CHECK_FALSE ((f.artifact ()).has_value ());
        CHECK (s.statistics ().compiled_blocks > 0);
      }
      CHECK (i64 (find (s, "calls", "entry")) == 42);
    }
}
TEST_CASE ("BBV session shares global layouts expression initializers and cross module data") {
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned n = 0; n <= unsigned (test::VariableExample::Narrow); ++n) {
      CAPTURE (level);
      CAPTURE (n);
      auto kind = static_cast<test::VariableExample> (n);
      auto c = test::variable_example (kind);
      auto s = session (*c, level);
      c.reset ();
      CHECK (s.statistics ().expressions
             == (kind == test::VariableExample::ExpressionInitializer ? 1 : 0));
      CHECK (i64 (find (s, "consumer", "entry")) == 42);
      CHECK (s.statistics ().compiled_functions > 0);
    }
}
TEST_CASE (
  "BBV session aliases exports recursive calls and function reference data use the same stable "
  "gate") {
  auto c = parse (
    "a:module\nimport fact\nslot:ref fact,0\nendmodule\nb:module\nexport fact\np:proto "
    "i64,i64:n\nfact:func i64,i64:n\nlocal i64:previous,i64:r\nble done,n,1\nsub "
    "previous,n,1\ncall p,fact,r,previous\nmul r,r,n\nret r\ndone:ret 1\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto s = session (c, level);
    auto a = find (s, "a", "fact"), b = find (s, "b", "fact");
    auto ae = a.native_entry (), be = b.native_entry ();
    REQUIRE ((ae).has_value ());
    REQUIRE ((be).has_value ());
    CHECK (ae.value ().address () == be.value ().address ());
    auto data = s.find_data ("a", "slot");
    REQUIRE ((data).has_value ());
    CHECK (read<std::uintptr_t> ((data).value ()) == ae.value ().address ());
    auto exported = s.find_export ("fact");
    REQUIRE ((exported).has_value ());
    CHECK (s.owns (std::get<FunctionHandle> ((exported).value ())));
    CHECK (s.statistics ().compiled_blocks == 0);
    std::array<std::int64_t, 1> args{6};
    CHECK (i64 (a, args) == 720);
    auto attempts = s.statistics ().block_compilation_attempts;
    {
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (be.value ().address ()) (5) == 120);
      REQUIRE ((invocation.status ()).has_value ());
    }
    CHECK (s.statistics ().block_compilation_attempts == attempts);
    CHECK (a.block_statistics ()->compiled_blocks == b.block_statistics ()->compiled_blocks);
  }
}
TEST_CASE ("BBV session source label data and jump returns stay demand driven") {
  auto c = parse (
    "m:module\np:proto\ncallee:func\nglobal i64:continuation:r19\njret "
    "continuation\nendfunc\nentry:func i64\nglobal i64:continuation:r19\nstart:laddr "
    "continuation,done\njcall p,callee\nret -1\ndone:ret 42\nendfunc\nslot:lref done,3\nbase:lref "
    "start,0\ntarget:lref done,0\ndiff:lref done,start,-4\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto s = session (c, level);
    auto entry = find (s, "m", "entry"), callee = find (s, "m", "callee");
    CHECK_FALSE ((callee.native_entry ()).has_value ());
    CHECK (s.statistics ().compiled_blocks == 0);
    auto slot = s.find_data ("m", "slot"), base = s.find_data ("m", "base"),
         target = s.find_data ("m", "target"), diff = s.find_data ("m", "diff");
    REQUIRE ((slot).has_value ());
    REQUIRE ((base).has_value ());
    REQUIRE ((target).has_value ());
    REQUIRE ((diff).has_value ());
    CHECK (read<std::uintptr_t> ((slot).value ()) == read<std::uintptr_t> ((target).value ()) + 3);
    CHECK (read<std::uintptr_t> ((diff).value ())
           == read<std::uintptr_t> ((target).value ()) - read<std::uintptr_t> ((base).value ())
                - 4);
    CHECK (i64 (entry) == 42);
    CHECK (s.statistics ().compiled_functions == 2);
    CHECK_FALSE ((entry.artifact ()).has_value ());
    CHECK_FALSE ((callee.artifact ()).has_value ());
  }
}
TEST_CASE (
  "BBV session failed entry and dynamic stack blocks recover every public call boundary and "
  "retry") {
  for (unsigned level = 0; level < 4; ++level)
    for (bool at_entry : {false, true}) {
      auto c = parse (branching);
      bool reject = true;
      auto s
        = session (c, level, {.block_versioning = {.observer = [&] (auto &event) -> Result<void> {
                                bool yes = false;
                                for (auto &l : event.version.block ().labels ())
                                  if (l.name () == "yes") yes = true;
                                if (reject && (at_entry ? event.version.call_entry () : yes))
                                  return Error{ErrorCode::InvalidArgument, "session cold failure"};
                                return {};
                              }}});
      auto f = find (s, "m", "f");
      auto entry = f.native_entry ();
      REQUIRE ((entry).has_value ());
      std::int64_t out = -1;
      auto check = [&] (auto result) {
        REQUIRE_FALSE (result.has_value ());
        CHECK (result.error ().message.find ("session cold failure") != std::string::npos);
        CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
      };
      std::array generic{ExecutionArgument (1), ExecutionArgument (ScalarValue::pointer (
                                                  reinterpret_cast<std::uintptr_t> (&out)))};
      check (f.call (generic));
      const auto out_address = std::uint64_t (reinterpret_cast<std::uintptr_t> (&out));
      std::array<std::int64_t, 2> scalar{1, std::bit_cast<std::int64_t> (out_address)};
      check (f.call_i64 (scalar));
      {
        ExecutionInvocation invocation;
        CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t, std::int64_t *)> (
                 entry.value ().address ()) (1, &out)
               == 0);
        check (invocation.status ());
      }
      CHECK (out == (at_entry ? -1 : 1));
      reject = false;
      CHECK (i64 (f, scalar) == 42);
      auto attempts = f.block_statistics ()->compilation_attempts;
      CHECK ((call (f, generic)[0].as_i64 ()).value () == 42);
      CHECK (f.block_statistics ()->compilation_attempts == attempts);
    }
}
namespace {
struct Reentry {
  std::uintptr_t entry;
  unsigned destroyed = 0;
  bool failed = false;
};
thread_local Reentry *reentry = nullptr;
std::int64_t callback (std::int64_t x) {
  struct Lifetime {
    ~Lifetime () { ++reentry->destroyed; }
  } lifetime;
  ExecutionInvocation invocation;
  auto result = reinterpret_cast<std::int64_t (*) (std::int64_t)> (reentry->entry) (x);
  reentry->failed = !(invocation.status ()).has_value ();
  return reentry->failed ? 7 : result;
}
}  // namespace
TEST_CASE (
  "BBV session nested native callback failures return locally before destroying host frames") {
  auto c = parse (
    "m:module\np:proto i64,i64:x\nimport callback\ninner:func i64,i64:x\nlocal p:stack\nalloca "
    "stack,4096\nbt done,x\nret 0\ndone:ret x\nendfunc\nouter:func i64,i64:x\nlocal i64:r\ncall "
    "p,callback,r,x\nret r\nendfunc\nendmodule\n");
  bool reject = true;
  auto s
    = session (c, 0,
               {.resolve = [] (auto &request) -> Result<ExternalSymbol> {
                  REQUIRE (request.name == "callback");
                  return ExternalSymbol{.address = reinterpret_cast<std::uintptr_t> (&callback),
                                        .signature = Signature{{Type::i64 ()}, {Type::i64 ()}}};
                },
                .block_versioning = {.observer = [&] (auto &event) -> Result<void> {
                  auto &f = event.version.block ().function ();
                  if (reject && f.name () == "inner" && !event.version.call_entry ())
                    return Error{ErrorCode::InvalidArgument, "nested session cold failure"};
                  return {};
                }}});
  auto inner = find (s, "m", "inner"), outer = find (s, "m", "outer");
  auto raw = inner.native_entry ();
  REQUIRE ((raw).has_value ());
  Reentry state{raw.value ().address ()};
  reentry = &state;
  std::array<std::int64_t, 1> args{1};
  CHECK (i64 (outer, args) == 7);
  CHECK (state.failed);
  CHECK (state.destroyed == 1);
  reject = false;
  CHECK (i64 (outer, args) == 1);
  CHECK_FALSE (state.failed);
  CHECK (state.destroyed == 2);
  CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
  reentry = nullptr;
}
TEST_CASE ("BBV session cold calls retain native hardware call preservation decisions") {
  auto c = parse (
    "m:module\np:proto i64\ncallee:func i64\nglobal i64:h:r5,d:v:v20\nmov h,20\ndmov v,22.0\nret "
    "0\nendfunc\nentry:func i64,i64:address\nglobal i64:h:r5,d:v:v20\nlocal i64:r\nmov h,0\ndmov "
    "v,0.0\ncall p,address,r\nd2i r,v\nadd r,r,h\nret r\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level)
    for (auto policy :
         {JITCompilation::Eager, JITCompilation::Lazy, JITCompilation::BlockVersioning}) {
      CAPTURE (level);
      CAPTURE (policy);
      auto loaded = ExecutionSession::
        create (c, {.optimization
                    = transforms::OptimizationOptions{.level
                                                      = static_cast<transforms::OptimizationLevel> (
                                                        level)},
                    .link = {.compilation = policy}});
      REQUIRE ((loaded).has_value ());
      auto &s = (loaded).value ();
      auto callee = find (s, "m", "callee").native_entry ();
      REQUIRE ((callee).has_value ());
      std::array<std::int64_t, 1> args{
        std::bit_cast<std::int64_t> (std::uint64_t (callee.value ().address ()))};
      // The O2/O3 reverse allocation rewrite preserves live bound
      // virtuals across calls. O0/O1 retain the physical callee's updated values.
      // Keep eager, lazy and block-versioned paths aligned.
      auto f = find (s, "m", "entry");
      CHECK (i64 (f, args) == (level < 2 ? 42 : 0));
      CHECK (i64 (f, args) == (level < 2 ? 42 : 0));
    }
}
TEST_CASE ("BBV session validates budgets preparation observers and expression failures at load") {
  auto c = parse ("m:module\nf:func i64\nret 42\nendfunc\nendmodule\n");
  auto no_budget
    = ExecutionSession::create (c, {.link = {.compilation = JITCompilation::BlockVersioning,
                                             .block_versioning = {.code_capacity = 0}}});
  CHECK_FALSE ((no_budget).has_value ());
  auto invalid
    = ExecutionSession::create (c, {.mode = ExecutionMode::Interpreter,
                                    .link = {.compilation = JITCompilation::BlockVersioning}});
  CHECK_FALSE ((invalid).has_value ());
  auto stage
    = ExecutionSession::create (c, {.link = {.compilation_observer = [] (auto &) -> Result<void> {
                                               return Error{ErrorCode::InvalidArgument,
                                                            "preparation refused"};
                                             },
                                             .compilation = JITCompilation::BlockVersioning}});
  REQUIRE_FALSE ((stage).has_value ());
  CHECK (stage.error ().message.find ("preparation refused") != std::string::npos);
  auto expression = test::variable_example (test::VariableExample::ExpressionInitializer);
  auto failed = ExecutionSession::create (*expression,
                                          {.link = {.compilation = JITCompilation::BlockVersioning,
                                                    .block_versioning
                                                    = {.observer = [] (auto &) -> Result<void> {
                                                        return Error{ErrorCode::InvalidArgument,
                                                                     "initializer refused"};
                                                      }}}});
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("initializer refused") != std::string::npos);
  CHECK (failed.error ().message.find ("initialize") != std::string::npos);
  CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
}
TEST_CASE (
  "BBV session exhausted arenas report checked diagnostics without changing engines or entries") {
  auto c = parse ("m:module\nf:func i64\nret 42\nendfunc\nendmodule\n");
  auto s = session (c, 0,
                    {.block_versioning
                     = {.code_capacity = native_code_memory_provider ()->page_size ()}});
  auto f = find (s, "m", "f");
  auto entry = f.native_entry ();
  REQUIRE ((entry).has_value ());
  auto bytes = s.statistics ().code_bytes;
  for (unsigned n = 1; n <= 2; ++n) {
    auto value = f.call_i64 ();
    REQUIRE_FALSE ((value).has_value ());
    CHECK (value.error ().message.find ("arena") != std::string::npos);
    CHECK (f.compilation_state () == FunctionCompilationState::Uncompiled);
    CHECK_FALSE ((f.artifact ()).has_value ());
    CHECK (f.block_statistics ()->compilation_attempts == n);
    CHECK (f.block_statistics ()->compiled_blocks == 0);
    CHECK (s.statistics ().code_bytes == bytes);
    CHECK (f.native_entry ().value ().address () == entry.value ().address ());
    CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
  }
}
TEST_CASE (
  "BBV session raw variadic entries retain unknown native arguments through first publication") {
  auto c = test::call_example (test::CallExample::Variadic);
  auto s = session (*c, 0);
  c.reset ();
  auto f = find (s, "calls", "sum");
  auto entry = f.native_entry ();
  REQUIRE ((entry).has_value ());
  auto address = entry.value ().address ();
  for (unsigned n = 0; n < 3; ++n) {
    ExecutionInvocation invocation;
    CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t, ...)> (address) (0, INT64_C (20),
                                                                             INT64_C (22))
           == 42);
    REQUIRE ((invocation.status ()).has_value ());
    CHECK (execution::detail::native_boundary_state_statistics ().depth == 0);
  }
  REQUIRE (f.block_statistics ());
  CHECK (f.block_statistics ()->compiled_blocks > 0);
  CHECK_FALSE ((f.artifact ()).has_value ());
}
TEST_CASE (
  "BBV session fixed seed loop inputs agree with interpreter eager and lazy JIT at O0-O3") {
  auto c = parse (
    "m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,0\nloop:ble done,x,0\nadd r,r,x\nsub "
    "x,x,1\njmp loop\ndone:ret r\nendfunc\nendmodule\n");
  std::uint64_t random = 0x7c8b42;
  for (unsigned level = 0; level < 4; ++level) {
    std::vector<ExecutionSession> engines;
    engines.push_back (session (c, level));
    for (auto policy : {JITCompilation::Eager, JITCompilation::Lazy}) {
      auto s = ExecutionSession::
        create (c, {.optimization
                    = transforms::OptimizationOptions{.level
                                                      = static_cast<transforms::OptimizationLevel> (
                                                        level)},
                    .link = {.compilation = policy}});
      REQUIRE ((s).has_value ());
      engines.push_back (std::move ((s).value ()));
    }
    auto interpreted = ExecutionSession::create (c, {.mode = ExecutionMode::Interpreter});
    REQUIRE ((interpreted).has_value ());
    engines.push_back (std::move ((interpreted).value ()));
    for (unsigned n = 0; n < 40; ++n) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      std::int64_t x = random % 65;
      std::array args{x};
      for (auto &engine : engines) CHECK (i64 (find (engine, "m", "f"), args) == x * (x + 1) / 2);
    }
  }
}
