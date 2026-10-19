/* Licensed under the MIT License. Owning session and actual native ABI boundary. */
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
ExecutionSession session (const Context &context, unsigned level = 2, LinkOptions link = {}) {
  auto s = ExecutionSession::
    create (context,
            {.optimization
             = transforms::OptimizationOptions{.level = static_cast<transforms::OptimizationLevel> (
                                                 level)},
             .link = std::move (link)});
  INFO ((s ? "" : s.error ().message));
  REQUIRE (s);
  return std::move (*s);
}
FunctionHandle find (const ExecutionSession &s, std::string_view module, std::string_view name) {
  auto f = s.find_function (module, name);
  REQUIRE (f);
  return *f;
}
std::unique_ptr<Context> parse (std::string_view text) {
  auto c = serialization::parse_text (test::native_hardware_text (text));
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  return std::move (*c);
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
    auto before = dump (s.context ());
    CHECK (dump (*c) == original);
    c.reset ();
    CHECK (s.host_entry_count () == 0);
    CHECK (wrappers == 0);
    for (unsigned n = 0; n < 3; ++n) {
      auto v = s.call_i64 (f);
      INFO ((v ? "" : v.error ().message));
      REQUIRE (v);
      CHECK (*v == 42);
    }
    CHECK (wrappers == 1);
    CHECK (s.host_entry_count () == 1);
    CHECK (dump (s.context ()) == before);
    auto second = find (s, "definition", "get_value");
    auto v = s.call_i64 (second);
    REQUIRE (v);
    CHECK (*v == 42);
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
        Context c;
        auto m = c.create_module ("arity");
        REQUIRE (m);
        Type type (floating ? Type::Kind::F64 : Type::Kind::I64);
        auto f = (*m)->create_function ("sum", {{type}, std::vector<Type> (count, type)});
        REQUIRE (f);
        auto b = (*f)->create_block ();
        REQUIRE (b);
        IRBuilder builder (**b);
        auto sum = builder.literal (type, floating ? Operand (0.0) : Operand (0));
        for (auto argument : (*f)->arguments ()) sum = sum + builder.expression (*argument);
        REQUIRE (builder.ret (sum));
        auto s = session (c, level);
        auto entry = find (s, "arity", "sum");
        if (floating) {
          std::vector<double> args (count, 2.5);
          auto v = s.call_double (entry, args);
          INFO ((v ? "" : v.error ().message));
          REQUIRE (v);
          CHECK (*v == count * 2.5);
        } else {
          std::vector<std::int64_t> args (count, 3);
          auto v = s.call_i64 (entry, args);
          INFO ((v ? "" : v.error ().message));
          REQUIRE (v);
          CHECK (*v == count * 3);
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
    REQUIRE (av);
    CHECK (*av == -3);
    std::array<std::uint64_t, 1> unsigned_arg{0xfedcba987654ffff};
    auto bv = s.call_u64 (b, unsigned_arg);
    REQUIRE (bv);
    CHECK (*bv == 65535);
    auto uv = s.call_u64 (u, unsigned_arg);
    REQUIRE (uv);
    CHECK (*uv == unsigned_arg[0]);
    std::array<double, 1> doubles{std::bit_cast<double> (std::uint64_t{0x7ff8000000012345})};
    auto dv = s.call_double (d, doubles);
    REQUIRE (dv);
    CHECK (std::bit_cast<std::uint64_t> (*dv) == 0x7ff8000000012345);
    doubles[0] = -0.0;
    dv = s.call_double (d, doubles);
    REQUIRE (dv);
    CHECK (std::bit_cast<std::uint64_t> (*dv) == 0x8000000000000000);
    auto entries = s.host_entry_count ();
    CHECK_FALSE (s.call_i64 (d));
    CHECK_FALSE (s.call_double (u, doubles));
    CHECK_FALSE (s.call_i64 (a));
    CHECK_FALSE (s.call_i64 (find (s, "m", "v")));
    CHECK_FALSE (s.call_i64 (find (s, "m", "j")));
    CHECK (s.host_entry_count () == entries);
    CHECK (s.status ());
  }
}
TEST_CASE (
  "session checks ownership moves replacement and retained orphan handles before native entry") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto a = session (*c), b = session (*c);
  auto af = find (a, "consumer", "entry"), bf = find (b, "consumer", "entry");
  CHECK_FALSE (b.call_i64 (af));
  CHECK (b.host_entry_count () == 0);
  CHECK (a.owns (af));
  CHECK_FALSE (a.owns (bf));
  b = std::move (a);
  CHECK_FALSE (a.status ());
  CHECK_FALSE (a.call_i64 (af));
  CHECK_FALSE (a.find_function ("consumer", "entry"));
  CHECK_FALSE (a.find_data ("consumer", "value"));
  CHECK_FALSE (b.call_i64 (bf));
  auto v = b.call_i64 (af);
  REQUIRE (v);
  CHECK (*v == 42);
  std::optional<FunctionHandle> orphan;
  {
    auto s = session (*c);
    orphan = find (s, "consumer", "entry");
  }
  CHECK (orphan->artifact ()->verify ());
  CHECK_FALSE (b.call_i64 (*orphan));
  CHECK_FALSE (b.find_function ("consumer", "missing"));
  CHECK (b.status ());
}
TEST_CASE ("session creation consumes owned optimization results and accepts isolated modules") {
  if (!native ()) return;
  for (bool optimized : {false, true}) {
    auto c = test::variable_example (test::VariableExample::Initialized);
    auto p = transforms::optimize (*c, {.level = transforms::OptimizationLevel::O3});
    REQUIRE (p);
    auto original = p->dump ();
    auto s = optimized ? ExecutionSession::create (std::move (*p))
                       : ExecutionSession::create (*c->find_module ("consumer"),
                                                   {.optimization = transforms::OptimizationOptions{
                                                      .level = transforms::OptimizationLevel::O3}});
    REQUIRE (s);
    CHECK (dump (s->context ()) == original);
    c.reset ();
    if (optimized) {
      CHECK_FALSE (p->verify ());
      CHECK_FALSE (ExecutionSession::create (std::move (*p)));
    }
    auto f = find (*s, "consumer", "entry");
    auto v = s->call_i64 (f);
    REQUIRE (v);
    CHECK (*v == 42);
    v = s->call_i64 (f);
    REQUIRE (v);
    CHECK (*v == 64);
    CHECK (dump (s->context ()) == original);
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
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (failed.error ().message.find ("consumer/entry [session:host-entry]") != std::string::npos);
  CHECK (s.host_entry_count () == 0);
  CHECK (s.status ());
  reject = false;
  auto v = s.call_i64 (f);
  REQUIRE (v);
  CHECK (*v == 42);
  CHECK (attempts == 2);
  v = s.call_i64 (f);
  REQUIRE (v);
  CHECK (*v == 64);
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
      INFO ((v ? "" : v.error ().message));
      REQUIRE (v);
      CHECK (*v == 42);
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
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().message.find ("publication failure") != std::string::npos);
  CHECK (s.host_entry_count () == 0);
  CHECK (provider->maps == 2);
  CHECK (provider->unmaps == 1);
  provider->fail = false;
  auto v = s.call_i64 (f);
  REQUIRE (v);
  CHECK (*v == 42);
  CHECK (s.host_entry_count () == 1);
  v = s.call_i64 (f);
  REQUIRE (v);
  CHECK (*v == 64);
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
      REQUIRE (builtin);
      reinterpret_cast<void (*) (std::byte *, const std::byte *, std::uint64_t)> (
        *builtin) (nullptr, nullptr, 1);
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
  REQUIRE (v);
  CHECK (*v == 42);
  v = s.call_i64 (b);
  REQUIRE (v);
  CHECK (*v == 42);
  CHECK (s.host_entry_count () == 1);
  CHECK (resolved == 1);
  auto f = find (s, "a", "f");
  fail = true;
  auto failed = s.call_i64 (f);
  fail = false;
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().message.find ("a/f [session:host-entry]: builtin:") != std::string::npos);
  v = s.call_i64 (f);
  REQUIRE (v);
  CHECK (*v == 42);
  CHECK (s.host_entry_count () == 2);
  CHECK (s.status ());
}
TEST_CASE ("session entry caching retains distinct typed contracts at the same native address") {
  if (!native ()) return;
  auto source = parse ("identity:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  auto code = LinkedProgram::create (*source);
  REQUIRE (code);
  auto native = code->find_function ("identity", "f");
  REQUIRE (native);
  // A generated identity accepts/returns the extended input unchanged, satisfying
  // either narrow contract without casting a C++ function to a different type.
  Context c;
  auto m = c.create_module ("aliases");
  REQUIRE (m);
  Type signed_byte (Type::Kind::I8), unsigned_byte (Type::Kind::U8);
  Signature small{{signed_byte}, {signed_byte}}, large{{unsigned_byte}, {unsigned_byte}};
  REQUIRE ((*m)->import ("signed", small));
  REQUIRE ((*m)->import ("unsigned", large));
  auto s = session (c, 2, {.resolve = [&] (auto &request) -> Result<ExternalSymbol> {
                      return ExternalSymbol{native->address (),
                                            request.name == "signed" ? small : large,
                                            {}};
                    }});
  auto a = find (s, "aliases", "signed"), b = find (s, "aliases", "unsigned");
  std::array<std::int64_t, 1> args{255};
  auto v = s.call_i64 (a, args);
  REQUIRE (v);
  CHECK (*v == -1);
  v = s.call_i64 (b, args);
  REQUIRE (v);
  CHECK (*v == 255);
  CHECK (s.host_entry_count () == 2);
  v = s.call_i64 (a, args);
  REQUIRE (v);
  CHECK (*v == -1);
  v = s.call_i64 (b, args);
  REQUIRE (v);
  CHECK (*v == 255);
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
    INFO ((value ? "" : value.error ().message));
    REQUIRE (value);
    CHECK (*value == 0);
    CHECK (after == 77);
  }
}
#endif
