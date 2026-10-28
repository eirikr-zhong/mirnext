/* Licensed under the MIT License. Owning session and actual native ABI boundary. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include <array>
#include <bit>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
bool native () { return test::native_execution_available (); }
ExecutionSession session (const Program &context, unsigned level = 2, LinkOptions link = {}) {
  auto s = ExecutionSession::
    create (context,
            {.optimization
             = transforms::OptimizationOptions{.level = static_cast<transforms::OptimizationLevel> (
                                                 level)},
             .link = std::move (link)});
  INFO (((s).has_value () ? "" : s.error ().message));
  REQUIRE ((s).has_value ());
  return std::move ((s).value ());
}
FunctionHandle find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE ((f).has_value ());
  return (f).value ();
}
std::unique_ptr<Program> parse (std::string_view text) {
  auto c = serialization::parse_text (test::native_hardware_text (text));
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
}  // namespace
TEST_CASE ("session entries cache independently from immutable IR and function compilation") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto c = test::variable_example (test::VariableExample::CrossModule);
    auto original = dump (*c);
    unsigned wrappers = 0;
    auto s = session (*c, level, {.compilation_observer = [&] (auto &view) -> Result<void> {
      wrappers += view.function.module_name () == "$host"
                  && view.stage == codegen::CompilationStage::Encode;
      return {};
    }});
    auto f = find (s, "consumer", "entry");
    auto before = dump (s.program ());
    CHECK (dump (*c) == original);
    c.reset ();
    CHECK (s.host_entry_count () == 0);
    CHECK (wrappers == 0);
    for (unsigned n = 0; n < 3; ++n) {
      auto v = s.call_i64 (f);
      INFO (((v).has_value () ? "" : v.error ().message));
      REQUIRE ((v).has_value ());
      CHECK ((v).value () == 42);
    }
    CHECK (wrappers == 1);
    CHECK (s.host_entry_count () == 1);
    CHECK (dump (s.program ()) == before);
    auto second = find (s, "definition", "get_value");
    auto v = s.call_i64 (second);
    REQUIRE ((v).has_value ());
    CHECK ((v).value () == 42);
    CHECK (wrappers == 2);
    CHECK (s.host_entry_count () == 2);
  }
}
TEST_CASE ("session typed wrappers handle exhausted integer and floating parameter registers") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned count : {0u, 1u, 8u, 9u, 17u})
      for (bool floating : {false, true}) {
        CAPTURE (level);
        CAPTURE (count);
        CAPTURE (floating);
        Program c;
        auto m = mirnext::ir::detail::access (c).create_module ("arity");
        REQUIRE ((m).has_value ());
        Type type (floating ? Type::Kind::F64 : Type::Kind::I64);
        auto f = mirnext::ir::detail::access (((m)).value ())
                   .create_function ("sum", {{type}, std::vector<Type> (count, type)});
        REQUIRE ((f).has_value ());
        auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
        REQUIRE ((b).has_value ());
        InstructionBuilder builder ((b).value ());
        auto sum = builder.literal (type, floating ? Operand (0.0) : Operand (0));
        for (auto argument :
             mirnext::ir::detail::ReferenceAccess::pointers (f.value ().arguments ()))
          sum = sum + builder.expression (*argument);
        REQUIRE ((builder.ret (sum)).has_value ());
        auto s = session (c, level);
        auto entry = find (s, "arity", "sum");
        if (floating) {
          std::vector<double> args (count, 2.5);
          auto v = s.call_double (entry, args);
          INFO (((v).has_value () ? "" : v.error ().message));
          REQUIRE ((v).has_value ());
          CHECK ((v).value () == count * 2.5);
        } else {
          std::vector<std::int64_t> args (count, 3);
          auto v = s.call_i64 (entry, args);
          INFO (((v).has_value () ? "" : v.error ().message));
          REQUIRE ((v).has_value ());
          CHECK ((v).value () == count * 3);
        }
        CHECK (s.host_entry_count () == 1);
      }
}
TEST_CASE ("session integer scalar slots preserve signed widths and unsigned bit patterns") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level) {
    auto c = parse (
      "m:module\na:func i8,i8:x\nret x\nendfunc\nb:func u16,u16:x\nret x\nendfunc\nu:func "
      "u64,u64:x\nret x\nendfunc\nd:func d,d:x\nret x\nendfunc\nv:func i64,i64:x,...\nret "
      "0\nendfunc\nj:func\nlocal i64:x\njret x\nendfunc\nendmodule\n");
    auto s = session (*c, level);
    auto a = find (s, "m", "a"), b = find (s, "m", "b"), u = find (s, "m", "u"),
         d = find (s, "m", "d");
    std::array<std::int64_t, 1> signed_arg{-3};
    auto av = s.call_i64 (a, signed_arg);
    REQUIRE ((av).has_value ());
    CHECK ((av).value () == -3);
    std::array<std::uint64_t, 1> unsigned_arg{0xfedcba987654ffff};
    auto bv = s.call_u64 (b, unsigned_arg);
    REQUIRE ((bv).has_value ());
    CHECK ((bv).value () == 65535);
    auto uv = s.call_u64 (u, unsigned_arg);
    REQUIRE ((uv).has_value ());
    CHECK ((uv).value () == unsigned_arg[0]);
    std::array<double, 1> doubles{std::bit_cast<double> (std::uint64_t{0x7ff8000000012345})};
    auto dv = s.call_double (d, doubles);
    REQUIRE ((dv).has_value ());
    CHECK (std::bit_cast<std::uint64_t> ((dv).value ()) == 0x7ff8000000012345);
    doubles[0] = -0.0;
    dv = s.call_double (d, doubles);
    REQUIRE ((dv).has_value ());
    CHECK (std::bit_cast<std::uint64_t> ((dv).value ()) == 0x8000000000000000);
    auto entries = s.host_entry_count ();
    CHECK_FALSE ((s.call_i64 (d)).has_value ());
    CHECK_FALSE ((s.call_double (u, doubles)).has_value ());
    CHECK_FALSE ((s.call_i64 (a)).has_value ());
    CHECK_FALSE ((s.call_i64 (find (s, "m", "v"))).has_value ());
    CHECK_FALSE ((s.call_i64 (find (s, "m", "j"))).has_value ());
    CHECK (s.host_entry_count () == entries);
    CHECK ((s.status ()).has_value ());
  }
}
TEST_CASE (
  "session checks ownership moves replacement and retained orphan handles before native entry") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto a = session (*c), b = session (*c);
  auto af = find (a, "consumer", "entry"), bf = find (b, "consumer", "entry");
  CHECK_FALSE ((b.call_i64 (af)).has_value ());
  CHECK (b.host_entry_count () == 0);
  CHECK (a.owns (af));
  CHECK_FALSE (a.owns (bf));
  b = std::move (a);
  CHECK_FALSE ((a.status ()).has_value ());
  CHECK_FALSE ((a.call_i64 (af)).has_value ());
  CHECK_FALSE ((a.find_function ("consumer", "entry")).has_value ());
  CHECK_FALSE ((a.find_data ("consumer", "value")).has_value ());
  CHECK_FALSE ((b.call_i64 (bf)).has_value ());
  auto v = b.call_i64 (af);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  std::optional<FunctionHandle> orphan;
  {
    auto s = session (*c);
    orphan = find (s, "consumer", "entry");
  }
  CHECK ((orphan->artifact ().value ().verify ()).has_value ());
  CHECK_FALSE ((b.call_i64 (*orphan)).has_value ());
  CHECK_FALSE ((b.find_function ("consumer", "missing")).has_value ());
  CHECK ((b.status ()).has_value ());
}
TEST_CASE ("session creation consumes owned optimization results and accepts isolated modules") {
  if (!native ()) return;
  for (bool optimized : {false, true}) {
    auto c = test::variable_example (test::VariableExample::Initialized);
    auto p = transforms::optimize (*c, {.level = transforms::OptimizationLevel::O3});
    REQUIRE ((p).has_value ());
    auto original = p.value ().dump ();
    auto s = optimized ? ExecutionSession::create (std::move ((p).value ()))
                       : ExecutionSession::create (*c->find_module ("consumer").get (),
                                                   {.optimization = transforms::OptimizationOptions{
                                                      .level = transforms::OptimizationLevel::O3}});
    REQUIRE ((s).has_value ());
    CHECK (dump (s.value ().program ()) == original);
    c.reset ();
    if (optimized) {
      CHECK_FALSE ((p.value ().verify ()).has_value ());
      CHECK_FALSE ((ExecutionSession::create (std::move ((p).value ()))).has_value ());
    }
    auto f = find ((s).value (), "consumer", "entry");
    auto v = s.value ().call_i64 (f);
    REQUIRE ((v).has_value ());
    CHECK ((v).value () == 42);
    v = s.value ().call_i64 (f);
    REQUIRE ((v).has_value ());
    CHECK ((v).value () == 64);
    CHECK (dump (s.value ().program ()) == original);
  }
}
TEST_CASE ("session does not poison cached state after wrapper observer failure") {
  if (!native ()) return;
  bool reject = true;
  unsigned attempts = 0;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto s = session (*c, 2, {.compilation_observer = [&] (auto &view) -> Result<void> {
    if (view.function.module_name () == "$host"
        && view.stage == codegen::CompilationStage::PrepareMachineIR) {
      ++attempts;
      if (reject) return Error{ErrorCode::UnknownName, "injected host entry failure"};
    }
    return {};
  }});
  auto f = find (s, "consumer", "entry");
  auto failed = s.call_i64 (f);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (failed.error ().message.find ("consumer/entry [session:host-entry]") != std::string::npos);
  CHECK (s.host_entry_count () == 0);
  CHECK ((s.status ()).has_value ());
  reject = false;
  auto v = s.call_i64 (f);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  CHECK (attempts == 2);
  v = s.call_i64 (f);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 64);
  CHECK (attempts == 2);
}
TEST_CASE ("session runtime ABI helpers support block and variadic calls at every level") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (auto mode : {test::CallExample::Direct, test::CallExample::Indirect,
                      test::CallExample::Block, test::CallExample::ReturnBlock,
                      test::CallExample::Variadic, test::CallExample::VariadicBlock,
                      test::CallExample::MultipleResults, test::CallExample::Inline}) {
      auto c = test::call_example (mode);
      if (test::check_native_result_limit (*c,
                                           {.optimization = transforms::OptimizationOptions{
                                              .level = static_cast<transforms::OptimizationLevel> (
                                                level)}}))
        continue;
      auto s = session (*c, level);
      c.reset ();
      auto v = s.call_i64 (find (s, "calls", "entry"));
      INFO (((v).has_value () ? "" : v.error ().message));
      REQUIRE ((v).has_value ());
      CHECK ((v).value () == 42);
    }
}
TEST_CASE (
  "session publication failure leaves globals untouched and retries with a fresh wrapper") {
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
      if (fail) return Error{ErrorCode::InvalidArgument, "injected wrapper publication failure"};
      return native->make_writable (m);
    }
    Result<void> make_executable (MappedCodeMemory m) override {
      return native->make_executable (m);
    }
    Result<void> flush (MappedCodeMemory m) override { return native->flush (m); }
  };
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto provider = std::make_shared<Provider> ();
  auto s = session (*c, 2, {.memory_provider = provider});
  auto f = find (s, "consumer", "entry");
  CHECK (provider->maps == 1);
  provider->fail = true;
  auto failed = s.call_i64 (f);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("publication failure") != std::string::npos);
  CHECK (s.host_entry_count () == 0);
  CHECK (provider->maps == 2);
  CHECK (provider->unmaps == 1);
  provider->fail = false;
  auto v = s.call_i64 (f);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  CHECK (s.host_entry_count () == 1);
  v = s.call_i64 (f);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 64);
  CHECK (provider->maps == 3);
}
TEST_CASE ("session aliases share cached host entries and builtin failures stay within one call") {
  if (!native ()) return;
  auto c = parse (
    "a:module\nimport ext\np:proto i64\nf:func i64\nlocal i64:r\ncall p,ext,r\nret "
    "r\nendfunc\nendmodule\nb:module\nimport ext\nendmodule\n");
  static bool fail = false;
  auto host = +[] () -> std::int64_t {
    if (fail) {
      auto builtin = builtin_address (codegen::Builtin::CopyBlock);
      REQUIRE ((builtin).has_value ());
      reinterpret_cast<void (*) (std::byte *, const std::byte *, std::uint64_t)> (
        (builtin).value ()) (nullptr, nullptr, 1);
    }
    return 42;
  };
  unsigned resolved = 0;
  auto s = session (*c, 2, {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
    ++resolved;
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (host),
                          Signature{{Type::i64 ()}, {}},
                          {}};
  }});
  auto a = find (s, "a", "ext"), b = find (s, "b", "ext");
  auto v = s.call_i64 (a);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  v = s.call_i64 (b);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  CHECK (s.host_entry_count () == 1);
  CHECK (resolved == 1);
  auto f = find (s, "a", "f");
  fail = true;
  auto failed = s.call_i64 (f);
  fail = false;
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("a/f [session:host-entry]: builtin:") != std::string::npos);
  v = s.call_i64 (f);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 42);
  CHECK (s.host_entry_count () == 2);
  CHECK ((s.status ()).has_value ());
}
TEST_CASE ("session entry caching retains distinct typed contracts at the same native address") {
  if (!native ()) return;
  auto source = parse ("identity:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  auto code = ExecutionSession::create (*source);
  REQUIRE ((code).has_value ());
  auto native = code.value ().find_function ("identity", "f");
  REQUIRE ((native).has_value ());
  // A generated identity accepts/returns the extended input unchanged, satisfying
  // either narrow contract without casting a C++ function to a different type.
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("aliases");
  REQUIRE ((m).has_value ());
  Type signed_byte (Type::Kind::I8), unsigned_byte (Type::Kind::U8);
  Signature small{{signed_byte}, {signed_byte}}, large{{unsigned_byte}, {unsigned_byte}};
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).import ("signed", small)).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).import ("unsigned", large)).has_value ());
  auto s = session (c, 2, {.resolve = [&] (auto &request) -> Result<ExternalSymbol> {
                      return ExternalSymbol{native.value ().address (),
                                            request.name == "signed" ? small : large,
                                            {}};
                    }});
  auto a = find (s, "aliases", "signed"), b = find (s, "aliases", "unsigned");
  std::array<std::int64_t, 1> args{255};
  auto v = s.call_i64 (a, args);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == -1);
  v = s.call_i64 (b, args);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 255);
  CHECK (s.host_entry_count () == 2);
  v = s.call_i64 (a, args);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == -1);
  v = s.call_i64 (b, args);
  REQUIRE ((v).has_value ());
  CHECK ((v).value () == 255);
  CHECK (s.host_entry_count () == 2);
}
#if defined(__aarch64__) || (defined(__x86_64__) && !defined(_MSC_VER))
namespace {
[[gnu::noinline]] Result<std::int64_t> host_preservation (ExecutionSession &session,
                                                          const FunctionHandle &function,
                                                          std::int64_t &after) {
#if defined(__aarch64__)
  asm volatile ("mov x19, %0" : : "r"(std::int64_t{77}) : "x19");
#else
  asm volatile ("movq %0, %%r12" : : "r"(std::int64_t{77}) : "r12");
#endif
  auto v = session.call_i64 (function);
#if defined(__aarch64__)
  asm volatile ("mov %0, x19" : "=r"(after));
  asm volatile ("" : : : "x19");
#else
  asm volatile ("movq %%r12, %0" : "=r"(after));
  asm volatile ("" : : : "r12");
#endif
  return v;
}
}  // namespace
TEST_CASE ("session host entries preserve host registers and internal MIR hardware side effects") {
  for (unsigned level = 0; level < 4; ++level) {
    auto c = parse (
      "m:module\np:proto i64:x,...\ncallee:func i64:x,...\nglobal i64:b:r19\nmov "
      "b,0\nret\nendfunc\nentry:func i64\nglobal i64:a:r19\nmov a,42\ncall p,callee,0\nret "
      "a\nendfunc\nendmodule\n");
    auto s = session (*c, level);
    auto f = find (s, "m", "entry");
    std::int64_t after = 0;
    auto value = host_preservation (s, f, after);
    INFO (((value).has_value () ? "" : value.error ().message));
    REQUIRE ((value).has_value ());
    CHECK ((value).value () == 0);
    CHECK (after == 77);
  }
}
#endif
