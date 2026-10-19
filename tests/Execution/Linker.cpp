/* Licensed under the MIT License. Owning linking and native execution. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Linker.hpp"
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
std::unique_ptr<Context> parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  return std::move (*c);
}
template <class T>
T read (std::span<const std::byte> bytes) {
  REQUIRE (bytes.size () >= sizeof (T));
  T value;
  std::memcpy (&value, bytes.data (), sizeof value);
  return value;
}
LinkedProgram linked (const Context &context, unsigned level = 2, LinkOptions options = {}) {
  ProgramOptions o;
  o.optimization.level = static_cast<transforms::OptimizationLevel> (level);
  o.link = std::move (options);
  auto p = LinkedProgram::create (context, std::move (o));
  INFO ((p ? "" : p.error ().message));
  REQUIRE (p);
  return std::move (*p);
}
std::int64_t call (const LinkedProgram &p, std::string_view module, std::string_view name) {
  auto f = p.find_function (module, name);
  REQUIRE (f);
  return reinterpret_cast<std::int64_t (*) ()> (f->address ()) ();
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
  Context c;
  auto m = c.create_module ("layout");
  REQUIRE (m);
  auto a = (*m)->create_data ("head", InitializedData{Type (Type::Kind::U8), {0xaa}});
  REQUIRE (a);
  auto b = (*m)->create_data ({}, InitializedData{Type (Type::Kind::U16), {0x1234}});
  REQUIRE (b);
  auto z = (*m)->create_data ({}, ZeroData{3});
  REQUIRE (z);
  auto r = (*m)->create_data ({}, SymbolReference{&(*a)->symbol (), -1});
  REQUIRE (r);
  REQUIRE ((*m)->prototype ("break", {}));
  auto q = (*m)->create_data ({}, ZeroData{0});
  REQUIRE (q);
  auto last = (*m)->create_data ("last", ZeroData{0});
  REQUIRE (last);
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  auto layout = layout_module_data (**m, *target);
  REQUIRE (layout);
  REQUIRE (layout->size () == 3);
  CHECK ((*layout)[0].size == 16);
  const std::size_t offsets[]{0, 1, 3, 6};
  for (unsigned n = 0; n < 4; ++n) CHECK ((*layout)[0].items[n].offset == offsets[n]);
  CHECK ((*layout)[1].size == 0);
  CHECK ((*layout)[2].size == 0);
  auto p = linked (c);
  CHECK (p.statistics ().sections == 3);
  CHECK (p.statistics ().code_batches == 0);
  auto head = p.find_data ("layout", "head");
  REQUIRE (head);
  CHECK (read<std::uint8_t> (head->bytes ()) == 0xaa);
  auto bytes = reinterpret_cast<const std::byte *> (head->address ());
  CHECK (read<std::uint16_t> ({bytes + 1, 2}) == 0x1234);
  CHECK (bytes[3] == std::byte{});
  CHECK (bytes[5] == std::byte{});
  CHECK (read<std::uintptr_t> ({bytes + 6, 8}) == head->address () - 1);
  CHECK_FALSE (p.find_function ("layout", "head"));
  CHECK_FALSE (p.find_data ("layout", "absent"));
  CHECK_FALSE (p.find_export ("head"));
}
TEST_CASE ("native execution session owns linked state and checks typed i64 host calls") {
  if (!native ()) return;
  auto context = test::variable_example (test::VariableExample::Initialized);
  auto session = ExecutionSession::create (*context);
  REQUIRE (session);
  auto entry = session->find_function ("consumer", "entry");
  REQUIRE (entry);
  auto result = session->call_i64 (*entry);
  REQUIRE (result);
  CHECK (*result == 42);
  auto wrong = session->call_i64 (*entry, std::array<std::int64_t, 1>{7});
  CHECK_FALSE (wrong);
  auto moved = std::move (*session);
  CHECK (moved.status ());
  CHECK_FALSE (session->status ());
  auto copied_handle = moved.find_function ("consumer", "entry");
  REQUIRE (copied_handle);
  auto again = moved.call_i64 (*copied_handle);
  REQUIRE (again);
  CHECK (*again == 64);
}
TEST_CASE ("native layout returns diagnostics before overflowing or allocating huge sections") {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Context c;
    auto m = c.create_module ("overflow");
    REQUIRE (m);
    REQUIRE ((*m)->create_data ("huge", ZeroData{mode == 1 ? SIZE_MAX - 7 : SIZE_MAX}));
    if (mode == 1) REQUIRE ((*m)->create_data ({}, ZeroData{8}));
    auto target = target::TargetMachine::native ();
    REQUIRE (target);
    CHECK_FALSE (layout_module_data (**m, *target));
    CHECK_FALSE (LinkedProgram::create (c));
  }
}
TEST_CASE ("native linked scalar variables execute at all optimization levels") {
  if (!native ()) return;
  for (unsigned level = 0; level < 4; ++level)
    for (auto mode : {test::VariableExample::Initialized, test::VariableExample::Zero,
                      test::VariableExample::Constant, test::VariableExample::Boolean,
                      test::VariableExample::Floating, test::VariableExample::Pointer,
                      test::VariableExample::Expression, test::VariableExample::CrossModule,
                      test::VariableExample::Narrow}) {
      CAPTURE (level);
      CAPTURE (static_cast<unsigned> (mode));
      auto c = test::variable_example (mode);
      auto before = dump (*c);
      auto p = linked (*c, level);
      CHECK (dump (*c) == before);
      c.reset ();
      CHECK (call (p, "consumer", "entry") == 42);
      CHECK (p.statistics ().code_batches == 1);
      CHECK (p.statistics ().expressions == (mode == test::VariableExample::Expression ? 1 : 0));
      if (mode == test::VariableExample::CrossModule) {
        auto a = p.find_data ("consumer", "value"), b = p.find_data ("definition", "value");
        REQUIRE (a);
        REQUIRE (b);
        CHECK (a->address () == b->address ());
        CHECK (read<std::int64_t> (b->bytes ()) == 42);
        CHECK (call (p, "definition", "get_value") == 42);
      }
      if (mode == test::VariableExample::Constant) {
        auto data = p.find_data ("consumer", "value");
        REQUIRE (data);
        CHECK_FALSE (data->writable_bytes ());
      }
    }
}
TEST_CASE (
  "native linked handles keep private definitions code and data alive after all producers") {
  if (!native ()) return;
  std::optional<LinkedFunction> function;
  std::optional<LinkedData> data;
  {
    auto c = test::variable_example (test::VariableExample::Initialized);
    auto p = linked (*c);
    c.reset ();
    auto f = p.find_function ("consumer", "entry");
    auto d = p.find_data ("consumer", "value");
    REQUIRE (f);
    REQUIRE (d);
    function = *f;
    data = *d;
    auto moved = std::move (p);
    CHECK_FALSE (p.find_function ("consumer", "entry"));
    CHECK (moved.find_function ("consumer", "entry"));
  }
  REQUIRE (function);
  REQUIRE (function->artifact ());
  REQUIRE (function->artifact ()->verify ());
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
  REQUIRE (a);
  REQUIRE (b);
  CHECK (a->address () != b->address ());
}
TEST_CASE (
  "native linking resolves imports once by name validates host metadata and shares bindings") {
  Context c;
  std::int64_t value = 17;
  unsigned calls = 0;
  for (auto name : {"a", "b"}) {
    auto m = c.create_module (name);
    REQUIRE (m);
    REQUIRE ((*m)->import_variable ("value", Type::i64 ()));
  }
  auto provider = std::make_shared<Provider> ();
  auto p = linked (c, 2,
                   {.resolve = [&] (const ImportRequest &request) -> Result<ExternalSymbol> {
                      ++calls;
                      CHECK (request.name == "value");
                      REQUIRE (request.variable);
                      return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&value),
                                            {},
                                            ExternalData{8, true, Type::i64 ()}};
                    },
                    .memory_provider = provider});
  CHECK (calls == 1);
  CHECK (p.statistics ().external_resolutions == 1);
  CHECK (provider->maps == 0);
  auto a = p.find_data ("a", "value"), b = p.find_data ("b", "value");
  REQUIRE (a);
  REQUIRE (b);
  CHECK (a->address () == b->address ());
  auto bytes = a->writable_bytes ();
  REQUIRE (bytes);
  std::int64_t updated = 42;
  std::memcpy (bytes->data (), &updated, 8);
  CHECK (value == 42);
  CHECK (read<std::int64_t> (b->bytes ()) == 42);
  for (unsigned mode = 0; mode < 6; ++mode) {
    ProgramOptions options;
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
    auto failed = LinkedProgram::create (c, options);
    REQUIRE_FALSE (failed);
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
    REQUIRE (target);
    REQUIRE (alias);
    REQUIRE (pointer);
    CHECK (target->address () == alias->address ());
    CHECK (read<std::uintptr_t> (pointer->bytes ()) == target->address ());
    auto exported = p.find_export ("target");
    REQUIRE (exported);
    CHECK (std::get<LinkedFunction> (*exported).address () == target->address ());
  }
}
TEST_CASE (
  "native anonymous label addresses differences and signed displacements retain source identity") {
  if (!native ()) return;
  Context c;
  auto m = c.create_module ("labels");
  REQUIRE (m);
  auto f = (*m)->create_function ("target", {{Type::i64 ()}, {}});
  REQUIRE (f);
  auto a = (*f)->create_block (), b = (*f)->create_block ();
  REQUIRE (a);
  REQUIRE (b);
  auto first = (*f)->create_label (**a), second = (*f)->create_label (**b);
  REQUIRE (first);
  REQUIRE (second);
  REQUIRE (IRBuilder (**a).ret (42));
  REQUIRE (IRBuilder (**b).ret (7));
  REQUIRE ((*m)->create_data ("address", LabelReference{*second, nullptr, 3}));
  REQUIRE ((*m)->create_data ("difference", LabelReference{*second, *first, -4}));
  for (unsigned level = 0; level < 4; ++level) {
    auto p = linked (c, level);
    auto function = p.find_function ("labels", "target");
    REQUIRE (function);
    REQUIRE (function->artifact ());
    // Optimization may add entry/return labels. Correspondence is to the owned
    // optimized function, not the source Builder's original label numbering.
    auto module = p.context ().find_module ("labels");
    REQUIRE (module);
    auto ref
      = std::get<LabelReference> (module->find_symbol ("difference")->data ()->initializer ());
    std::array<std::uint64_t, 2> offsets{};
    unsigned found = 0;
    std::vector<const Label *> labels;
    for (auto &label : ref.label->function ().labels ()) labels.push_back (&label);
    for (auto &label : function->artifact ()->labels ())
      if (label.source_ordinal) {
        REQUIRE (*label.source_ordinal < labels.size ());
        auto source = labels[*label.source_ordinal];
        if (source == ref.label) {
          offsets[1] = label.offset;
          ++found;
        }
        if (source == ref.base) {
          offsets[0] = label.offset;
          ++found;
        }
      }
    CHECK (found == 2);
    auto address = p.find_data ("labels", "address"),
         difference = p.find_data ("labels", "difference");
    REQUIRE (address);
    REQUIRE (difference);
    CHECK (read<std::uint64_t> (address->bytes ()) == function->address () + offsets[1] + 3);
    CHECK (read<std::uint64_t> (difference->bytes ()) == offsets[1] - offsets[0] - 4);
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
      CHECK (invocation.status ());
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
    REQUIRE (x);
    REQUIRE (y);
    CHECK (read<long double> (x->bytes ()) == 3.75L);
    CHECK (read<std::int8_t> (y->bytes ()) == -3);
    CHECK (p.statistics ().expressions == 2);
  }
}
TEST_CASE ("native linking is atomic on resolver observer and publication failure") {
  if (!native ()) return;
  auto c = test::variable_example (test::VariableExample::Initialized);
  auto before = dump (*c);
  auto provider = std::make_shared<Provider> ();
  ProgramOptions options;
  options.link.memory_provider = provider;
  options.link.compilation_observer
    = [] (auto &) -> Result<void> { return Error{ErrorCode::UnknownName, "stopped compilation"}; };
  auto failed = LinkedProgram::create (*c, options);
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (provider->maps == 0);
  options.link.compilation_observer = {};
  provider->fail_write = true;
  failed = LinkedProgram::create (*c, options);
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().message.find ("link:publish") != std::string::npos);
  CHECK (provider->maps == 1);
  CHECK (provider->unmaps == 1);
  provider->fail_write = false;
  {
    auto p = LinkedProgram::create (*c, options);
    REQUIRE (p);
    CHECK (call (*p, "consumer", "entry") == 42);
    CHECK (provider->maps == 2);
    CHECK (provider->unmaps == 1);
  }
  CHECK (provider->unmaps == 2);
  CHECK (dump (*c) == before);
  auto optimized = transforms::optimize (*c);
  REQUIRE (optimized);
  auto p = LinkedProgram::link (std::move (*optimized));
  REQUIRE (p);
  CHECK_FALSE (LinkedProgram::link (std::move (*optimized)));
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
    ProgramOptions options;
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
    auto p = LinkedProgram::create (*c, options);
    if (!mode) {
      INFO ((p ? "" : p.error ().message));
      REQUIRE (p);
      CHECK (call (*p, "m", "f") == 42);
    } else {
      REQUIRE_FALSE (p);
      CHECK (p.error ().message.find ("link:symbols") != std::string::npos);
      CHECK (provider->maps == 0);
    }
  }
}
