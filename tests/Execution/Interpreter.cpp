/* Licensed under the MIT License. Interpreter execution tests. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../IR/StackScopes.hpp"
#include "InterpreterFixtures.hpp"
#include <array>
#include <bit>
#include <cstring>
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
ExecutionSession program (const Program &c, ExecutionOptions options = {}) {
  options.mode = ExecutionMode::Interpreter;
  auto p = ExecutionSession::create (c, std::move (options));
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  return std::move ((p).value ());
}
FunctionHandle find (const ExecutionSession &p, std::string_view module, std::string_view name) {
  auto f = p.find_function (module, name);
  REQUIRE ((f).has_value ());
  return (f).value ();
}
std::int64_t call (const ExecutionSession &p, std::string_view module, std::string_view name,
                   std::span<const std::int64_t> args = {}) {
  auto v = p.call_i64 (find (p, module, name), args);
  INFO (((v).has_value () ? "" : v.error ().message));
  REQUIRE ((v).has_value ());
  return (v).value ();
}
}  // namespace
TEST_CASE ("owning interpreter executes original and O0-O3 call and variable fixtures") {
  for (unsigned level = 0; level < 5; ++level) {
    for (unsigned n = 0; n <= unsigned (test::CallExample::Inline); ++n) {
      CAPTURE (level);
      CAPTURE (n);
      auto c = test::call_example (static_cast<test::CallExample> (n));
      auto before = dump (*c);
      auto optimized
        = transforms::optimize (*c, {.level = static_cast<transforms::OptimizationLevel> (
                                       level ? level - 1 : 0)});
      REQUIRE ((optimized).has_value ());
      auto p = level ? ExecutionSession::create (std::move ((optimized).value ()),
                                                 {.mode = ExecutionMode::Interpreter})
                     : ExecutionSession::create (*c, {.mode = ExecutionMode::Interpreter});
      INFO (((p).has_value () ? "" : p.error ().message));
      REQUIRE ((p).has_value ());
      CHECK (dump (*c) == before);
      c.reset ();
      CHECK (call ((p).value (), "calls", "entry") == 42);
    }
    for (unsigned n = 0; n <= unsigned (test::VariableExample::Narrow); ++n) {
      CAPTURE (level);
      CAPTURE (n);
      auto c = test::variable_example (static_cast<test::VariableExample> (n));
      auto optimized
        = transforms::optimize (*c, {.level = static_cast<transforms::OptimizationLevel> (
                                       level ? level - 1 : 0)});
      REQUIRE ((optimized).has_value ());
      auto p = level ? ExecutionSession::create (std::move ((optimized).value ()),
                                                 {.mode = ExecutionMode::Interpreter})
                     : ExecutionSession::create (*c, {.mode = ExecutionMode::Interpreter});
      REQUIRE ((p).has_value ());
      c.reset ();
      CHECK (call ((p).value (), "consumer", "entry") == 42);
    }
  }
}
TEST_CASE ("owning interpreter prepares and executes all scalar and comparison branch opcodes") {
  for (unsigned fixture = 0; fixture < 3; ++fixture) {
    auto c = fixture == 0   ? test::interpreter_scalars ()
             : fixture == 1 ? test::interpreter_ieee ()
                            : test::interpreter_overflow ();
    auto p = program (*c);
    CHECK (p.interpreter_statistics ().value ().functions > 80);
    for (auto &m : c->modules ())
      for (auto &f : m.functions ()) {
        auto result = p.call (find (p, m.name (), f.symbol ().name ()));
        INFO (f.symbol ().name ());
        INFO (((result).has_value () ? "" : result.error ().message));
        REQUIRE ((result).has_value ());
        CHECK (result.value ().size () == 1);
        CHECK (((result).value ())[0].type ().is_scalar ());
      }
  }
}
TEST_CASE (
  "owning interpreter retains data and function handles across source destruction and moves") {
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto p = program (*c);
  auto before = dump (p.program ());
  auto f = find (p, "consumer", "entry");
  auto data = p.find_data ("consumer", "value");
  REQUIRE ((data).has_value ());
  c.reset ();
  CHECK (call (p, "consumer", "entry") == 42);
  auto moved = std::move (p);
  CHECK_FALSE ((p.call_i64 (f)).has_value ());
  CHECK_FALSE ((p.find_data ("consumer", "value")).has_value ());
  CHECK (call (moved, "consumer", "entry") == 64);
  auto other = program (moved.program ());
  CHECK_FALSE ((other.call_i64 (f)).has_value ());
  CHECK (dump (moved.program ()) == before);
  auto bytes = data.value ().bytes ();
  std::int64_t value;
  std::memcpy (&value, bytes.data (), 8);
  CHECK (value == 64);
}
TEST_CASE (
  "owning interpreter implements local addresses narrow accesses labels and jump returns") {
  std::string hard
    = target::TargetMachine::native ().value ().architecture () == target::Architecture::AArch64
        ? "r19"
        : "r12";
  auto c = parse("m:module\nget:func\nglobal i64:next:"+hard+"\njret next\nendfunc\np:proto\nf:func i64\nglobal i64:target:"+hard+"\nlocal i64:x,i64:a\nmov x,42\naddr8 a,x\nmov i8:0(a),-3\next8 x,x\nladdr target,done\njcall p,get\nret 99\ndone:ret x\nendfunc\nlabeldata:lref done,done,7\nendmodule\n");
  auto p = program (*c);
  CHECK (call (p, "m", "f") == -3);
  auto data = p.find_data ("m", "labeldata");
  REQUIRE ((data).has_value ());
  std::uint64_t difference;
  std::memcpy (&difference, data.value ().bytes ().data (), 8);
  CHECK (difference == 7);
}
TEST_CASE ("owning interpreter typed host calls support mixed values and validate results") {
  auto c = parse (
    "m:module\np:proto d,i64:x,d:y\nimport host\nf:func d\nlocal d:r\ncall p,host,r,7,2.5\nret "
    "r\nendfunc\nendmodule\n");
  unsigned calls = 0;
  bool fail = false;
  auto p = program (*c, {.link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                           ExternalSymbol s{0,
                                            Signature{{Type (Type::Kind::F64)},
                                                      {Type::i64 (), Type (Type::Kind::F64)}},
                                            {}};
                           s.interpret = [&] (auto args) -> Result<std::vector<ScalarValue>> {
                             ++calls;
                             REQUIRE (args.size () == 2);
                             CHECK (args[0].scalar ().value ().as_i64 ().value () == 7);
                             CHECK (args[1].scalar ().value ().as_double ().value () == 2.5);
                             if (fail) return std::vector<ScalarValue>{ScalarValue (1)};
                             return std::vector<ScalarValue>{ScalarValue (9.5)};
                           };
                           return s;
                         }}});
  auto f = find (p, "m", "f");
  auto result = p.call (f);
  REQUIRE ((result).has_value ());
  CHECK ((((result).value ())[0].as_double ()).value () == 9.5);
  CHECK (calls == 1);
  fail = true;
  CHECK_FALSE ((p.call (f)).has_value ());
  fail = false;
  result = p.call (f);
  REQUIRE ((result).has_value ());
  CHECK ((((result).value ())[0].as_double ()).value () == 9.5);
}
TEST_CASE (
  "owning interpreter returns diagnostics for undefined arithmetic and bounded execution") {
  for (auto text : {"div r,1,0", "div r,-9223372036854775808,-1", "lsh r,1,64",
                    "mov r,3\nswitch r,done", "bend 1", "jmpi 0", "mov r,-1\nalloca r,r"}) {
    auto c = parse ("m:module\nf:func i64\nlocal i64:r\n" + std::string (text)
                    + "\ndone:ret 42\nendfunc\nendmodule\n");
    auto p = program (*c);
    auto result = p.call_i64 (find (p, "m", "f"));
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("m/f") != std::string::npos);
  }
  auto c = parse ("m:module\nf:func i64\nloop:jmp loop\nret 0\nendfunc\nendmodule\n");
  auto p = program (*c, {.instruction_limit = 19});
  auto result = p.call_i64 (find (p, "m", "f"));
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("instruction limit") != std::string::npos);
}
TEST_CASE ("owning interpreter never asks for native code publication") {
  struct Reject final : CodeMemoryProvider {
    unsigned calls = 0;
    std::size_t page_size () const override { return 4096; }
    Result<MappedCodeMemory> allocate (std::size_t) override {
      ++calls;
      return Error{ErrorCode::InvalidArgument, "unexpected native allocation"};
    }
    Result<void> release (MappedCodeMemory) override {
      ++calls;
      return {};
    }
    Result<void> make_writable (MappedCodeMemory) override {
      ++calls;
      return {};
    }
    Result<void> make_executable (MappedCodeMemory) override {
      ++calls;
      return {};
    }
    Result<void> flush (MappedCodeMemory) override {
      ++calls;
      return {};
    }
  };
  auto provider = std::make_shared<Reject> ();
  auto c = test::call_example (test::CallExample::VariadicBlock);
  auto p = program (*c, {.link = {.memory_provider = provider}});
  CHECK (call (p, "calls", "entry") == 42);
  CHECK (provider->calls == 0);
}
TEST_CASE ("owning interpreter copies every block class and preserves writable return buffers") {
  for (unsigned category = 0; category <= 4; ++category) {
    CAPTURE (category);
    auto block_type = category ? "blk" + std::to_string (category) : std::string ("blk");
    auto c = parse (
      "m:module\nf:func i64," + block_type
      + ":16(a)\nlocal i64:r\nmov r,i64:0(a)\nmov i64:0(a),99\nret r\nendfunc\nendmodule\n");
    auto p = program (*c);
    std::array<std::int64_t, 2> values{42, 7};
    std::array args{ExecutionArgument::block (std::as_bytes (std::span (values)),
                                              static_cast<Type::BlockClass> (category))};
    auto result = p.call (find (p, "m", "f"), args);
    REQUIRE ((result).has_value ());
    CHECK ((((result).value ())[0].as_i64 ()).value () == 42);
    CHECK (values[0] == 42);
    std::array bad{ExecutionArgument::block (std::as_bytes (std::span (values)).first (8),
                                             static_cast<Type::BlockClass> (category))};
    CHECK_FALSE ((p.call (find (p, "m", "f"), bad)).has_value ());
  }
  auto c = test::call_example (test::CallExample::ReturnBlock);
  auto p = program (*c);
  std::array<std::int64_t, 2> values{};
  std::array args{ExecutionArgument::return_buffer (std::as_writable_bytes (std::span (values)))};
  auto result = p.call (find (p, "calls", "sum"), args);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().empty ());
  CHECK (values[0] == 42);
  std::array bad{ExecutionArgument::block (std::as_bytes (std::span (values)))};
  CHECK_FALSE ((p.call (find (p, "calls", "sum"), bad)).has_value ());
}
TEST_CASE ("owning interpreter keeps stack marks and locals valid through nested segments") {
  auto c = parse (
    "m:module\nf:func i64\nlocal i64:x,i64:a,i64:b,i64:c,i64:outer,i64:inner,i64:z\nbstart "
    "outer\nalloca a,16\nmov i64:0(a),42\nbstart inner\nalloca b,16\nbend inner\nalloca c,16\nbne "
    "bad,b,c\nalloca b,0\nalloca z,0\nbne bad,b,z\nalloca b,8192\nmov i64:0(b),7\nbend inner\nmov "
    "x,i64:0(a)\nbend outer\nret x\nbad:ret -1\nendfunc\nendmodule\n");
  auto p = program (*c);
  CHECK (call (p, "m", "f") == 42);
}
TEST_CASE ("owning interpreter bounds recursive calls without poisoning subsequent calls") {
  auto c = parse (
    "m:module\np:proto i64,i64:n\nf:func i64,i64:n\nlocal i64:r\nbeq done,n,0\nsub n,n,1\ncall "
    "p,f,r,n\nadd r,r,1\nret r\ndone:ret 0\nendfunc\nendmodule\n");
  auto p = program (*c, {.call_depth_limit = 8});
  auto f = find (p, "m", "f");
  std::array<std::int64_t, 1> args{7};
  auto value = p.call_i64 (f, args);
  REQUIRE ((value).has_value ());
  CHECK ((value).value () == 7);
  args[0] = 8;
  value = p.call_i64 (f, args);
  REQUIRE_FALSE ((value).has_value ());
  CHECK (value.error ().message.find ("call depth limit") != std::string::npos);
  args[0] = 2;
  value = p.call_i64 (f, args);
  REQUIRE ((value).has_value ());
  CHECK ((value).value () == 2);
}
TEST_CASE ("owning interpreter rejects exhausted varargs and unpromoted host arguments") {
  auto c = test::call_example (test::CallExample::Variadic);
  auto p = program (*c);
  auto f = find (p, "calls", "sum");
  std::array short_args{ExecutionArgument (0), ExecutionArgument (42)};
  auto value = p.call (f, short_args);
  REQUIRE_FALSE ((value).has_value ());
  CHECK (value.error ().message.find ("variadic argument exceeds") != std::string::npos);
  std::array float_args{ExecutionArgument (0), ExecutionArgument (1.0f)};
  CHECK_FALSE ((p.call (f, float_args)).has_value ());
  std::array args{ExecutionArgument (0), ExecutionArgument (20), ExecutionArgument (22)};
  value = p.call (f, args);
  REQUIRE ((value).has_value ());
  CHECK ((((value).value ())[0].as_i64 ()).value () == 42);
}
TEST_CASE ("owning interpreter enforces readonly storage through computed addresses") {
  auto c = test::variable_example (test::VariableExample::Constant);
  auto &m = *c->modules ().begin ();
  auto symbol = m.find_symbol ("value").get ();
  REQUIRE (symbol);
  auto f = mirnext::ir::detail::access (m).create_function ("overwrite", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder b ((block).value ());
  auto address = b.address_of (*symbol);
  REQUIRE ((address).has_value ());
  REQUIRE (
    (b.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(address).value ()), 99))
      .has_value ());
  REQUIRE ((b.ret (1)).has_value ());
  auto p = program (*c);
  auto result = p.call_i64 (find (p, "consumer", "overwrite"));
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("read-only loaded data") != std::string::npos);
  CHECK (call (p, "consumer", "entry") == 42);
}
TEST_CASE (
  "owning interpreter uses addresses for undef va-list operands and copies their cursors") {
  auto c = parse (
    "m:module\nf:func i64,i64:fixed,...\nlocal "
    "i64:list,i64:copy,i64:cursor,i64:a,i64:b,i64:c,d:v\nalloca list,64\nalloca copy,64\nmov "
    "i64:0(list),0\nmov i64:8(list),0\nmov i64:16(list),0\nmov i64:24(list),0\nva_start "
    "undef:(list)\nmov i64:0(copy),i64:0(list)\nmov i64:8(copy),i64:8(list)\nmov "
    "i64:16(copy),i64:16(list)\nmov i64:24(copy),i64:24(list)\nva_arg "
    "cursor,undef:(list),i64:0\nmov a,i64:0(cursor)\nva_arg cursor,undef:(list),d:0\ndmov "
    "v,d:0(cursor)\nd2i b,v\nva_arg cursor,undef:(copy),i64:0\nmov c,i64:0(cursor)\nva_end "
    "undef:(list)\nadd a,a,b\nadd a,a,c\nret a\nendfunc\nendmodule\n");
  auto p = program (*c);
  std::array args{ExecutionArgument (0), ExecutionArgument (10), ExecutionArgument (22.0)};
  auto value = p.call (find (p, "m", "f"), args);
  INFO (((value).has_value () ? "" : value.error ().message));
  REQUIRE ((value).has_value ());
  CHECK ((((value).value ())[0].as_i64 ()).value () == 42);
}
TEST_CASE ("owning interpreter shares typed host symbol identity across module imports") {
  auto c = parse (
    "a:module\nimport host\nexport address\naddress:func i64\nret "
    "host\nendfunc\nendmodule\nb:module\nimport host,address\np:proto i64\nf:func i64\nlocal "
    "i64:r\ncall p,address,r\neq r,r,host\nret r\nendfunc\nendmodule\n");
  unsigned resolutions = 0;
  auto p = program (*c, {.link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                           ++resolutions;
                           ExternalSymbol value{0, Signature{{Type::i64 ()}, {}}, {}};
                           value.interpret = [] (auto) -> Result<std::vector<ScalarValue>> {
                             return std::vector<ScalarValue>{ScalarValue (42)};
                           };
                           return value;
                         }}});
  CHECK (resolutions == 1);
  CHECK (call (p, "b", "f") == 1);
  CHECK (call (p, "a", "host") == 42);
}
TEST_CASE ("owning interpreter materializes nested stack scopes on each outgoing edge") {
  for (auto mode :
       {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto c = test::stack_scope_example (mode);
    auto original = dump (*c);
    auto raw = program (*c);
    CHECK (call (raw, "scopes", "entry") == 42);
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (mode);
      CAPTURE (level);
      auto optimized
        = transforms::optimize (*c, {.level = static_cast<transforms::OptimizationLevel> (level)});
      REQUIRE ((optimized).has_value ());
      auto p = ExecutionSession::create (std::move ((optimized).value ()),
                                         {.mode = ExecutionMode::Interpreter});
      REQUIRE ((p).has_value ());
      CHECK (call ((p).value (), "scopes", "entry") == 42);
    }
    CHECK (dump (*c) == original);
  }
}
TEST_CASE ("owning interpreter narrow local addresses modify the intended scalar lane") {
  for (auto bits : {8, 16, 32}) {
    auto width = std::to_string (bits);
    auto c
      = parse ("m:module\nf:func i64\nlocal i64:x,i64:a\nmov x,42\naddr" + width + " a,x\nmov i"
               + width + ":0(a),-3\next" + width + " x,x\nret x\nendfunc\nendmodule\n");
    auto p = program (*c);
    CHECK (call (p, "m", "f") == -3);
  }
}
TEST_CASE ("owning interpreter retains ordinary property and narrow boolean branch semantics") {
  for (auto name : {"prbeq", "prbne"})
    for (auto property : {0, 1}) {
      auto c = parse ("m:module\nf:func i64\nlocal i64:r\nmov r,42\nprset r,99\n"
                      + std::string (name) + " yes,r," + std::to_string (property)
                      + "\nret 0\nyes:ret 1\nendfunc\nendmodule\n");
      auto p = program (*c);
      CHECK (call (p, "m", "f")
             == (std::string_view (name) == "prbeq" ? property == 0 : property != 0));
    }
  for (auto name : {"bt", "bf", "bts", "bfs"}) {
    auto c = parse ("m:module\nf:func i64\nlocal i64:r\nmov r,4294967296\n" + std::string (name)
                    + " yes,r\nret 0\nyes:ret 1\nendfunc\nendmodule\n");
    auto p = program (*c);
    bool expected = std::string_view (name) == "bt" || std::string_view (name) == "bfs";
    CHECK (call (p, "m", "f") == expected);
  }
}
TEST_CASE ("owning interpreter reports missing native-address adapters without allocating code") {
  auto c = parse (
    "m:module\nimport host\np:proto i64\nf:func i64\nlocal i64:r\ncall p,host,r\nret "
    "r\nendfunc\nendmodule\n");
  unsigned calls = 0;
  auto host = +[] () -> std::int64_t { return 42; };
  auto p = program (*c, {.link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                           ++calls;
                           return ExternalSymbol{reinterpret_cast<std::uintptr_t> (host),
                                                 Signature{{Type::i64 ()}, {}},
                                                 {}};
                         }},
                         .native_calls = false});
  auto value = p.call_i64 (find (p, "m", "f"));
  REQUIRE_FALSE ((value).has_value ());
  CHECK (calls == 1);
  CHECK (value.error ().message.find ("typed interpreter implementation") != std::string::npos);
}
TEST_CASE (
  "owning interpreter preserves original function lookup after cross-module inline renaming") {
  auto c = parse (
    "defs:module\nhidden:func i64\nret 7\nendfunc\npublic:func i64\nret hidden\nendfunc\nexport "
    "public\nendmodule\nconsumer:module\nimport public\np:proto i64\nentry:func i64\nlocal "
    "i64:a,i64:r\ncall p,public,a\ncall p,a,r\nret r\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto optimized
      = transforms::optimize (*c, {.level = static_cast<transforms::OptimizationLevel> (level)});
    REQUIRE ((optimized).has_value ());
    REQUIRE ((optimized.value ().find_function ("defs", "hidden")).has_value ());
    CHECK (optimized.value ().program ().find_module ("defs").get ()->find_symbol ("hidden").get ()
           == nullptr);
    auto p = ExecutionSession::create (std::move ((optimized).value ()),
                                       {.mode = ExecutionMode::Interpreter});
    REQUIRE ((p).has_value ());
    CHECK (call ((p).value (), "consumer", "entry") == 7);
    CHECK (call ((p).value (), "defs", "hidden") == 7);
  }
}
TEST_CASE (
  "owning interpreter accepts the verifier's shared double and long-double representation") {
  if constexpr (sizeof (long double) != sizeof (double)) return;
  auto c
    = parse ("m:module\nf:func ld\nret 1.25\nendfunc\ng:func d\nret 2.5L\nendfunc\nendmodule\n");
  auto p = program (*c);
  auto a = p.call (find (p, "m", "f")), b = p.call (find (p, "m", "g"));
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  CHECK ((((a).value ())[0].as_long_double ()).value () == 1.25L);
  CHECK ((((b).value ())[0].as_double ()).value () == 2.5);
}

TEST_CASE ("owning interpreter executes v128 vector arithmetic and moves") {
  auto c = parse (R"(m:module
add:func v128<i32x4>
local v128<i32x4>:x,v128<i32x4>:y
vmov x,v128<i32x4>:0x01000000020000000300000004000000
vadd y,x,x
ret y
endfunc
sub:func v128<i32x4>
local v128<i32x4>:x,v128<i32x4>:y
vmov x,v128<i32x4>:0x01000000020000000300000004000000
vsub y,x,x
ret y
endfunc
band:func v128<i32x4>
local v128<i32x4>:x,v128<i32x4>:y
vmov x,v128<i32x4>:0x01000000020000000300000004000000
vand y,x,x
ret y
endfunc
bor:func v128<i32x4>
local v128<i32x4>:x,v128<i32x4>:y
vmov x,v128<i32x4>:0x01000000020000000300000004000000
vor y,x,x
ret y
endfunc
xor:func v128<i32x4>
local v128<i32x4>:x,v128<i32x4>:y
vmov x,v128<i32x4>:0x01000000020000000300000004000000
vxor y,x,x
ret y
endfunc
endmodule
)");
  auto p = program (*c);
  std::array<std::byte, 16> input{};
  std::uint32_t input_lanes[]{1, 2, 3, 4};
  std::memcpy (input.data (), input_lanes, sizeof input_lanes);
  std::array<std::byte, 16> doubled{};
  std::uint32_t doubled_lanes[]{2, 4, 6, 8};
  std::memcpy (doubled.data (), doubled_lanes, sizeof doubled_lanes);
  for (auto [name, expected] :
       {std::pair{"add", doubled}, std::pair{"sub", std::array<std::byte, 16>{}},
        std::pair{"band", input}, std::pair{"bor", input},
        std::pair{"xor", std::array<std::byte, 16>{}}}) {
    auto result = p.call (find (p, "m", name));
    REQUIRE ((result).has_value ());
    REQUIRE ((result).value ().size () == 1);
    auto bytes = (result).value ()[0].as_vector ();
    REQUIRE (bytes.size () == 16);
    CHECK (std::equal (bytes.begin (), bytes.end (), expected.begin (), expected.end ()));
  }
}

TEST_CASE ("owning JIT executes the v128 i8x16 core") {
  auto c = parse (R"(m:module
f:func v128<i8x16>,v128<i8x16>:a
local v128<i8x16>:r
vadd r,a,a
ret r
endfunc
endmodule
)");
  ExecutionOptions options;
  options.mode = ExecutionMode::JIT;
  auto session = ExecutionSession::create (std::move (*c), options);
  REQUIRE ((session).has_value ());
  std::array<std::byte, 16> bytes{};
  for (unsigned n = 0; n < bytes.size (); ++n) bytes[n] = std::byte (n + 1);
  auto input = ScalarValue::vector (Type::vector128 (Type::VectorElement::I8, 16), bytes);
  REQUIRE ((input).has_value ());
  std::array args{ExecutionArgument ((input).value ())};
  auto result = (session).value ().call (find ((session).value (), "m", "f"), args);
  REQUIRE ((result).has_value ());
  REQUIRE ((result).value ().size () == 1);
  for (unsigned n = 0; n < bytes.size (); ++n)
    CHECK (result.value ()[0].as_vector ()[n] == std::byte (2 * (n + 1)));
}
