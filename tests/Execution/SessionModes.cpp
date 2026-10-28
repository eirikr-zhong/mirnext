/* Licensed under the MIT License. Unified engines and typed native ABI calls. */
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
#include <cstdarg>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
bool native () { return test::native_execution_available (); }
auto parse (std::string_view text) {
  auto c = serialization::parse_text (test::native_hardware_text (text));
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
ExecutionSession make (const Program &c, ExecutionOptions options = {}) {
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
auto call (const ExecutionSession &s, const FunctionHandle &f,
           std::span<const ExecutionArgument> args = {}) {
  auto r = s.call (f, args);
  INFO (((r).has_value () ? "" : r.error ().message));
  REQUIRE ((r).has_value ());
  return std::move ((r).value ());
}
}  // namespace
TEST_CASE (
  "unified session generic calls support mixed scalars multiple results and exact floating bits") {
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter}) {
    if (mode == ExecutionMode::JIT && !native ()) continue;
    auto c = parse (
      "m:module\nf:func i64,d,f,ld,i8:x,u16:y,d:a,f:b,ld:c\nlocal i64:r\nadd r,x,y\nret "
      "r,a,b,c\nendfunc\nendmodule\n");
    if (test::check_native_result_limit (*c, {.mode = mode})) continue;
    auto before = dump (*c);
    auto s = make (*c, {.mode = mode});
    CHECK (s.mode () == mode);
    CHECK (dump (*c) == before);
    c.reset ();
    auto f = find (s, "m", "f");
    CHECK (f.mode () == mode);
    CHECK (bool ((f.artifact ()).has_value ()) == (mode == ExecutionMode::JIT));
    std::array args{ExecutionArgument (255), ExecutionArgument (UINT64_C (0xffff)),
                    ExecutionArgument (std::bit_cast<double> (UINT64_C (0x7ff8000000000042))),
                    ExecutionArgument (-0.0f), ExecutionArgument (2.5L)};
    for (unsigned n = 0; n < 3; ++n) {
      auto result = call (s, f, args);
      REQUIRE (result.size () == 4);
      CHECK ((result[0].as_i64 ()).value () == 65534);
      CHECK (std::bit_cast<std::uint64_t> ((result[1].as_double ()).value ())
             == UINT64_C (0x7ff8000000000042));
      CHECK (std::bit_cast<std::uint32_t> ((result[2].as_float ()).value ())
             == UINT32_C (0x80000000));
      CHECK ((result[3].as_long_double ()).value () == 2.5L);
    }
    CHECK (s.host_entry_count () == (mode == ExecutionMode::JIT ? 1 : 0));
    CHECK (s.statistics ().functions == 1);
  }
}
TEST_CASE (
  "unified session calls every block variadic and multiple-return fixture directly at O0-O3") {
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
        auto s = make (*c, {.mode = mode,
                            .optimization = transforms::OptimizationOptions{
                              .level = static_cast<transforms::OptimizationLevel> (level)}});
        c.reset ();
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
        auto result = call (s, find (s, "calls", "sum"), args);
        if (kind == test::CallExample::ReturnBlock) {
          CHECK (result.empty ());
          CHECK (bytes[0] == 42);
        } else if (kind == test::CallExample::MultipleResults) {
          REQUIRE (result.size () == 2);
          CHECK ((result[0].as_i64 ()).value () == 20);
          CHECK ((result[1].as_i64 ()).value () == 22);
        } else {
          REQUIRE (result.size () == 1);
          CHECK ((result[0].as_i64 ()).value () == 42);
        }
      }
}
TEST_CASE ("unified session modes preserve source ownership globals and stable handles") {
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto raw = make (*c, {.mode = ExecutionMode::Interpreter});
  CHECK ((raw.interpreter_statistics ()).has_value ());
  CHECK (raw.statistics ().code_bytes == 0);
  CHECK (raw.host_entry_count () == 0);
  auto f = find (raw, "consumer", "entry");
  auto data = raw.find_data ("consumer", "value");
  REQUIRE ((data).has_value ());
  c.reset ();
  auto other = make (raw.program (), {.mode = ExecutionMode::Interpreter});
  CHECK_FALSE (other.owns (f));
  CHECK_FALSE ((other.call (f)).has_value ());
  auto moved = std::move (raw);
  CHECK_FALSE ((raw.status ()).has_value ());
  CHECK_FALSE (raw.owns (f));
  CHECK_FALSE ((raw.call (f)).has_value ());
  CHECK (moved.owns (f));
  CHECK ((call (moved, f)[0].as_i64 ()).value () == 42);
  CHECK ((call (moved, f)[0].as_i64 ()).value () == 64);
  auto consumed = transforms::optimize (moved.program ());
  REQUIRE ((consumed).has_value ());
  auto loaded = ExecutionSession::create (std::move ((consumed).value ()),
                                          {.mode = ExecutionMode::Interpreter});
  REQUIRE ((loaded).has_value ());
  CHECK_FALSE ((consumed.value ().verify ()).has_value ());
  CHECK_FALSE (loaded.value ().owns (f));
  CHECK_FALSE ((loaded.value ().call (f)).has_value ());
  CHECK (
    (call ((loaded).value (), find ((loaded).value (), "consumer", "entry"))[0].as_i64 ()).value ()
    == 42);
  auto module = loaded.value ().program ().find_module ("consumer").get ();
  REQUIRE (module);
  auto isolated = ExecutionSession::create (*module, {.mode = ExecutionMode::Interpreter});
  REQUIRE ((isolated).has_value ());
  if (native ()) {
    auto jit = make (moved.program ());
    CHECK_FALSE (jit.owns (f));
    CHECK_FALSE ((jit.call (f)).has_value ());
    CHECK_FALSE ((moved.call (find (jit, "consumer", "entry"))).has_value ());
  }
}
TEST_CASE (
  "unified calls copy every block class and reject malformed values before creating native "
  "entries") {
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter})
    for (unsigned category = 0; category <= 4; ++category) {
      if (mode == ExecutionMode::JIT && !native ()) continue;
      CAPTURE (mode);
      CAPTURE (category);
      auto type = category ? "blk" + std::to_string (category) : std::string ("blk");
      auto c = parse (
        "m:module\nf:func i64," + type
        + ":16(a)\nlocal i64:r\nmov r,i64:0(a)\nmov i64:0(a),99\nret r\nendfunc\nendmodule\n");
      auto s = make (*c, {.mode = mode});
      auto f = find (s, "m", "f");
      std::array<std::int64_t, 2> bytes{42, 7};
      auto data = std::as_bytes (std::span (bytes));
      auto abi = static_cast<Type::BlockClass> (category),
           wrong = static_cast<Type::BlockClass> ((category + 1) % 5);
      for (auto argument :
           {ExecutionArgument::block (data.first (8), abi), ExecutionArgument::block (data, wrong),
            ExecutionArgument (42),
            ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (bytes)))}) {
        std::array args{argument};
        CHECK_FALSE ((s.call (f, args)).has_value ());
        CHECK (s.host_entry_count () == 0);
        CHECK (bytes[0] == 42);
      }
      std::array args{ExecutionArgument::block (data, abi)};
      CHECK ((call (s, f, args)[0].as_i64 ()).value () == 42);
      CHECK (bytes[0] == 42);
      CHECK (s.host_entry_count () == (mode == ExecutionMode::JIT ? 1 : 0));
    }
}
TEST_CASE (
  "unified session options distinguish raw interpretation explicit optimization and consumed "
  "products") {
  auto c = parse ("m:module\nf:func i64\nlocal i64:r\nadd r,20,22\nret r\nendfunc\nendmodule\n");
  auto original = dump (*c);
  auto raw = make (*c, {.mode = ExecutionMode::Interpreter});
  CHECK (dump (raw.program ()) == original);
  unsigned observed = 0;
  auto optimized = make (*c, {.mode = ExecutionMode::Interpreter,
                              .optimization = transforms::OptimizationOptions{
                                .observer = [&] (auto &) -> Result<void> {
                                  ++observed;
                                  return {};
                                }}});
  CHECK (observed > 0);
  CHECK (dump (*c) == original);
  CHECK ((call (optimized, find (optimized, "m", "f"))[0].as_i64 ()).value () == 42);
  CHECK_FALSE (
    (ExecutionSession::create (*c, {.mode = static_cast<ExecutionMode> (99)})).has_value ());
  CHECK_FALSE ((ExecutionSession::create (*c, {.instruction_limit = 1})).has_value ());
  auto p = transforms::optimize (*c);
  REQUIRE ((p).has_value ());
  CHECK_FALSE ((ExecutionSession::create (std::move ((p).value ()),
                                          {.optimization = transforms::OptimizationOptions{}}))
                 .has_value ());
  auto loop = parse ("m:module\nf:func i64\nloop:jmp loop\nret 0\nendfunc\nendmodule\n");
  auto bounded = make (*loop, {.mode = ExecutionMode::Interpreter, .instruction_limit = 12});
  auto result = bounded.call (find (bounded, "m", "f"));
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("instruction limit") != std::string::npos);
}
TEST_CASE ("native generic adapters validate before execution and cache unnamed argument types") {
  if (!native ()) return;
  auto c = parse ("m:module\nv:func i64,i64:x,...\nret x\nendfunc\nendmodule\n");
  auto s = make (*c);
  auto f = find (s, "m", "v");
  std::array integer{ExecutionArgument (42), ExecutionArgument (20)},
    floating{ExecutionArgument (42), ExecutionArgument (2.5)};
  CHECK ((call (s, f, integer)[0].as_i64 ()).value () == 42);
  CHECK ((call (s, f, floating)[0].as_i64 ()).value () == 42);
  CHECK (s.host_entry_count () == 2);
  CHECK ((call (s, f, integer)[0].as_i64 ()).value () == 42);
  CHECK (s.host_entry_count () == 2);
  std::array bad{ExecutionArgument (42), ExecutionArgument (1.0f)};
  CHECK_FALSE ((s.call (f, bad)).has_value ());
  CHECK_FALSE ((s.call (f)).has_value ());
  CHECK_FALSE ((s.call_i64 (f)).has_value ());
  CHECK (s.host_entry_count () == 2);
}
TEST_CASE ("native generic calls preserve arguments and results across inline frame boundaries") {
  if (!native ()) return;
  for (unsigned count : {0u, 13u, 14u, 15u, 16u, 17u, 32u})
    for (unsigned results : {0u, 1u, 2u}) {
      CAPTURE (count);
      CAPTURE (results);
      Program c;
      auto m = mirnext::ir::detail::access (c).create_module ("m");
      REQUIRE ((m).has_value ());
      auto f = mirnext::ir::detail::access (((m)).value ())
                 .create_function ("f", {std::vector<Type> (results, Type::i64 ()),
                                         std::vector<Type> (count, Type::i64 ())});
      REQUIRE ((f).has_value ());
      auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
      REQUIRE ((block).has_value ());
      InstructionBuilder b ((block).value ());
      std::vector<Operand> returned;
      for (unsigned n = 0; n < results; ++n) {
        if (count)
          returned.emplace_back (*mirnext::ir::detail::ReferenceAccess::pointers (
            f.value ().arguments ())[n ? 0 : count - 1]);
        else
          returned.emplace_back (std::int64_t (40 + n));
      }
      REQUIRE ((b.ret (returned)).has_value ());
      if (test::check_native_result_limit (c)) continue;
      auto s = make (c);
      auto handle = find (s, "m", "f");
      for (unsigned trial = 0; trial < 3; ++trial) {
        std::vector<ExecutionArgument> args;
        for (unsigned n = 0; n < count; ++n) args.emplace_back (n + 1 + trial);
        auto values = call (s, handle, args);
        REQUIRE (values.size () == results);
        for (unsigned n = 0; n < results; ++n)
          CHECK ((values[n].as_i64 ()).value () == (count ? (n ? 1 : count) + trial : 40 + n));
        CHECK (s.host_entry_count () == 1);
      }
    }
}
TEST_CASE ("native generic adapter failures leave globals unchanged and can retry") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  bool fail = true;
  auto s = make (*c, {.link = {.compilation_observer = [&] (auto &event) -> Result<void> {
                        if (fail && event.function.module_name () == "$native-call")
                          return Error{ErrorCode::InvalidArgument, "rejected native adapter"};
                        return {};
                      }}});
  auto f = find (s, "consumer", "entry");
  auto value = s.call (f);
  REQUIRE_FALSE ((value).has_value ());
  CHECK (s.host_entry_count () == 0);
  auto data = s.find_data ("consumer", "value");
  REQUIRE ((data).has_value ());
  std::int64_t bits;
  std::memcpy (&bits, data.value ().bytes ().data (), 8);
  CHECK (bits == 20);
  fail = false;
  CHECK ((call (s, f)[0].as_i64 ()).value () == 42);
  CHECK ((call (s, f)[0].as_i64 ()).value () == 64);
  CHECK (s.host_entry_count () == 1);
}
namespace {
struct BorrowedImportMetadata {
  std::string_view module, name;
  std::string &released;
  BorrowedImportMetadata (std::string_view m, std::string_view n, std::string &r)
    : module (m), name (n), released (r) {}
  ~BorrowedImportMetadata () { released = std::string (module) + '/' + std::string (name); }
};
std::int64_t lifetime_host () { return 42; }
double foreign_mixed (std::int8_t x, std::uint16_t y, double a, float b, long double c) {
  return x + y + a + b + c;
}
struct Pair {
  std::int64_t x, y;
};
// A two-integer C++ aggregate uses the SysV INTEGER/INTEGER class. Default
// BLK is MEMORY there; AArch64 and Windows accept this explicit class too.
constexpr auto pair_class = Type::BlockClass::Class1;
std::int64_t foreign_pair (Pair p) { return p.x + p.y; }
std::int64_t foreign_pair_alias (Pair p, std::int64_t *original) {
  *original = 99;
  return p.x;
}
struct Large {
  std::int64_t x, y, z;
};
Large foreign_large (std::int64_t n) { return {n, n + 1, n + 2}; }
double foreign_variadic (std::int64_t n, ...) {
  va_list list;
  va_start (list, n);
  auto i = va_arg (list, std::int64_t);
  auto d = va_arg (list, double);
  va_end (list);
  return n + i + d;
}
}  // namespace
TEST_CASE (
  "loaded IR outlives callback metadata through the last function data or native entry handle") {
  for (unsigned path = 0; path < 4; ++path) {
    if (path >= 2 && !native ()) continue;
    CAPTURE (path);
    std::string released;
    std::optional<FunctionHandle> retained;
    std::optional<mirnext::execution::DataHandle> data;
    std::optional<NativeEntry> entry;
    {
      auto c = parse (
        "long_lived_module_metadata:module\nimport long_lived_import_metadata\np:proto i64\nf:func "
        "i64\nlocal i64:r\ncall p,long_lived_import_metadata,r\nret r\nendfunc\nvalue:i64 "
        "20\nendmodule\n");
      ExecutionOptions
        options{.mode = path == 2 ? ExecutionMode::JIT : ExecutionMode::Interpreter,
                .link = {.resolve = [&] (const ImportRequest &request) -> Result<ExternalSymbol> {
                  ExternalSymbol symbol{reinterpret_cast<std::uintptr_t> (&lifetime_host),
                                        Signature{{Type::i64 ()}, {}},
                                        {}};
                  symbol.interpret =
                    [metadata = std::make_shared<BorrowedImportMetadata> (request.module,
                                                                          request.name, released)] (
                      auto) -> Result<std::vector<ScalarValue>> {
                    CHECK (metadata->name == "long_lived_import_metadata");
                    return std::vector<ScalarValue>{ScalarValue (42)};
                  };
                  return symbol;
                }},
                .native_entries = path == 3};
      if (path == 1) options.optimization = transforms::OptimizationOptions{};
      auto s = make (*c, std::move (options));
      c.reset ();
      retained = find (s, "long_lived_module_metadata", "f");
      data = (s.find_data ("long_lived_module_metadata", "value")).value ();
      if (path >= 2) {
        auto e = s.native_entry (*retained);
        REQUIRE ((e).has_value ());
        entry = std::move ((e).value ());
      }
      CHECK ((call (s, *retained)[0].as_i64 ()).value () == 42);
      CHECK (released.empty ());
    }
    CHECK (retained->name () == "f");
    CHECK (released.empty ());
    retained.reset ();
    if (entry) {
      CHECK (reinterpret_cast<std::int64_t (*) ()> (entry->address ()) () == 42);
      entry.reset ();
    }
    CHECK (released.empty ());
    CHECK (data->bytes ().size () == 8);
    data.reset ();
    CHECK (released == "long_lived_module_metadata/long_lived_import_metadata");
  }
}
TEST_CASE (
  "typed callbacks receive independent by-value blocks when another argument aliases the source") {
  for (unsigned path = 0; path < 3; ++path) {
    if (path && !native ()) continue;
    CAPTURE (path);
    Program c;
    auto m = mirnext::ir::detail::access (c).create_module ("m");
    REQUIRE ((m).has_value ());
    Signature signature{{Type::i64 ()},
                        {Type::block (sizeof (Pair), pair_class), Type::pointer ()}};
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ()).import ("host", signature)).has_value ());
    auto s
      = make (c, {.mode = path == 2 ? ExecutionMode::JIT : ExecutionMode::Interpreter,
                  .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                    ExternalSymbol symbol{path
                                            ? reinterpret_cast<std::uintptr_t> (&foreign_pair_alias)
                                            : 0,
                                          signature,
                                          {}};
                    if (!path)
                      symbol.interpret = [] (auto args) -> Result<std::vector<ScalarValue>> {
                        auto address = args[1].scalar ().value ().as_u64 ();
                        if (!address.has_value ()) return address.error ();
                        *reinterpret_cast<std::int64_t *> (address.value ()) = 99;
                        std::int64_t value;
                        std::memcpy (&value, args[0].block ().value ().bytes.data (), 8);
                        return std::vector<ScalarValue>{ScalarValue (value)};
                      };
                    return symbol;
                  }}});
    auto f = find (s, "m", "host");
    for (unsigned n = 0; n < 3; ++n) {
      Pair pair{42, 7};
      std::array args{ExecutionArgument::block (std::as_bytes (std::span (&pair, 1)), pair_class),
                      ExecutionArgument (
                        ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (&pair.x)))};
      CHECK ((call (s, f, args)[0].as_i64 ()).value () == 42);
      CHECK (pair.x == 99);
      CHECK (s.host_entry_count () == (path ? 1 : 0));
    }
  }
}
TEST_CASE ("typed callbacks copy unnamed blocks of every ABI class while sharing RBLK storage") {
  for (unsigned category = 0; category <= 4; ++category) {
    auto abi = static_cast<Type::BlockClass> (category);
    Program c;
    auto m = mirnext::ir::detail::access (c).create_module ("m");
    REQUIRE ((m).has_value ());
    Signature signature{{}, {Type::return_block (16)}, true};
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ()).import ("host", signature)).has_value ());
    auto s = make (c, {.mode = ExecutionMode::Interpreter,
                       .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                         ExternalSymbol host{0, signature, {}};
                         host.interpret = [abi] (auto args) -> Result<std::vector<ScalarValue>> {
                           CHECK (args[1].type ().block_class () == abi);
                           std::int64_t changed = 99, value;
                           auto output = args[0].block ().value ().writable;
                           std::memcpy (output.data (), &changed, 8);
                           std::memcpy (&value, args[1].block ().value ().bytes.data (), 8);
                           std::memcpy (output.data () + 8, &value, 8);
                           return std::vector<ScalarValue>{};
                         };
                         return host;
                       }}});
    std::array<std::int64_t, 2> bytes{42, 7};
    std::array args{ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (bytes))),
                    ExecutionArgument::block (std::as_bytes (std::span (bytes)), abi)};
    CHECK (call (s, find (s, "m", "host"), args).empty ());
    CHECK (bytes[0] == 99);
    CHECK (bytes[1] == 42);
    CHECK (s.host_entry_count () == 0);
  }
}
TEST_CASE (
  "owning interpreter native imports use C++ ABI adapters for mixed values blocks RBLK and "
  "varargs") {
  if (!native ()) return;
  struct Binding {
    std::string name;
    std::uintptr_t address;
    Signature signature;
  };
  std::array bindings{Binding{"mixed",
                              reinterpret_cast<std::uintptr_t> (&foreign_mixed),
                              {{Type (Type::Kind::F64)},
                               {Type (Type::Kind::I8), Type (Type::Kind::U16),
                                Type (Type::Kind::F64), Type (Type::Kind::F32),
                                Type (Type::Kind::LongDouble)}}},
                      Binding{"pair",
                              reinterpret_cast<std::uintptr_t> (&foreign_pair),
                              {{Type::i64 ()}, {Type::block (sizeof (Pair), pair_class)}}},
                      Binding{"large",
                              reinterpret_cast<std::uintptr_t> (&foreign_large),
                              {{}, {Type::return_block (24), Type::i64 ()}}},
                      Binding{"variadic",
                              reinterpret_cast<std::uintptr_t> (&foreign_variadic),
                              {{Type (Type::Kind::F64)}, {Type::i64 ()}, true}}};
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("ffi");
  REQUIRE ((m).has_value ());
  for (auto &b : bindings)
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ()).import (b.name, b.signature)).has_value ());
  unsigned resolved = 0;
  auto s = make (c, {.mode = ExecutionMode::Interpreter,
                     .link = {.resolve = [&] (auto &request) -> Result<ExternalSymbol> {
                       ++resolved;
                       for (auto &b : bindings)
                         if (request.name == b.name)
                           return ExternalSymbol{b.address, b.signature, {}};
                       return Error{ErrorCode::UnknownName, "unknown foreign import"};
                     }}});
  CHECK (resolved == 4);
  CHECK (s.host_entry_count () == 0);
  std::array mixed{ExecutionArgument (-3), ExecutionArgument (5), ExecutionArgument (20.0),
                   ExecutionArgument (10.0f), ExecutionArgument (10.0L)};
  CHECK ((call (s, find (s, "ffi", "mixed"), mixed)[0].as_double ()).value () == 42.0);
  Pair pair{20, 22};
  std::array block{ExecutionArgument::block (std::as_bytes (std::span (&pair, 1)), pair_class)};
  CHECK ((call (s, find (s, "ffi", "pair"), block)[0].as_i64 ()).value () == 42);
  CHECK (pair.x == 20);
  Large result{};
  std::array returned{ExecutionArgument::return_buffer (
                        std::as_writable_bytes (std::span (&result, 1))),
                      ExecutionArgument (20)};
  CHECK (call (s, find (s, "ffi", "large"), returned).empty ());
  CHECK (result.x == 20);
  CHECK (result.y == 21);
  CHECK (result.z == 22);
  std::array variadic{ExecutionArgument (10), ExecutionArgument (20), ExecutionArgument (12.0)};
  CHECK ((call (s, find (s, "ffi", "variadic"), variadic)[0].as_double ()).value () == 42.0);
  CHECK (s.host_entry_count () == 4);
  CHECK ((call (s, find (s, "ffi", "mixed"), mixed)[0].as_double ()).value () == 42.0);
  CHECK (s.host_entry_count () == 4);
}
TEST_CASE (
  "unified interpreter native calls cache through IR calls and honor the no-native policy") {
  if (!native ()) return;
  auto c = parse (
    "m:module\nimport host\np:proto d,i64:x,...\nf:func d\nlocal d:r\ncall "
    "p,host,r,10,20,12.0\nret r\nendfunc\nendmodule\n");
  auto resolver = [] (auto &) -> Result<ExternalSymbol> {
    return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&foreign_variadic),
                          Signature{{Type (Type::Kind::F64)}, {Type::i64 ()}, true},
                          {}};
  };
  auto s = make (*c, {.mode = ExecutionMode::Interpreter, .link = {.resolve = resolver}});
  auto f = find (s, "m", "f");
  CHECK ((call (s, f)[0].as_double ()).value () == 42.0);
  CHECK ((call (s, f)[0].as_double ()).value () == 42.0);
  CHECK (s.host_entry_count () == 1);
  auto disabled = make (*c, {.mode = ExecutionMode::Interpreter,
                             .link = {.resolve = resolver},
                             .native_calls = false});
  auto error = disabled.call (find (disabled, "m", "f"));
  REQUIRE_FALSE ((error).has_value ());
  CHECK (error.error ().message.find ("native foreign calls are disabled") != std::string::npos);
  CHECK (disabled.host_entry_count () == 0);
}
TEST_CASE (
  "unified sessions resolve exported function and data handles with the same ownership checks") {
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter}) {
    if (mode == ExecutionMode::JIT && !native ()) continue;
    auto c = test::variable_example (test::VariableExample::CrossModule);
    auto s = make (*c, {.mode = mode});
    auto function = s.find_export ("get_value"), data = s.find_export ("value");
    REQUIRE ((function).has_value ());
    REQUIRE ((data).has_value ());
    auto f = std::get_if<FunctionHandle> (&(function).value ());
    auto d = std::get_if<mirnext::execution::DataHandle> (&(data).value ());
    REQUIRE (f);
    REQUIRE (d);
    CHECK (s.owns (*f));
    CHECK ((call (s, *f)[0].as_i64 ()).value () == 20);
    auto bytes = d->writable_bytes ();
    REQUIRE ((bytes).has_value ());
    std::int64_t value = 42;
    std::memcpy (bytes.value ().data (), &value, 8);
    CHECK ((call (s, *f)[0].as_i64 ()).value () == 42);
    CHECK_FALSE ((s.find_export ("missing")).has_value ());
    auto moved = std::move (s);
    CHECK_FALSE ((s.find_export ("value")).has_value ());
    CHECK (moved.owns (*f));
  }
}
TEST_CASE ("interpreter rejects external JCALL even when a typed callback exists") {
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("m");
  REQUIRE ((m).has_value ());
  auto host = mirnext::ir::detail::access (((m)).value ()).import ("host", Signature{{}, {}});
  REQUIRE ((host).has_value ());
  auto f
    = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder b ((block).value ());
  REQUIRE ((b.jump_call ((host).value ())).has_value ());
  REQUIRE ((b.ret (42)).has_value ());
  bool invoked = false;
  auto s = make (c, {.mode = ExecutionMode::Interpreter,
                     .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                       ExternalSymbol symbol{0, Signature{{}, {}}, {}};
                       symbol.jump_return = true;
                       symbol.interpret = [&] (auto) -> Result<std::vector<ScalarValue>> {
                         invoked = true;
                         return std::vector<ScalarValue>{};
                       };
                       return symbol;
                     }}});
  auto result = s.call (find (s, "m", "entry"));
  REQUIRE_FALSE ((result).has_value ());
  CHECK_FALSE (invoked);
  CHECK (result.error ().message.find ("external jcall requires a nonstandard ABI entry")
         != std::string::npos);
  CHECK (s.host_entry_count () == 0);
}
TEST_CASE ("interpreter native adapter runtime diagnostics do not poison cached retries") {
  if (!native ()) return;
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("m");
  REQUIRE ((m).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ()).import ("host", Signature{{Type::i64 ()}, {}}))
      .has_value ());
  static bool fail = false;
  auto host = +[] () -> std::int64_t {
    if (fail) {
      auto f = builtin_address (codegen::Builtin::CopyBlock);
      REQUIRE ((f).has_value ());
      reinterpret_cast<void (*) (std::byte *, const std::byte *, std::uint64_t)> (
        (f).value ()) (nullptr, nullptr, 1);
    }
    return 42;
  };
  auto s = make (c, {.mode = ExecutionMode::Interpreter,
                     .link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                       return ExternalSymbol{reinterpret_cast<std::uintptr_t> (host),
                                             Signature{{Type::i64 ()}, {}},
                                             {}};
                     }}});
  auto f = find (s, "m", "host");
  fail = true;
  auto error = s.call (f);
  fail = false;
  REQUIRE_FALSE ((error).has_value ());
  CHECK (error.error ().message.find ("builtin:") != std::string::npos);
  CHECK ((call (s, f)[0].as_i64 ()).value () == 42);
  CHECK (s.host_entry_count () == 1);
}
TEST_CASE (
  "unified expression initialization uses the selected engine and a checked native adapter") {
  auto c = test::variable_example (test::VariableExample::ExpressionInitializer);
  for (auto mode : {ExecutionMode::JIT, ExecutionMode::Interpreter}) {
    if (mode == ExecutionMode::JIT && !native ()) continue;
    auto s = make (*c, {.mode = mode});
    auto data = s.find_data ("consumer", "value");
    REQUIRE ((data).has_value ());
    std::int64_t bits;
    std::memcpy (&bits, data.value ().bytes ().data (), 8);
    CHECK (bits == 42);
    CHECK (s.statistics ().expressions == 1);
    CHECK (s.host_entry_count () == (mode == ExecutionMode::JIT ? 1 : 0));
    CHECK ((call (s, find (s, "consumer", "entry"))[0].as_i64 ()).value () == 42);
  }
}
#if defined(__aarch64__) || (defined(__x86_64__) && !defined(_MSC_VER))
namespace {
[[gnu::noinline]] auto generic_preservation (ExecutionSession &s, const FunctionHandle &f,
                                             std::int64_t &after) {
#if defined(__aarch64__)
  asm volatile ("mov x19, %0" : : "r"(std::int64_t{77}) : "x19");
#else
  asm volatile ("movq %0, %%r12" : : "r"(std::int64_t{77}) : "r12");
#endif
  auto result = s.call (f);
#if defined(__aarch64__)
  asm volatile ("mov %0, x19" : "=r"(after));
  asm volatile ("" : : : "x19");
#else
  asm volatile ("movq %%r12, %0" : "=r"(after));
  asm volatile ("" : : : "r12");
#endif
  return result;
}
}  // namespace
TEST_CASE ("generic native adapters preserve host hardware bindings and retain MIR call effects") {
  for (unsigned level = 0; level < 4; ++level) {
    auto c = parse (
      "m:module\np:proto i64:x,...\ncallee:func i64:x,...\nglobal i64:b:r19\nmov "
      "b,0\nret\nendfunc\nentry:func i64\nglobal i64:a:r19\nmov a,42\ncall p,callee,0\nret "
      "a\nendfunc\nendmodule\n");
    auto s = make (*c, {.optimization = transforms::OptimizationOptions{
                          .level = static_cast<transforms::OptimizationLevel> (level)}});
    auto f = find (s, "m", "entry");
    std::int64_t after = 0;
    auto result = generic_preservation (s, f, after);
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    CHECK ((((result).value ())[0].as_i64 ()).value () == 0);
    CHECK (after == 77);
  }
}
#endif
