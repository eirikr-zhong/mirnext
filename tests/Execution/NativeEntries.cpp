/* Licensed under the MIT License. Real native ABI entries into owned C++ IR. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "../IR/Calls.hpp"
#include <array>
#include <bit>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
bool native () { return test::native_execution_available (); }
auto parse (std::string_view text) {
  auto c = serialization::parse_text (test::native_hardware_text (text));
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  return std::move (*c);
}
ExecutionSession session (const Context &c,
                          ExecutionOptions options
                          = {.mode = ExecutionMode::Interpreter, .native_entries = true}) {
  auto s = ExecutionSession::create (c, std::move (options));
  INFO ((s ? "" : s.error ().message));
  REQUIRE (s);
  return std::move (*s);
}
NativeEntry entry (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE (f);
  auto e = s.native_entry (*f);
  INFO ((e ? "" : e.error ().message));
  REQUIRE (e);
  return *e;
}
ExecutionSession import_entry (const NativeEntry &e) {
  Context c;
  auto m = c.create_module ("host");
  REQUIRE (m);
  REQUIRE ((*m)->import ("callee", e.signature ()));
  return session (c, {.mode = ExecutionMode::Interpreter,
                      .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                        return ExternalSymbol{e.address (), e.signature (), {}};
                      }}});
}
std::int64_t invoke_callback (std::int64_t (*callback) (std::int64_t), std::int64_t value) {
  return callback (value);
}
}  // namespace
TEST_CASE (
  "native interpreted entries own the program and expose identical function and data addresses") {
  if (!native ()) return;
  std::optional<NativeEntry> retained;
  {
    auto c = parse (
      "m:module\nadd:func i64,i64:x\nlocal i64:r\nadd r,x,1\nret r\nendfunc\nslot:ref "
      "add,0\nexpression:func i64\nret add\nendfunc\ninitialized:expr expression\nendmodule\n");
    auto original = dump (*c);
    auto s = session (*c);
    CHECK (dump (*c) == original);
    c.reset ();
    auto e = entry (s, "m", "add");
    CHECK (e.name () == "add");
    CHECK (e.module_name () == "m");
    CHECK (e.signature ().parameters.size () == 1);
    for (auto name : {"slot", "initialized"}) {
      auto data = s.find_data ("m", name);
      REQUIRE (data);
      std::uintptr_t address;
      std::memcpy (&address, data->bytes ().data (), 8);
      CHECK (address == e.address ());
    }
    CHECK (s.statistics ().code_batches == 1);
    CHECK (s.statistics ().code_bytes > 0);
    CHECK (s.host_entry_count () == 2);
    auto f = s.find_function ("m", "add");
    REQUIRE (f);
    auto moved = std::move (s);
    CHECK_FALSE (s.native_entry (*f));
    CHECK (moved.native_entry (*f)->address () == e.address ());
    retained = e;
  }
  ExecutionInvocation invocation;
  CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (retained->address ()) (41) == 42);
  CHECK (invocation.status ());
}
TEST_CASE (
  "native interpreted entries roundtrip every call fixture and optimization level through actual "
  "ABI code") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned n = 0; n <= unsigned (test::CallExample::Inline); ++n) {
      CAPTURE (level);
      CAPTURE (n);
      auto kind = static_cast<test::CallExample> (n);
      auto c = test::call_example (kind);
      if (test::check_native_result_limit (*c, {.mode = ExecutionMode::Interpreter,
                                                .native_entries = true}))
        continue;
      auto s
        = session (*c,
                   {.mode = ExecutionMode::Interpreter,
                    .optimization
                    = transforms::OptimizationOptions{.level
                                                      = static_cast<transforms::OptimizationLevel> (
                                                        level)},
                    .native_entries = true});
      c.reset ();
      auto e = entry (s, "calls", "sum");
      auto host = import_entry (e);
      auto f = host.find_function ("host", "callee");
      REQUIRE (f);
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
      auto result = host.call (*f, args);
      INFO ((result ? "" : result.error ().message));
      REQUIRE (result);
      if (kind == test::CallExample::ReturnBlock) {
        CHECK (result->empty ());
        CHECK (bytes[0] == 42);
      } else if (kind == test::CallExample::MultipleResults) {
        REQUIRE (result->size () == 2);
        CHECK (*(*result)[0].as_i64 () == 20);
        CHECK (*(*result)[1].as_i64 () == 22);
      } else {
        REQUIRE (result->size () == 1);
        CHECK (*(*result)[0].as_i64 () == 42);
      }
      CHECK (host.host_entry_count () == 1);
    }
}
TEST_CASE ("native entries retain mixed scalar bits and exhausted argument register banks") {
  if (!native ()) return;
  auto c = parse (
    "m:module\nf:func d,i8:x,u16:y,d:a,f:b,ld:c\nret a\nendfunc\ng:func f,f:a\nret "
    "a\nendfunc\nendmodule\n");
  auto s = session (*c);
  auto f = entry (s, "m", "f"), g = entry (s, "m", "g");
  auto bits = UINT64_C (0x7ff8000000012345);
  ExecutionInvocation invocation;
  auto value
    = reinterpret_cast<double (*) (std::int8_t, std::uint16_t, double, float, long double)> (
      f.address ()) (-3, 5, std::bit_cast<double> (bits), 2.5f, 3.0L);
  CHECK (std::bit_cast<std::uint64_t> (value) == bits);
  CHECK (std::bit_cast<std::uint32_t> (reinterpret_cast<float (*) (float)> (g.address ()) (-0.0f))
         == UINT32_C (0x80000000));
  CHECK (invocation.status ());
  for (bool floating : {false, true}) {
    Context context;
    auto m = context.create_module ("many");
    REQUIRE (m);
    auto type = floating ? Type (Type::Kind::F64) : Type::i64 ();
    auto fn = (*m)->create_function ("sum", {{type}, std::vector<Type> (17, type)});
    REQUIRE (fn);
    auto b = (*fn)->create_block ();
    REQUIRE (b);
    IRBuilder body (**b);
    auto sum = body.literal (type, floating ? Operand (0.0) : Operand (0));
    for (auto a : (*fn)->arguments ()) sum = sum + body.expression (*a);
    REQUIRE (body.ret (sum));
    auto program = session (context);
    auto exposed = entry (program, "many", "sum");
    auto caller = import_entry (exposed);
    auto target = caller.find_function ("host", "callee");
    REQUIRE (target);
    std::vector<ExecutionArgument> args (17, floating ? ExecutionArgument (2.5)
                                                      : ExecutionArgument (3));
    auto result = caller.call (*target, args);
    REQUIRE (result);
    if (floating)
      CHECK (*(*result)[0].as_double () == 42.5);
    else
      CHECK (*(*result)[0].as_i64 () == 51);
  }
}
TEST_CASE ("native entries consume real scalar and copied variadic cursors from a C++ caller") {
  if (!native ()) return;
  auto c = parse (
    "m:module\nf:func i64,i64:fixed,...\nlocal "
    "i64:list,i64:copy,i64:p,i64:a,i64:b,i64:c,d:v\nalloca list,64\nalloca copy,64\nmov "
    "i64:8(list),0\nmov i64:16(list),0\nmov i64:24(list),0\nva_start undef:(list)\nmov "
    "i64:0(copy),i64:0(list)\nmov i64:8(copy),i64:8(list)\nmov i64:16(copy),i64:16(list)\nmov "
    "i64:24(copy),i64:24(list)\nva_arg p,undef:(list),i64:0\nmov a,i64:0(p)\nva_arg "
    "p,undef:(list),d:0\ndmov v,d:0(p)\nd2i b,v\nva_arg p,undef:(copy),i64:0\nmov c,i64:0(p)\nadd "
    "a,a,b\nadd a,a,c\nva_end undef:(list)\nret a\nendfunc\nendmodule\n");
  auto s = session (*c);
  auto e = entry (s, "m", "f");
  ExecutionInvocation invocation;
  auto value = reinterpret_cast<std::int64_t (*) (std::int64_t, ...)> (
    e.address ()) (0, std::int64_t{10}, 22.0);
  CHECK (value == 42);
  CHECK (invocation.status ());
}
TEST_CASE ("native variadic activation retains bounds checks for nested synthetic varargs") {
  if (!native ()) return;
  for (bool complete : {false, true}) {
    auto c = parse (
      "m:module\nsignature:proto i64,i64:fixed,...\ng:func i64,i64:fixed,...\nlocal "
      "i64:list,i64:p,i64:a,i64:b\nalloca list,64\nva_start undef:(list)\nva_arg "
      "p,undef:(list),i64:0\nmov a,i64:0(p)\nva_arg p,undef:(list),i64:0\nmov b,i64:0(p)\nadd "
      "a,a,b\nret a\nendfunc\nf:func i64,i64:fixed,...\nlocal i64:list,i64:p,i64:n,i64:r\nalloca "
      "list,64\nva_start undef:(list)\nva_arg p,undef:(list),i64:0\nmov n,i64:0(p)\ncall "
      "signature,g,r,0,n"
      + std::string (complete ? ",0" : "") + "\nret r\nendfunc\nendmodule\n");
    auto s = session (*c);
    auto e = entry (s, "m", "f");
    for (unsigned n = 0; n < 3; ++n) {
      ExecutionInvocation invocation;
      auto value = reinterpret_cast<std::int64_t (*) (std::int64_t, ...)> (
        e.address ()) (0, std::int64_t{42});
      if (complete) {
        CHECK (value == 42);
        CHECK (invocation.status ());
      } else {
        CHECK (value == 0);
        auto status = invocation.status ();
        REQUIRE_FALSE (status);
        CHECK (status.error ().message.find ("variadic argument exceeds its invocation area")
               != std::string::npos);
      }
    }
  }
}
TEST_CASE (
  "native callback reentry shares instruction and call-depth limits and recovers after errors") {
  if (!native ()) return;
  auto c = parse (
    "m:module\nimport host\np:proto i64,p:callback,i64:n\nf:func i64,i64:n\nlocal i64:r\nbeq "
    "done,n,0\nsub n,n,1\ncall p,host,r,f,n\nadd r,r,1\nret r\ndone:ret 0\nendfunc\nendmodule\n");
  auto resolver = [] (auto &) -> Result<ExternalSymbol> {
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&invoke_callback),
                          Signature{{Type::i64 ()}, {Type::pointer (), Type::i64 ()}},
                          {}};
  };
  for (bool instructions : {false, true}) {
    auto s = session (*c, {.mode = ExecutionMode::Interpreter,
                           .link = {.resolve = resolver},
                           .instruction_limit = instructions ? 20u : 0u,
                           .call_depth_limit = instructions ? 0u : 4u,
                           .native_entries = true});
    auto f = s.find_function ("m", "f");
    REQUIRE (f);
    std::array<std::int64_t, 1> args{10};
    auto result = s.call_i64 (*f, args);
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find (instructions ? "instruction limit" : "depth limit")
           != std::string::npos);
    args[0] = 1;
    result = s.call_i64 (*f, args);
    INFO ((result ? "" : result.error ().message));
    REQUIRE (result);
    CHECK (*result == 1);
    auto e = s.native_entry (*f);
    REQUIRE (e);
    {
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (e->address ()) (10) == 0);
      CHECK_FALSE (invocation.status ());
    }
    {
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (e->address ()) (1) == 1);
      CHECK (invocation.status ());
    }
  }
}
TEST_CASE (
  "native entry publication is explicit and does not pass virtual function identities into native "
  "calls") {
  auto c = parse ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  auto raw = session (*c, {.mode = ExecutionMode::Interpreter});
  auto f = raw.find_function ("m", "f");
  REQUIRE (f);
  CHECK_FALSE (raw.native_entry (*f));
  auto other = session (*c, {.mode = ExecutionMode::Interpreter});
  CHECK_FALSE (other.native_entry (*f));
  CHECK_FALSE (ExecutionSession::create (*c, {.native_entries = true}));
  auto program = session (*c);
  auto pf = program.find_function ("m", "f");
  REQUIRE (pf);
  CHECK_FALSE (program.native_entry (*f));
  auto native = session (*c, {.mode = ExecutionMode::JIT});
  auto nf = native.find_function ("m", "f");
  REQUIRE (nf);
  auto ne = native.native_entry (*nf);
  REQUIRE (ne);
  CHECK (nf->artifact ());
  CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (ne->address ()) (42) == 42);
  auto calls = parse (
    "m:module\nimport host\np:proto i64,p:callback,i64:n\nf:func i64,i64:n\nret "
    "n\nendfunc\nentry:func i64\nlocal i64:r\ncall p,host,r,f,42\nret r\nendfunc\nendmodule\n");
  auto unsafe
    = session (*calls,
               {.mode = ExecutionMode::Interpreter,
                .link = {.resolve = [] (auto &) -> Result<ExternalSymbol> {
                  return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&invoke_callback),
                                        Signature{{Type::i64 ()}, {Type::pointer (), Type::i64 ()}},
                                        {}};
                }}});
  auto caller = unsafe.find_function ("m", "entry");
  REQUIRE (caller);
  auto result = unsafe.call (*caller);
  REQUIRE_FALSE (result);
  CHECK (result.error ().message.find ("requires native_entries at load time")
         != std::string::npos);
}
TEST_CASE (
  "native entry publication propagates observer failure before data initialization and source "
  "mutation") {
  if (!native ()) return;
  auto c = parse ("m:module\nf:func i64\nret 42\nendfunc\nendmodule\n");
  auto before = dump (*c);
  unsigned seen = 0;
  auto s = ExecutionSession::create (*c, {.mode = ExecutionMode::Interpreter,
                                          .link = {.compilation_observer
                                                   = [&] (auto &event) -> Result<void> {
                                            ++seen;
                                            CHECK (event.function.module_name ()
                                                   == "$interpreter-entry");
                                            return Error{ErrorCode::InvalidOperand,
                                                         "rejected entry compilation"};
                                          }},
                                          .native_entries = true});
  REQUIRE_FALSE (s);
  CHECK (seen == 1);
  CHECK (dump (*c) == before);
  CHECK (s.error ().message.find ("native-entry") != std::string::npos);
  CHECK (s.error ().message.find ("rejected entry compilation") != std::string::npos);
  auto retried = session (*c);
  auto e = entry (retried, "m", "f");
  CHECK (reinterpret_cast<std::int64_t (*) ()> (e.address ()) () == 42);
}
TEST_CASE ("native entry mode keeps internal JCALL and label references in the interpreter") {
  if (!native ()) return;
  auto c = parse (
    "m:module\np:proto\ncallee:func\nglobal i64:continuation:r19\njret "
    "continuation\nendfunc\nentry:func i64\nglobal i64:continuation:r19\nladdr "
    "continuation,done\njcall p,callee\nret -1\ndone:ret 42\nendfunc\nslot:lref "
    "done,0\nendmodule\n");
  auto s = session (*c);
  auto e = entry (s, "m", "entry");
  ExecutionInvocation invocation;
  CHECK (reinterpret_cast<std::int64_t (*) ()> (e.address ()) () == 42);
  CHECK (invocation.status ());
  auto jump = s.find_function ("m", "callee");
  REQUIRE (jump);
  CHECK_FALSE (s.native_entry (*jump));
}
TEST_CASE (
  "callback-only native entries share imported symbol identity and keep typed callbacks alive") {
  if (!native ()) return;
  auto c = parse (
    "a:module\nimport host\nslot:ref host,0\nendmodule\nb:module\nimport host\nslot:ref "
    "host,0\nendmodule\n");
  unsigned resolutions = 0;
  std::optional<NativeEntry> retained;
  {
    auto s
      = session (*c, {.mode = ExecutionMode::Interpreter,
                      .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                        ++resolutions;
                        ExternalSymbol symbol{0, Signature{{Type::i64 ()}, {Type::i64 ()}}, {}};
                        symbol.interpret = [calls = std::make_shared<unsigned> (0)] (
                                             auto args) -> Result<std::vector<ScalarValue>> {
                          auto value = args[0].scalar ()->as_i64 ();
                          if (!value) return value.error ();
                          ++*calls;
                          return std::vector<ScalarValue>{ScalarValue (*value + *calls)};
                        };
                        return symbol;
                      }},
                      .native_entries = true});
    auto a = entry (s, "a", "host"), b = entry (s, "b", "host");
    CHECK (a.address () == b.address ());
    CHECK (resolutions == 1);
    CHECK (s.host_entry_count () == 1);
    auto d = s.find_data ("b", "slot");
    REQUIRE (d);
    std::uintptr_t bits;
    std::memcpy (&bits, d->bytes ().data (), 8);
    CHECK (bits == a.address ());
    ExecutionInvocation invocation;
    CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (a.address ()) (40) == 41);
    CHECK (invocation.status ());
    retained = b;
  }
  ExecutionInvocation invocation;
  CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (retained->address ()) (40) == 42);
  CHECK (invocation.status ());
}
TEST_CASE (
  "fixed native entries preserve the interpreter callback priority over an optional native "
  "implementation") {
  if (!native ()) return;
  Context c;
  auto m = c.create_module ("m");
  REQUIRE (m);
  Signature signature{{Type::i64 ()}, {Type::i64 ()}};
  REQUIRE ((*m)->import ("host", signature));
  auto s
    = session (c,
               {.mode = ExecutionMode::Interpreter,
                .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                  auto native = +[] (std::int64_t) -> std::int64_t { return -1; };
                  ExternalSymbol symbol{reinterpret_cast<std::uintptr_t> (native), signature, {}};
                  symbol.interpret = [] (auto args) -> Result<std::vector<ScalarValue>> {
                    return std::vector<ScalarValue>{*args[0].scalar ()};
                  };
                  return symbol;
                }},
                .native_entries = true});
  auto f = s.find_function ("m", "host");
  REQUIRE (f);
  std::array<std::int64_t, 1> args{42};
  auto result = s.call_i64 (*f, args);
  REQUIRE (result);
  CHECK (*result == 42);
  auto e = s.native_entry (*f);
  REQUIRE (e);
  ExecutionInvocation invocation;
  CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (e->address ()) (42) == 42);
  CHECK (invocation.status ());
}
TEST_CASE ("native entries pass all block classes by value and write RBLK buffers") {
  if (!native ()) return;
  struct Pair {
    std::int64_t x, y;
  };
  struct FloatingPair {
    double x, y;
  };
  struct IntegerFloating {
    std::int64_t x;
    double y;
  };
  struct FloatingInteger {
    double x;
    std::int64_t y;
  };
  struct Large {
    std::int64_t x, y, z;
  };
  const bool sysv = target::TargetMachine::native ()->abi () == target::ABI::X86SysV;
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE (level);
    auto check = [&] (unsigned category, auto value) {
      CAPTURE (category);
      using Aggregate = decltype (value);
      using Words = std::array<std::int64_t, sizeof (Aggregate) / sizeof (std::int64_t)>;
      static_assert (sizeof (Words) == sizeof (Aggregate));
      const auto before = std::bit_cast<Words> (value);
      std::int64_t expected = 0;
      for (auto word : before) expected ^= word;
      auto type = category ? "blk" + std::to_string (category) : std::string ("blk");
      auto text = "m:module\nf:func i64," + type + ":" + std::to_string (sizeof (Aggregate))
                  + "(a)\nlocal i64:r\nmov r,i64:0(a)\n";
      for (std::size_t n = 1; n < before.size (); ++n)
        text += "xor r,r,i64:" + std::to_string (n * 8) + "(a)\n";
      auto c = parse (text + "mov i64:0(a),99\nret r\nendfunc\nendmodule\n");
      auto s
        = session (*c,
                   {.mode = ExecutionMode::Interpreter,
                    .optimization
                    = transforms::OptimizationOptions{.level
                                                      = static_cast<transforms::OptimizationLevel> (
                                                        level)},
                    .native_entries = true});
      auto e = entry (s, "m", "f");
      ExecutionInvocation invocation;
      CHECK (reinterpret_cast<std::int64_t (*) (Aggregate)> (e.address ()) (value) == expected);
      CHECK (std::bit_cast<Words> (value) == before);
      CHECK (invocation.status ());
    };
    // A 24-byte C++ aggregate is MEMORY on SysV. The four small classes
    // select INTEGER/INTEGER, SSE/SSE, INTEGER/SSE and SSE/INTEGER there.
    check (0, Large{42, 7, 13});
    check (1, Pair{42, 7});
    if (sysv) {
      check (2, FloatingPair{42.0, 7.0});
      check (3, IntegerFloating{42, 7.0});
      check (4, FloatingInteger{42.0, 7});
    } else {
      // AArch64 and Windows ignore the SysV class hints. In particular an
      // AArch64 homogeneous floating aggregate needs a different convention.
      for (unsigned category = 2; category <= 4; ++category) check (category, Pair{42, 7});
    }
  }
  // AArch64 RBLK follows the host compiler's x8 convention. On x86 MIR passes
  // an explicit output pointer but does not promise the C++ sret pointer in AX.
  auto source = parse (
    "m:module\nf:func rblk:24(buffer),i64:n\nmov i64:0(buffer),n\nadd n,n,1\nmov "
    "i64:8(buffer),n\nadd n,n,1\nmov i64:16(buffer),n\nret\nendfunc\nendmodule\n");
  auto large = session (*source);
  auto le = entry (large, "m", "f");
  ExecutionInvocation invocation;
  Large result{};
  if (target::TargetMachine::native ()->architecture () == target::Architecture::X86_64)
    reinterpret_cast<void (*) (Large *, std::int64_t)> (le.address ()) (&result, 20);
  else
    result = reinterpret_cast<Large (*) (std::int64_t)> (le.address ()) (20);
  CHECK (result.x == 20);
  CHECK (result.y == 21);
  CHECK (result.z == 22);
  CHECK (invocation.status ());
}
TEST_CASE ("native callback diagnostics are scoped first errors and do not poison later entries") {
  if (!native ()) return;
  auto c
    = parse ("m:module\nf:func i64,i64:x\nlocal i64:r\ndiv r,42,x\nret r\nendfunc\nendmodule\n");
  auto s = session (*c);
  auto e = entry (s, "m", "f");
  auto call = reinterpret_cast<std::int64_t (*) (std::int64_t)> (e.address ());
  {
    ExecutionInvocation outer;
    CHECK (call (0) == 0);
    auto original = outer.status ();
    REQUIRE_FALSE (original);
    CHECK (original.error ().message.find ("m/f") != std::string::npos);
    CHECK (original.error ().message.find ("division by zero") != std::string::npos);
    {
      ExecutionInvocation inner;
      CHECK (call (1) == 42);
      CHECK (inner.status ());
    }
    CHECK (call (1) == 42);
    CHECK (outer.status ().error ().message == original.error ().message);
  }
  ExecutionInvocation retry;
  CHECK (call (1) == 42);
  CHECK (retry.status ());
}
TEST_CASE (
  "native entry publication reports unsupported callback metadata and allocation errors without "
  "fallback") {
  if (!native ()) return;
  Context c;
  auto m = c.create_module ("m");
  REQUIRE (m);
  Signature signature{{Type::i64 ()}, {Type::i64 ()}, true};
  REQUIRE ((*m)->import ("host", signature));
  auto unsupported
    = ExecutionSession::create (c, {.mode = ExecutionMode::Interpreter,
                                    .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                                      ExternalSymbol symbol{0, signature, {}};
                                      symbol.interpret
                                        = [] (auto) -> Result<std::vector<ScalarValue>> {
                                        return std::vector<ScalarValue>{ScalarValue (42)};
                                      };
                                      return symbol;
                                    }},
                                    .native_entries = true});
  REQUIRE_FALSE (unsupported);
  CHECK (unsupported.error ().message.find ("variadic callback-only import") != std::string::npos);
  struct Reject final : CodeMemoryProvider {
    unsigned allocations = 0;
    std::size_t page_size () const override { return 4096; }
    Result<MappedCodeMemory> allocate (std::size_t) override {
      ++allocations;
      return Error{ErrorCode::InvalidOperand, "rejected native entry allocation"};
    }
    Result<void> release (MappedCodeMemory) override { return {}; }
    Result<void> make_writable (MappedCodeMemory) override { return {}; }
    Result<void> make_executable (MappedCodeMemory) override { return {}; }
    Result<void> flush (MappedCodeMemory) override { return {}; }
  };
  auto source = parse ("m:module\nf:func i64\nret 42\nendfunc\nendmodule\n");
  auto provider = std::make_shared<Reject> ();
  auto failed = ExecutionSession::create (*source, {.mode = ExecutionMode::Interpreter,
                                                    .link = {.memory_provider = provider},
                                                    .native_entries = true});
  REQUIRE_FALSE (failed);
  CHECK (provider->allocations == 1);
  CHECK (failed.error ().message.find ("rejected native entry allocation") != std::string::npos);
  auto plain = ExecutionSession::create (*source, {.mode = ExecutionMode::Interpreter,
                                                   .link = {.memory_provider = provider}});
  REQUIRE (plain);
  CHECK (provider->allocations == 1);
}
