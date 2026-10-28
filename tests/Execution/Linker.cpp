/* Licensed under the MIT License. Owning linking and native execution. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "../IR/Variables.hpp"
#include "../IR/Calls.hpp"
#include <array>
#include <bit>
#include <cstring>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
bool native () { return test::native_execution_available (); }
std::unique_ptr<Program> parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
template <class T>
T read (std::span<const std::byte> bytes) {
  REQUIRE (bytes.size () >= sizeof (T));
  T value;
  std::memcpy (&value, bytes.data (), sizeof value);
  return value;
}
ExecutionSession linked (const Program &context, unsigned level = 2, LinkOptions options = {}) {
  ExecutionOptions o;
  o.optimization.emplace ().level = static_cast<transforms::OptimizationLevel> (level);
  o.link = std::move (options);
  auto p = ExecutionSession::create (context, std::move (o));
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  return std::move ((p).value ());
}
std::int64_t call (const ExecutionSession &p, std::string_view module, std::string_view name) {
  auto f = p.find_function (module, name);
  REQUIRE ((f).has_value ());
  return reinterpret_cast<std::int64_t (*) ()> (f.value ().address ()) ();
}
struct Provider final : CodeMemoryProvider {
  std::shared_ptr<CodeMemoryProvider> native = native_code_memory_provider ();
  unsigned maps = 0, unmaps = 0, writes = 0;
  bool fail_write = false;
  std::size_t page_size () const override { return native->page_size (); }
  Result<MappedCodeMemory> allocate (std::size_t size) override {
    ++maps;
    return native->allocate (size);
  }
  Result<void> release (MappedCodeMemory memory) override {
    ++unmaps;
    return native->release (memory);
  }
  Result<void> make_writable (MappedCodeMemory memory) override {
    ++writes;
    if (fail_write) return Error{ErrorCode::InvalidArgument, "injected publication failure"};
    return native->make_writable (memory);
  }
  Result<void> make_executable (MappedCodeMemory memory) override {
    return native->make_executable (memory);
  }
  Result<void> flush (MappedCodeMemory memory) override { return native->flush (memory); }
};
}  // namespace
TEST_CASE ("native module layout preserves packed anonymous items interruption and zero sections") {
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("layout");
  REQUIRE ((m).has_value ());
  auto a = mirnext::ir::detail::access (((m)).value ())
             .create_data ("head", InitializedData{Type (Type::Kind::U8), {0xaa}});
  REQUIRE ((a).has_value ());
  auto b = mirnext::ir::detail::access (((m)).value ())
             .create_data ({}, InitializedData{Type (Type::Kind::U16), {0x1234}});
  REQUIRE ((b).has_value ());
  auto z = mirnext::ir::detail::access (((m)).value ()).create_data ({}, ZeroData{3});
  REQUIRE ((z).has_value ());
  auto r
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ({},
                      mirnext::ir::detail::NodeAccess::make_SymbolReference (&a.value ().symbol (),
                                                                             -1));
  REQUIRE ((r).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).prototype ("break", {})).has_value ());
  auto q = mirnext::ir::detail::access (((m)).value ()).create_data ({}, ZeroData{0});
  REQUIRE ((q).has_value ());
  auto last = mirnext::ir::detail::access (((m)).value ()).create_data ("last", ZeroData{0});
  REQUIRE ((last).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto layout = layout_module_data ((m).value (), (target).value ());
  REQUIRE ((layout).has_value ());
  REQUIRE (layout.value ().size () == 3);
  CHECK (((layout).value ())[0].size == 16);
  const std::size_t offsets[]{0, 1, 3, 6};
  for (unsigned n = 0; n < 4; ++n) CHECK (((layout).value ())[0].items[n].offset == offsets[n]);
  CHECK (((layout).value ())[1].size == 0);
  CHECK (((layout).value ())[2].size == 0);
  auto p = linked (c);
  CHECK (p.statistics ().sections == 3);
  CHECK (p.statistics ().code_batches == 0);
  auto head = p.find_data ("layout", "head");
  REQUIRE ((head).has_value ());
  CHECK (read<std::uint8_t> (head.value ().bytes ()) == 0xaa);
  auto bytes = reinterpret_cast<const std::byte *> (head.value ().address ());
  CHECK (read<std::uint16_t> ({bytes + 1, 2}) == 0x1234);
  CHECK (bytes[3] == std::byte{});
  CHECK (bytes[5] == std::byte{});
  CHECK (read<std::uintptr_t> ({bytes + 6, 8}) == head.value ().address () - 1);
  CHECK_FALSE ((p.find_function ("layout", "head")).has_value ());
  CHECK_FALSE ((p.find_data ("layout", "absent")).has_value ());
  CHECK_FALSE ((p.find_export ("head")).has_value ());
}
TEST_CASE ("native execution session owns linked state and checks typed i64 host calls") {
  if (!native ()) return;
  auto context = test::variable_example (test::VariableExample::Initialized);
  auto session = ExecutionSession::create (*context);
  REQUIRE ((session).has_value ());
  auto entry = session.value ().find_function ("consumer", "entry");
  REQUIRE ((entry).has_value ());
  auto result = session.value ().call_i64 ((entry).value ());
  REQUIRE ((result).has_value ());
  CHECK ((result).value () == 42);
  auto wrong = session.value ().call_i64 ((entry).value (), std::array<std::int64_t, 1>{7});
  CHECK_FALSE ((wrong).has_value ());
  auto moved = std::move ((session).value ());
  CHECK ((moved.status ()).has_value ());
  CHECK_FALSE ((session.value ().status ()).has_value ());
  auto copied_handle = moved.find_function ("consumer", "entry");
  REQUIRE ((copied_handle).has_value ());
  auto again = moved.call_i64 ((copied_handle).value ());
  REQUIRE ((again).has_value ());
  CHECK ((again).value () == 64);
}
TEST_CASE ("native layout returns diagnostics before overflowing or allocating huge sections") {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Program c;
    auto m = mirnext::ir::detail::access (c).create_module ("overflow");
    REQUIRE ((m).has_value ());
    REQUIRE ((mirnext::ir::detail::access (((m)).value ())
                .create_data ("huge", ZeroData{mode == 1 ? SIZE_MAX - 7 : SIZE_MAX}))
               .has_value ());
    if (mode == 1)
      REQUIRE (
        (mirnext::ir::detail::access (((m)).value ()).create_data ({}, ZeroData{8})).has_value ());
    auto target = target::TargetMachine::native ();
    REQUIRE ((target).has_value ());
    CHECK_FALSE ((layout_module_data ((m).value (), (target).value ())).has_value ());
    CHECK_FALSE ((ExecutionSession::create (c)).has_value ());
  }
}
TEST_CASE ("native linked scalar variables execute at all optimization levels") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (auto mode : {test::VariableExample::Initialized, test::VariableExample::Zero,
                      test::VariableExample::Constant, test::VariableExample::Boolean,
                      test::VariableExample::Floating, test::VariableExample::Pointer,
                      test::VariableExample::ExpressionInitializer,
                      test::VariableExample::CrossModule, test::VariableExample::Narrow}) {
      CAPTURE (level);
      CAPTURE (static_cast<unsigned> (mode));
      auto c = test::variable_example (mode);
      auto before = dump (*c);
      auto p = linked (*c, level);
      CHECK (dump (*c) == before);
      c.reset ();
      CHECK (call (p, "consumer", "entry") == 42);
      CHECK (p.statistics ().code_batches == 1);
      CHECK (p.statistics ().expressions
             == (mode == test::VariableExample::ExpressionInitializer ? 1 : 0));
      if (mode == test::VariableExample::CrossModule) {
        auto a = p.find_data ("consumer", "value"), b = p.find_data ("definition", "value");
        REQUIRE ((a).has_value ());
        REQUIRE ((b).has_value ());
        CHECK (a.value ().address () == b.value ().address ());
        CHECK (read<std::int64_t> (b.value ().bytes ()) == 42);
        CHECK (call (p, "definition", "get_value") == 42);
      }
      if (mode == test::VariableExample::Constant) {
        auto data = p.find_data ("consumer", "value");
        REQUIRE ((data).has_value ());
        CHECK_FALSE ((data.value ().writable_bytes ()).has_value ());
      }
    }
}
TEST_CASE (
  "native linked handles keep private definitions code and data alive after all producers") {
  if (!native ()) return;
  std::optional<FunctionHandle> function;
  std::optional<mirnext::execution::DataHandle> data;
  {
    auto c = test::variable_example (test::VariableExample::Initialized);
    auto p = linked (*c);
    c.reset ();
    auto f = p.find_function ("consumer", "entry");
    auto d = p.find_data ("consumer", "value");
    REQUIRE ((f).has_value ());
    REQUIRE ((d).has_value ());
    function = (f).value ();
    data = (d).value ();
    auto moved = std::move (p);
    CHECK_FALSE ((p.find_function ("consumer", "entry")).has_value ());
    CHECK ((moved.find_function ("consumer", "entry")).has_value ());
  }
  REQUIRE (function);
  REQUIRE ((function->artifact ()).has_value ());
  REQUIRE ((function->artifact ().value ().verify ()).has_value ());
  CHECK (function->signature ().results == std::vector{Type::i64 ()});
  CHECK (reinterpret_cast<std::int64_t (*) ()> (function->address ()) () == 42);
  REQUIRE (data);
  CHECK (read<std::int64_t> (data->bytes ()) == 42);
  auto copy = function;
  function.reset ();
  CHECK (reinterpret_cast<std::int64_t (*) ()> (copy->address ()) () == 64);
}
TEST_CASE ("native linking repeated programs isolate globals and do not rewrite source IR") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto before = dump (*c);
  auto first = linked (*c), second = linked (*c);
  CHECK (call (first, "consumer", "entry") == 42);
  CHECK (call (first, "consumer", "entry") == 64);
  CHECK (call (second, "consumer", "entry") == 42);
  CHECK (dump (*c) == before);
  auto a = first.find_data ("consumer", "value"), b = second.find_data ("consumer", "value");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  CHECK (a.value ().address () != b.value ().address ());
}
TEST_CASE (
  "native linking resolves imports once by name validates host metadata and shares bindings") {
  Program c;
  std::int64_t value = 17;
  unsigned calls = 0;
  for (auto name : {"a", "b"}) {
    auto m = mirnext::ir::detail::access (c).create_module (name);
    REQUIRE ((m).has_value ());
    REQUIRE ((mirnext::ir::detail::access (((m)).value ()).import_variable ("value", Type::i64 ()))
               .has_value ());
  }
  auto provider = std::make_shared<Provider> ();
  auto p = linked (c, 2,
                   {.resolve = [&] (const ImportRequest &request) -> Result<ExternalSymbol> {
                      ++calls;
                      CHECK (request.name == "value");
                      REQUIRE ((request.variable ()).has_value ());
                      return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value),
                                            {},
                                            ExternalData{8, true, Type::i64 ()}};
                    },
                    .memory_provider = provider});
  CHECK (calls == 1);
  CHECK (p.statistics ().external_resolutions == 1);
  CHECK (provider->maps == 0);
  auto a = p.find_data ("a", "value"), b = p.find_data ("b", "value");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  CHECK (a.value ().address () == b.value ().address ());
  auto bytes = a.value ().writable_bytes ();
  REQUIRE ((bytes).has_value ());
  std::int64_t updated = 42;
  std::memcpy (bytes.value ().data (), &updated, 8);
  CHECK (value == 42);
  CHECK (read<std::int64_t> (b.value ().bytes ()) == 42);
  for (unsigned mode = 0; mode < 6; ++mode) {
    ExecutionOptions options;
    options.link.memory_provider = provider;
    options.link.resolve = [&] (auto &) -> Result<ExternalSymbol> {
      if (!mode) return Error{ErrorCode::UnknownName, "missing host value"};
      if (mode == 1) return ExternalSymbol{0, {}, ExternalData{8}};
      if (mode == 2)
        return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value), {}, ExternalData{4}};
      if (mode == 3)
        return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value),
                              {},
                              ExternalData{8, false}};
      if (mode == 4)
        return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value),
                              {},
                              ExternalData{8, true, Type (Type::Kind::F64)}};
      return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value),
                            Signature{{Type::i64 ()}, {}},
                            {}};
    };
    auto failed = ExecutionSession::create (c, options);
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("link:symbols") != std::string::npos);
    CHECK (provider->maps == 0);
  }
}
TEST_CASE (
  "native function references and cross-module calls use linked definitions before host "
  "resolution") {
  if (!native ()) return;
  auto c = parse (
    "a:module\nexport target\ntarget:func i64,i64:x\nlocal i64:r\nadd r,x,5\nret "
    "r\nendfunc\nendmodule\nb:module\np:proto i64,i64:x\nimport target\nptr:ref "
    "target,0\nentry:func i64\nlocal i64:r,i64:fp\nmov fp,ptr\nmov fp,i64:(fp)\ncall "
    "p,fp,r,37\nret r\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    unsigned calls = 0;
    auto p = linked (*c, level, {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
      ++calls;
      return Error{ErrorCode::UnknownName, "unexpected resolver call"};
    }});
    CHECK (calls == 0);
    CHECK (call (p, "b", "entry") == 42);
    auto target = p.find_function ("a", "target"), alias = p.find_function ("b", "target");
    auto pointer = p.find_data ("b", "ptr");
    REQUIRE ((target).has_value ());
    REQUIRE ((alias).has_value ());
    REQUIRE ((pointer).has_value ());
    CHECK (target.value ().address () == alias.value ().address ());
    CHECK (read<std::uintptr_t> (pointer.value ().bytes ()) == target.value ().address ());
    auto exported = p.find_export ("target");
    REQUIRE ((exported).has_value ());
    CHECK (std::get<FunctionHandle> ((exported).value ()).address () == target.value ().address ());
  }
}
TEST_CASE (
  "native anonymous label addresses differences and signed displacements retain source identity") {
  if (!native ()) return;
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("labels");
  REQUIRE ((m).has_value ());
  auto f
    = mirnext::ir::detail::access (((m)).value ()).create_function ("target", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto a = mirnext::ir::detail::access (((f)).value ()).create_block (),
       b = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  auto first = mirnext::ir::detail::access (((f)).value ()).create_label ((a).value ()),
       second = mirnext::ir::detail::access (((f)).value ()).create_label ((b).value ());
  REQUIRE ((first).has_value ());
  REQUIRE ((second).has_value ());
  REQUIRE ((InstructionBuilder ((a).value ()).ret (42)).has_value ());
  REQUIRE ((InstructionBuilder ((b).value ()).ret (7)).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ())
       .create_data ("address",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(second).value (),
                                                                           nullptr, 3)))
      .has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ())
       .create_data ("difference",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(second).value (),
                                                                           &(first).value (), -4)))
      .has_value ());
  for (unsigned level = 0; level < 4; ++level) {
    auto p = linked (c, level);
    auto function = p.find_function ("labels", "target");
    REQUIRE ((function).has_value ());
    REQUIRE ((function.value ().artifact ()).has_value ());
    // Optimization may add entry/return labels. Correspondence is to the owned
    // optimized function, not the source Builder's original label numbering.
    auto module = p.program ().find_module ("labels").get ();
    REQUIRE (module);
    auto ref = std::get<LabelReference> (
      module->find_symbol ("difference").get ()->data ().get ()->initializer ());
    std::array<std::uint64_t, 2> offsets{};
    unsigned found = 0;
    std::vector<const Label *> labels;
    for (auto &label : mirnext::ir::detail::NodeAccess::label (ref)->function ().labels ())
      labels.push_back (&label);
    for (auto &label : function.value ().artifact ().get ()->labels ())
      if (label.source_ordinal) {
        REQUIRE (*label.source_ordinal < labels.size ());
        auto source = labels[*label.source_ordinal];
        if (source == mirnext::ir::detail::NodeAccess::label (ref)) {
          offsets[1] = label.offset;
          ++found;
        }
        if (source == mirnext::ir::detail::NodeAccess::base (ref)) {
          offsets[0] = label.offset;
          ++found;
        }
      }
    CHECK (found == 2);
    auto address = p.find_data ("labels", "address"),
         difference = p.find_data ("labels", "difference");
    REQUIRE ((address).has_value ());
    REQUIRE ((difference).has_value ());
    CHECK (read<std::uint64_t> (address.value ().bytes ())
           == function.value ().address () + offsets[1] + 3);
    CHECK (read<std::uint64_t> (difference.value ().bytes ()) == offsets[1] - offsets[0] - 4);
    CHECK (call (p, "labels", "target") == 42);
  }
}
TEST_CASE (
  "native linked calls execute block returns multiple results and variadic runtime helpers") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (auto mode : {test::CallExample::Direct, test::CallExample::Indirect,
                      test::CallExample::Block, test::CallExample::ReturnBlock,
                      test::CallExample::Variadic, test::CallExample::VariadicBlock,
                      test::CallExample::MultipleResults, test::CallExample::Inline}) {
      CAPTURE (level);
      CAPTURE (static_cast<unsigned> (mode));
      auto c = test::call_example (mode);
      if (test::check_native_result_limit (*c,
                                           {.optimization = transforms::OptimizationOptions{
                                              .level = static_cast<transforms::OptimizationLevel> (
                                                level)}}))
        continue;
      auto p = linked (*c, level);
      c.reset ();
      ExecutionInvocation invocation;
      CHECK (call (p, "calls", "entry") == 42);
      CHECK ((invocation.status ()).has_value ());
      CHECK (p.statistics ().code_batches == 1);
    }
}
TEST_CASE (
  "native expression initialization uses scalar widths long double helpers and literal pools") {
  if (!native ()) return;
  auto c = parse (
    "m:module\ne:func ld\nlocal ld:r\nldadd r,1.25l,2.5l\nret r\nendfunc\nx:expr e\ns:func i8\nret "
    "-3\nendfunc\ny:expr s\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto p = linked (*c, level);
    auto x = p.find_data ("m", "x"), y = p.find_data ("m", "y");
    REQUIRE ((x).has_value ());
    REQUIRE ((y).has_value ());
    CHECK (read<long double> (x.value ().bytes ()) == 3.75L);
    CHECK (read<std::int8_t> (y.value ().bytes ()) == -3);
    CHECK (p.statistics ().expressions == 2);
  }
}
TEST_CASE ("native linking is atomic on resolver observer and publication failure") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto before = dump (*c);
  auto provider = std::make_shared<Provider> ();
  ExecutionOptions options;
  options.link.memory_provider = provider;
  options.link.compilation_observer
    = [] (auto &) -> Result<void> { return Error{ErrorCode::UnknownName, "stopped compilation"}; };
  auto failed = ExecutionSession::create (*c, options);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (provider->maps == 0);
  options.link.compilation_observer = {};
  provider->fail_write = true;
  failed = ExecutionSession::create (*c, options);
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("link:publish") != std::string::npos);
  CHECK (provider->maps == 1);
  CHECK (provider->unmaps == 1);
  provider->fail_write = false;
  {
    auto p = ExecutionSession::create (*c, options);
    REQUIRE ((p).has_value ());
    CHECK (call ((p).value (), "consumer", "entry") == 42);
    CHECK (provider->maps == 2);
    CHECK (provider->unmaps == 1);
  }
  CHECK (provider->unmaps == 2);
  CHECK (dump (*c) == before);
  auto optimized = transforms::optimize (*c);
  REQUIRE ((optimized).has_value ());
  auto p = ExecutionSession::create (std::move ((optimized).value ()));
  REQUIRE ((p).has_value ());
  CHECK_FALSE ((ExecutionSession::create (std::move ((optimized).value ()))).has_value ());
}
namespace {
std::int64_t host_increment (std::int64_t value) { return value + 5; }
}  // namespace
TEST_CASE (
  "native external calls check untyped import prototypes and special conventions before "
  "compilation") {
  if (!native ()) return;
  auto c = parse (
    "m:module\np:proto i64,i64:x\nimport ext\nf:func i64\nlocal i64:r\ncall p,ext,r,37\nret "
    "r\nendfunc\nendmodule\n");
  for (unsigned mode = 0; mode < 4; ++mode) {
    ExecutionOptions options;
    auto provider = std::make_shared<Provider> ();
    options.link.memory_provider = provider;
    options.link.resolve = [&] (auto &) -> Result<ExternalSymbol> {
      if (mode == 1)
        return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&host_increment),
                              {},
                              ExternalData{8}};
      return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&host_increment),
                            mode == 2 ? Signature{{Type (Type::Kind::F64)}, {}}
                                      : Signature{{Type::i64 ()}, {Type::i64 ()}},
                            {},
                            mode == 3};
    };
    auto p = ExecutionSession::create (*c, options);
    if (!mode) {
      INFO (((p).has_value () ? "" : p.error ().message));
      REQUIRE ((p).has_value ());
      CHECK (call ((p).value (), "m", "f") == 42);
    } else {
      REQUIRE_FALSE ((p).has_value ());
      CHECK (p.error ().message.find ("link:symbols") != std::string::npos);
      CHECK (provider->maps == 0);
    }
  }
}
