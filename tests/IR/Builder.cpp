/* Licensed under the MIT License. Construction syntax and failure boundaries. */
#include "mirnext/IR/Construction.hpp"
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <array>
#include <limits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;

namespace {
template <class Input>
Result<ExecutionSession> load_fixture (Input &&input, ExecutionOptions options = {}) {
  if constexpr (std::is_same_v<std::remove_cvref_t<Input>, ProgramBuilder>) {
    auto program = input.finish ();
    if (!program.has_value ()) return program.error ();
    return ExecutionSession::create (std::move (program).value (), std::move (options));
  } else
    return ExecutionSession::create (std::forward<Input> (input), std::move (options));
}
ProgramBuilder arithmetic () {
  ProgramBuilder program;
  auto module = program.module ("example");
  auto add = module.function<std::int64_t (std::int64_t)> ("add_one");
  add.ret (add.arg (0) + 1);
  auto twice = module.function<std::int64_t (std::int64_t)> ("twice");
  // Sequence construction explicitly: operand evaluation order differs between compilers.
  auto first = twice.call (add, {twice.arg (0)});
  auto second = twice.call (add, {twice.arg (0)});
  twice.ret (first + second);
  return program;
}
ExecutionOptions options (unsigned engine) {
  ExecutionOptions o;
  if (engine == 0) o.mode = ExecutionMode::Interpreter;
  if (engine == 2) o.link.compilation = JITCompilation::Lazy;
  if (engine == 3) o.link.compilation = JITCompilation::BlockVersioning;
  if (engine == 4)
    o.select_engine = [] (const Function &f) -> Result<ExecutionEngine> {
      return f.symbol ().name () == "add_one" ? ExecutionEngine::Interpreter
                                              : ExecutionEngine::LazyJIT;
    };
  return o;
}
}  // namespace

TEST_CASE ("friendly builder composes expressions and calls in every engine") {
  for (unsigned engine = 0; engine < 5; ++engine) {
    CAPTURE (engine);
    auto session = load_fixture (arithmetic (), options (engine));
    INFO (((session).has_value () ? "" : session.error ().message));
    REQUIRE ((session).has_value ());
    auto result = session.value ().call_i64 ("example", "twice", {20});
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    CHECK ((result).value () == 42);
    auto f = session.value ().find_function ("example", "add_one");
    REQUIRE ((f).has_value ());
    CHECK ((f.value ().call_i64 ({41})).value () == 42);
    CHECK ((session.value ().call_i64 ((f).value (), {41})).value () == 42);
    CHECK_FALSE ((session.value ().call_i64 ("example", "missing", {41})).has_value ());
    CHECK_FALSE ((session.value ().call_i64 ("example", "add_one", {})).has_value ());
    CHECK_FALSE ((session.value ().call_double ("example", "add_one", {41.0})).has_value ());
  }
}
TEST_CASE ("friendly builder preserves the checked builder IR and wire formats") {
  auto built = arithmetic ().finish ();
  REQUIRE ((built).has_value ());
  Program reference;
  auto m = mirnext::ir::detail::access (reference).create_module ("example");
  REQUIRE ((m).has_value ());
  auto add = mirnext::ir::detail::access (((m)).value ())
               .create_function ("add_one", signature<std::int64_t (std::int64_t)> ());
  REQUIRE ((add).has_value ());
  auto block = mirnext::ir::detail::access (((add)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder b ((block).value ());
  REQUIRE ((b.ret (b.expression (
                     *mirnext::ir::detail::ReferenceAccess::pointers (add.value ().arguments ())[0])
                   + 1))
             .has_value ());
  auto twice = mirnext::ir::detail::access (((m)).value ())
                 .create_function ("twice", signature<std::int64_t (std::int64_t)> ());
  REQUIRE ((twice).has_value ());
  block = mirnext::ir::detail::access (((twice)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder caller ((block).value ());
  auto a = caller.call ((add).value (), {*mirnext::ir::detail::ReferenceAccess::pointers (
                                          twice.value ().arguments ())[0]});
  REQUIRE ((a).has_value ());
  auto c = caller.call ((add).value (), {*mirnext::ir::detail::ReferenceAccess::pointers (
                                          twice.value ().arguments ())[0]});
  REQUIRE ((c).has_value ());
  REQUIRE ((caller.ret (caller.expression ((a.value ().value ()).value ())
                        + caller.expression ((c.value ().value ()).value ())))
             .has_value ());
  CHECK (dump ((built).value ()) == dump (reference));
  auto text = serialization::write_text ((built).value ());
  REQUIRE ((text).has_value ());
  auto parsed = serialization::parse_text ((text).value ());
  REQUIRE ((parsed).has_value ());
  auto normalized_text = serialization::write_text ((parsed).value ());
  REQUIRE ((normalized_text).has_value ());
  CHECK ((normalized_text).value () == (text).value ());
  auto wire = serialization::write_binary ((built).value ());
  REQUIRE ((wire).has_value ());
  auto old_wire = serialization::write_binary (reference);
  REQUIRE ((old_wire).has_value ());
  CHECK ((wire).value () == (old_wire).value ());
  auto decoded = serialization::read_binary ((wire).value ());
  REQUIRE ((decoded).has_value ());
  auto normalized_wire = serialization::write_binary ((decoded).value ());
  REQUIRE ((normalized_wire).has_value ());
  CHECK ((normalized_wire).value () == (wire).value ());
}
TEST_CASE ("builder handles and expressions survive copies and owner moves") {
  ProgramBuilder source;
  auto m = source.module ("move");
  auto f = m.function<std::int64_t (std::int64_t)> ("f");
  auto expression = f.arg (0) + 1;
  auto copy = f;
  ProgramBuilder destination (std::move (source));
  CHECK_FALSE ((source.finish ()).has_value ());
  CHECK_FALSE (source.error ().value ().message.empty ());
  copy.ret (expression);
  auto session = load_fixture (std::move (destination), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("move", "f", {41})).value () == 42);
  auto retained = session.value ().find_function ("move", "f");
  REQUIRE ((retained).has_value ());
  {
    auto owned = std::move ((session).value ());
    CHECK_FALSE ((session.value ().call_i64 ("move", "f", {41})).has_value ());
  }
  CHECK ((retained.value ().call_i64 ({41})).value () == 42);
  CHECK_FALSE (expression.is_valid ());
  copy.ret (expression + 1);
  CHECK ((destination.error ()).has_value ());
  CHECK_FALSE ((destination.finish ()).has_value ());
  CHECK ((retained.value ().call_i64 ({41})).value () == 42);
}
TEST_CASE ("first builder error survives every following construction operation") {
  ProgramBuilder p;
  auto m = p.module ("bad");
  auto f = m.function<std::int64_t ()> ("f");
  SUBCASE ("duplicate function") { m.function<void ()> ("f"); }
  SUBCASE ("argument range") { f.arg (0); }
  SUBCASE ("wrong return type") { f.ret (f.literal (1.5)); }
  SUBCASE ("cross function value") {
    auto g = m.function<std::int64_t (std::int64_t)> ("g");
    f.ret (g.arg (0));
  }
  SUBCASE ("cross program symbol") {
    auto other = arithmetic ();
    auto other_module = other.module ("other");
    f.call (other_module.import<void ()> ("external"));
  }
  SUBCASE ("cross function block") {
    auto g = m.function<void ()> ("g");
    f.jump (g.entry ());
  }
  SUBCASE ("invalid block size") { f.block_arg (f.allocate_stack (8), Type::block (0)); }
  SUBCASE ("readonly binding") {
    auto v = m.variable ("constant", Type::i64 (), 42, false);
    f.store (v, 0);
  }
  SUBCASE ("void scalar call") {
    auto v = m.function<void ()> ("void");
    v.ret ();
    f.ret (f.call (v));
  }
  SUBCASE ("multiple scalar call") {
    auto v = m.function ("multi", {{Type::i64 (), Type::i64 ()}, {}});
    v.ret ({1, 2});
    f.ret (f.call (v));
  }
  SUBCASE ("call result range") {
    auto v = m.function<std::int64_t ()> ("one");
    v.ret (1);
    f.ret (f.call (v).result (1));
  }
  SUBCASE ("wrong argument") {
    auto v = m.import<std::int64_t (double)> ("external");
    f.call (v, {f.literal (1)});
  }
  SUBCASE ("closed block") {
    f.ret (1);
    f.ret (2);
  }
  SUBCASE ("invalid opcode operands") { f.emit (Opcode::Add, {1}); }
  REQUIRE ((p.error ()).has_value ());
  auto first = p.error ().value ().message;
  auto invalid = f.arg (9) + 2;
  f.ret (invalid);
  f.store (f.memory (Type::i64 (), invalid), invalid);
  f.call (f, {invalid});
  f.position_at (f.block ());
  m.variable ("after", Type::i64 ());
  p.module ("after");
  auto finished = p.finish ();
  REQUIRE_FALSE ((finished).has_value ());
  CHECK (finished.error ().message == first);
  auto session = load_fixture (std::move (p));
  REQUIRE_FALSE ((session).has_value ());
  CHECK (session.error ().message == first);
}
TEST_CASE ("failed module and function handles safely propagate the original error") {
  ProgramBuilder p;
  auto m = p.module ("");
  auto f = m.function<std::int64_t ()> ("f");
  auto v = f.load (m.variable ("v", Type::i64 ()));
  f.ret (f.call (f, {v + 1}).result (0));
  f.end_stack_scope (f.begin_stack_scope ());
  REQUIRE ((p.error ()).has_value ());
  auto r = p.finish ();
  REQUIRE_FALSE ((r).has_value ());
  CHECK (r.error ().message == p.error ().value ().message);
}
TEST_CASE ("finish diagnoses unresolved functions and incomplete control flow") {
  ProgramBuilder p;
  auto m = p.module ("verify");
  SUBCASE ("unresolved declaration") { m.declare_function<void ()> ("f"); }
  SUBCASE ("missing return") { m.function<std::int64_t ()> ("f"); }
  auto r = p.finish ();
  CHECK_FALSE ((r).has_value ());
  CHECK ((p.error ()).has_value ());
}
TEST_CASE ("declarations imports globals and indirect calls share canonical symbols") {
  for (unsigned engine = 0; engine < 5; ++engine) {
    ProgramBuilder p;
    auto a = p.module ("a");
    auto declaration = a.declare_function<std::int64_t (std::int64_t)> ("step");
    auto f = a.define_function (declaration);
    f.ret (f.arg (0) + 2);
    a.export_symbol (f);
    auto b = p.module ("b");
    auto imported = b.import<std::int64_t (std::int64_t)> ("step");
    auto target = b.variable ("target", Type::pointer (), imported);
    auto count = b.variable<std::int64_t> ("count", 0);
    auto entry = b.function<std::int64_t ()> ("entry");
    entry.store (count, entry.load (count) + 1);
    entry.ret (entry.call_indirect (imported, entry.load (target), {40}));
    auto session = load_fixture (std::move (p), options (engine));
    INFO (((session).has_value () ? "" : session.error ().message));
    REQUIRE ((session).has_value ());
    CHECK ((session.value ().call_i64 ("b", "entry")).value () == 42);
    CHECK ((session.value ().call_i64 ("b", "entry")).value () == 42);
  }
}
TEST_CASE ("block parameters dynamic stack scopes and explicit saves are ergonomic") {
  for (unsigned engine = 0; engine < 4; ++engine) {
    ProgramBuilder p;
    auto m = p.module ("blocks");
    auto sum = m.function ("sum", {{Type::i64 ()}, {Type::block (16)}});
    auto address = sum.arg (0);
    sum.ret (sum.load (sum.memory (Type::i64 (), address))
             + sum.load (sum.memory (Type::i64 (), address, 8)));
    auto f = m.function<std::int64_t ()> ("entry");
    auto scope = f.begin_stack_scope ();
    auto saved = f.stack_save ();
    auto buffer = f.allocate_stack (16);
    f.store (f.memory (Type::i64 (), buffer), 20);
    f.store (f.memory (Type::i64 (), buffer, 8), 22);
    auto answer = f.call (sum, {f.block_arg (buffer, Type::block (16))});
    f.stack_restore (saved);
    f.ret (answer);
    f.end_stack_scope (scope);
    auto session = load_fixture (std::move (p), options (engine));
    INFO (((session).has_value () ? "" : session.error ().message));
    REQUIRE ((session).has_value ());
    CHECK ((session.value ().call_i64 ("blocks", "entry")).value () == 42);
  }
}
TEST_CASE ("multiple results and void calls require no scalar extraction boilerplate") {
  ProgramBuilder p;
  auto m = p.module ("multi");
  auto pair = m.function ("pair", {{Type::i64 (), Type::i64 ()}, {Type::i64 ()}});
  pair.ret ({pair.arg (0), pair.arg (0) + 2});
  auto none = m.function<void ()> ("none");
  none.ret ();
  auto f = m.function<std::int64_t ()> ("entry");
  f.call (none);
  auto both = f.call (pair, {20});
  f.ret (both.result (0) + both.result (1));
  auto session = load_fixture (std::move (p), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("multi", "entry")).value () == 42);
}
TEST_CASE ("explicit blocks preserve conditional fallthrough switch and label identities") {
  ProgramBuilder p;
  auto m = p.module ("cfg");
  auto f = m.function<std::int64_t (std::int64_t)> ("choose");
  auto fallthrough = f.block ("fallthrough"), yes = f.block ("yes");
  f.branch_if (f.arg (0) == 0, yes);
  f.position_at (fallthrough);
  f.ret (41);
  f.position_at (yes);
  auto first = f.label (yes, "first"), second = f.label (yes, "second");
  f.address_of (first);
  f.address_of (second);
  f.ret (42);
  auto s = m.function<std::int64_t (std::int64_t)> ("switch");
  auto a = s.block (), b = s.block ();
  s.switch_to (s.arg (0), {a, b});
  s.position_at (a);
  s.ret (40);
  s.position_at (b);
  s.ret (42);
  auto session = load_fixture (std::move (p), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("cfg", "choose", {0})).value () == 42);
  CHECK ((session.value ().call_i64 ("cfg", "choose", {1})).value () == 41);
  CHECK ((session.value ().call_i64 ("cfg", "switch", {1})).value () == 42);
}
TEST_CASE ("narrow unsigned boolean and floating signatures retain explicit types") {
  auto narrow = signature<std::uint8_t (std::int32_t, bool, double, void *)> ();
  CHECK (narrow.results[0] == Type (Type::Kind::U8));
  CHECK (narrow.parameters
         == std::vector<Type>{Type (Type::Kind::I32), Type (Type::Kind::Bool),
                              Type (Type::Kind::F64), Type::pointer ()});
  ProgramBuilder p;
  auto m = p.module ("types");
  auto u = m.function<std::uint64_t (std::uint64_t)> ("unsigned");
  u.ret (u.arg (0) + 1);
  auto d = m.function<double (double)> ("double");
  d.ret (d.arg (0) * 2.0);
  auto b = m.function<bool (bool)> ("bool");
  b.ret (!b.arg (0));
  auto i = m.function<std::int32_t (std::int32_t)> ("i32");
  i.ret (i.arg (0) + 1);
  auto session = load_fixture (std::move (p), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK (
    (session.value ().call_u64 ("types", "unsigned", {std::numeric_limits<std::uint64_t>::max ()}))
      .value ()
    == 0);
  CHECK ((session.value ().call_double ("types", "double", {21.0})).value () == 42.0);
  CHECK ((session.value ().call_i64 ("types", "bool", {0})).value () == 1);
  CHECK ((session.value ().call_i64 ("types", "i32", {41})).value () == 42);
}
TEST_CASE ("varargs overflow and exact opcode selection use the same error boundary") {
  for (unsigned engine = 0; engine < 4; ++engine) {
    ProgramBuilder p;
    auto m = p.module ("advanced");
    auto va = m.function ("variadic", Signature{{Type::i64 ()}, {Type::i64 ()}, true});
    auto list = va.allocate_stack (64);
    va.start_varargs (list);
    auto next = va.next_vararg (Type::i64 (), list);
    va.end_varargs (list);
    va.ret (va.arg (0) + next);
    auto entry = m.function<std::int64_t ()> ("entry");
    entry.ret (entry.call (va, {40, 2}));
    auto over = m.function<std::int64_t (std::int64_t, std::int64_t)> ("overflow");
    auto normal = over.block (), overflow = over.block ();
    auto answer = over.binary (Opcode::Addo, over.arg (0), over.arg (1));
    over.emit (Opcode::Bo, {over.label (overflow)});
    over.position_at (normal);
    over.ret (answer);
    over.position_at (overflow);
    over.ret (99);
    // More than four operands exercises the conversion buffer's heap path.
    auto many = m.function<std::int64_t (std::int64_t)> ("many");
    auto a = many.block (), b = many.block (), c = many.block (), d = many.block (),
         e = many.block ();
    many.emit (Opcode::Switch, {many.arg (0), many.label (a), many.label (b), many.label (c),
                                many.label (d), many.label (e)});
    for (auto block : {a, b, c, d, e}) {
      many.position_at (block);
      many.ret (42);
    }
    auto session = load_fixture (std::move (p), options (engine));
    INFO (((session).has_value () ? "" : session.error ().message));
    REQUIRE ((session).has_value ());
    CHECK ((session.value ().call_i64 ("advanced", "entry")).value () == 42);
    CHECK ((session.value ().call_i64 ("advanced", "overflow", {40, 2})).value () == 42);
    CHECK ((session.value ().call_i64 ("advanced", "overflow", {INT64_MAX, 1})).value () == 99);
    CHECK ((session.value ().call_i64 ("advanced", "many", {4})).value () == 42);
  }
}
TEST_CASE ("nested scopes loop edges and copied insertion positions retain their semantics") {
  for (unsigned engine = 0; engine < 4; ++engine) {
    ProgramBuilder p;
    auto m = p.module ("loop");
    auto f = m.function<std::int64_t (std::int64_t)> ("sum");
    auto outer = f.begin_stack_scope ();
    auto total = f.local (Type::i64 ());
    f.assign (total, 0);
    auto index = f.local (Type::i64 ());
    f.assign (index, 0);
    auto test = f.block (), body = f.block (), exit = f.block ();
    f.jump (test);
    f.position_at (test);
    f.branch (Comparison::GreaterEqual, index, f.arg (0), exit);
    f.position_at (body);
    auto inner = f.begin_stack_scope ();
    auto slot = f.allocate_stack (8);
    f.store (f.memory (Type::i64 (), slot), index);
    f.assign (total, total + f.load (f.memory (Type::i64 (), slot)));
    f.assign (index, index + 1);
    f.end_stack_scope (inner);
    f.jump (test);
    f.position_at (exit);
    f.ret (total);
    f.end_stack_scope (outer);
    auto session = load_fixture (std::move (p), options (engine));
    INFO (((session).has_value () ? "" : session.error ().message));
    REQUIRE ((session).has_value ());
    CHECK ((session.value ().call_i64 ("loop", "sum", {0})).value () == 0);
    CHECK ((session.value ().call_i64 ("loop", "sum", {5})).value () == 10);
  }
}
TEST_CASE ("block ABI classifications and implicit return buffers survive encoding") {
  ProgramBuilder p;
  auto m = p.module ("abi");
  auto f = m.function<void ()> ("entry");
  auto buffer = f.allocate_stack (32);
  for (unsigned cls = 0; cls < 5; ++cls) {
    auto type = Type::block (16, static_cast<Type::BlockClass> (cls));
    auto imported = m.import ("class" + std::to_string (cls), Signature{{}, {type}});
    f.call (imported, {f.block_arg (buffer, type)});
  }
  auto returned = m.import ("returned", Signature{{}, {Type::return_block (32)}});
  f.call (returned, {f.return_buffer (buffer, 32)});
  f.ret ();
  auto context = p.finish ();
  REQUIRE ((context).has_value ());
  auto binary = serialization::write_binary ((context).value ());
  REQUIRE ((binary).has_value ());
  auto read = serialization::read_binary ((binary).value ());
  REQUIRE ((read).has_value ());
  auto encoded = serialization::write_binary ((read).value ());
  REQUIRE ((encoded).has_value ());
  CHECK ((encoded).value () == (binary).value ());
  unsigned blocks = 0, returns = 0;
  for (auto &module : read.value ().modules ())
    for (auto &function : module.functions ())
      for (auto &block : function.blocks ())
        for (auto &instruction : block.instructions ()) {
          if (instruction.opcode () != Opcode::Call) continue;
          auto memory
            = mirnext::ir::detail::operand_access (instruction.operands ().back ()).get<Memory> ();
          REQUIRE (memory);
          if (memory->type.kind () == Type::Kind::ReturnBlock) {
            ++returns;
            CHECK (memory->type.block_size () == 32);
          } else {
            CHECK (static_cast<unsigned> (memory->type.block_class ()) == blocks++);
            CHECK (memory->type.block_size () == 16);
          }
        }
  CHECK (blocks == 5);
  CHECK (returns == 1);
}
TEST_CASE ("hardware aliases share a register identity in the user builder") {
  ProgramBuilder p;
  auto m = p.module ("hardware");
  auto f = m.function<std::int64_t ()> ("entry");
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto binding = target.value ().architecture () == target::Architecture::X86_64 ? "rax" : "r5";
  auto first = f.local (Type::i64 (), "first", binding);
  auto alias = f.local (Type::i64 (), "alias", binding);
  f.assign (first, 42);
  f.assign (alias, 0);
  f.ret (first);
  auto context = p.finish ();
  REQUIRE ((context).has_value ());
  const auto &function = *context.value ().modules ().begin ()->functions ().begin ();
  CHECK (std::ranges::distance (function.registers ()) == 1);
  auto session = load_fixture ((context).value (), {.mode = ExecutionMode::Interpreter});
  INFO (((session).has_value () ? "" : session.error ().message));
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("hardware", "entry")).value () == 0);
}

TEST_CASE ("moving a program preserves nodes interned types and module ownership") {
  Program source;
  auto module = mirnext::ir::detail::access (source).create_module ("stable");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", signature<std::int64_t ()> ());
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  REQUIRE ((InstructionBuilder ((block).value ()).ret (42)).has_value ());
  auto signature = source.intern_signature (function.value ().signature ());
  REQUIRE ((signature).has_value ());
  auto identity = &(module).value ();
  auto fn_identity = &(function).value ();
  auto signature_identity = &(signature).value ();
  Program destination (std::move (source));
  CHECK (destination.find_module ("stable").get () == identity);
  CHECK (&identity->program () == &destination);
  CHECK (identity->find_symbol ("entry").get ()->function ().get () == fn_identity);
  auto interned = destination.intern_signature (fn_identity->signature ());
  REQUIRE ((interned).has_value ());
  CHECK (&(interned).value () == signature_identity);
  CHECK ((destination.verify ()).has_value ());
  Program assigned;
  assigned = std::move (destination);
  CHECK (&identity->program () == &assigned);
  CHECK ((assigned.verify ()).has_value ());
  auto session = load_fixture (assigned, {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("stable", "entry")).value () == 42);
}

TEST_CASE ("builder module data uses typed references and preserves declaration layout") {
  ProgramBuilder builder;
  auto m = builder.module ("data");
  auto f = m.function<std::int64_t ()> ("initializer");
  auto anchor = f.label (f.entry (), "entry");
  f.ret (42);
  auto declared = m.declare_data ("values");
  auto values = m.define_data (declared, DataInput::values (Type::i64 (), {20}), false);
  m.anonymous_data (DataInput::zero (8));
  m.data ("pointer", DataInput::reference (values, 8));
  m.data ("label", DataInput::label (anchor, 4));
  m.data ("difference", DataInput::difference (anchor, anchor, 3));
  m.data ("expression", DataInput::expression (f));
  auto bound = m.bind_variable (values, Type::i64 (), false);
  auto read = m.function<std::int64_t ()> ("read");
  read.ret (read.load (bound));
  m.rename_symbol (values, "renamed");
  auto program = builder.finish ();
  INFO (((program).has_value () ? "" : program.error ().message));
  REQUIRE ((program).has_value ());
  REQUIRE ((values.get ()).has_value ());
  CHECK (values.get ().value ().symbol ().name () == "renamed");
  auto module = program.value ().find_module ("data").get ();
  REQUIRE (module);
  CHECK (std::ranges::distance (module->data ()) == 6);
  auto bytes = serialization::write_binary ((program).value ());
  REQUIRE ((bytes).has_value ());
  auto decoded = serialization::read_binary ((bytes).value ());
  REQUIRE ((decoded).has_value ());
  auto session
    = load_fixture (std::move ((decoded).value ()), {.mode = ExecutionMode::Interpreter});
  INFO (((session).has_value () ? "" : session.error ().message));
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("data", "read")).value () == 20);
}

TEST_CASE ("foreign data input poisons construction before any later append") {
  ProgramBuilder first, second;
  auto a = first.module ("a"), b = second.module ("b");
  auto source = a.data ("source", DataInput::zero (8));
  auto invalid = b.data ("foreign", DataInput::reference (source));
  REQUIRE ((second.error ()).has_value ());
  auto diagnostic = second.error ().value ().message;
  b.data ("after", DataInput::zero (8));
  CHECK_FALSE ((invalid.get ()).has_value ());
  auto result = second.finish ();
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message == diagnostic);
}

TEST_CASE ("program editor records first failure preserves earlier edits and blocks compilation") {
  auto built = arithmetic ().finish ();
  REQUIRE ((built).has_value ());
  auto &function = *built.value ().modules ().begin ()->functions ().begin ();
  const auto &ret = *std::prev (function.blocks ().begin ()->instructions ().end ());
  auto before = function.instruction_count ();
  {
    ProgramEditor editor ((built).value ());
    auto edit = editor.edit (function);
    edit.replace_operand (ret, 0, 42);
    CHECK ((ret.operands ()[0].get<std::int64_t> ()).value () == 42);
    edit.replace_operand (ret, 20, 1);
    REQUIRE ((editor.error ()).has_value ());
    auto diagnostic = editor.error ().value ().message;
    edit.emit (Opcode::Ret, {99});
    edit.replace_operand (ret, 0, 99);
    CHECK (function.instruction_count () == before);
    CHECK ((ret.operands ()[0].get<std::int64_t> ()).value () == 42);
    auto done = editor.finish ();
    REQUIRE_FALSE ((done).has_value ());
    CHECK (done.error ().message == diagnostic);
  }
  CHECK_FALSE ((built.value ().verify ()).has_value ());
  CHECK_FALSE ((load_fixture ((built).value ())).has_value ());
}

TEST_CASE ("successful editor survives owner movement and invalidates erased handles") {
  auto built = arithmetic ().finish ();
  REQUIRE ((built).has_value ());
  auto &function = *built.value ().modules ().begin ()->functions ().begin ();
  auto &block = *function.blocks ().begin ();
  auto &ret = *std::prev (block.instructions ().end ());
  ProgramEditor editor ((built).value ());
  auto edit = editor.edit (function);
  auto copied = edit;
  auto dead = copied.instruction (*block.instructions ().begin ());
  REQUIRE ((dead.get ()).has_value ());
  copied.replace_operand (ret, 0, 42);
  // The former definition is no longer used after the replacement.
  edit.erase ((dead.get ()).value ());
  CHECK_FALSE ((dead.get ()).has_value ());
  auto moved = std::move ((built).value ());
  REQUIRE ((editor.finish ()).has_value ());
  REQUIRE ((moved.verify ()).has_value ());
  auto session = load_fixture (std::move (moved), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("example", "add_one", {3})).value () == 42);
}

TEST_CASE ("discarded unfinished edit batch remains a program diagnostic") {
  auto built = arithmetic ().finish ();
  REQUIRE ((built).has_value ());
  {
    ProgramEditor editor ((built).value ());
  }
  auto checked = built.value ().verify ();
  REQUIRE_FALSE ((checked).has_value ());
  CHECK (checked.error ().message.find ("not finished") != std::string::npos);
  ProgramEditor next ((built).value ());
  auto done = next.finish ();
  REQUIRE_FALSE ((done).has_value ());
  CHECK (done.error ().message == checked.error ().message);
}

TEST_CASE ("SSA editors construct and finish a phase through the program edit batch") {
  ProgramBuilder builder;
  auto function = builder.module ("ssa").function<std::int64_t (std::int64_t)> ("entry");
  function.ret (function.arg (0) + 1);
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  auto &body = *program.value ().modules ().begin ()->functions ().begin ();
  auto &block = *body.blocks ().begin ();
  auto &add = *block.instructions ().begin ();
  auto &ret = *std::prev (block.instructions ().end ());
  auto output = mirnext::ir::detail::operand_access (add.operands ()[0]).get<const Register *> ();
  REQUIRE (output);
  ProgramEditor edit ((program).value ());
  auto ssa = edit.edit (body).create_ssa ();
  auto initial = ssa.initial (body.arguments ()[0], SSAValue::Kind::Entry);
  auto definition = ssa.definition (add, 0);
  ssa.input (add, 1, SSAOperandPart::Register, initial);
  ssa.input (ret, 0, SSAOperandPart::Register, definition);
  ssa.mark_renamed ();
  ssa.mark_conventional ();
  REQUIRE ((definition.get ()).has_value ());
  REQUIRE ((edit.finish ()).has_value ());
  REQUIRE ((body.ssa_form ()).has_value ());
  REQUIRE ((body.ssa_form ().value ().verify ()).has_value ());
  CHECK ((definition.get ()).has_value ());
  CHECK (&definition.get ().value ().form () == &(body.ssa_form ()).value ());
  ProgramEditor exit ((program).value ());
  auto existing = exit.edit (body).ssa ();
  existing.complete ();
  REQUIRE ((exit.finish ()).has_value ());
  CHECK_FALSE ((body.ssa_form ()).has_value ());
  CHECK_FALSE ((definition.get ()).has_value ());
  auto previous_phase = std::uint64_t (0);
  for (unsigned round = 0; round < 16; ++round) {
    analysis::FunctionAnalysisManager analyses (body);
    transforms::FunctionPassManager passes;
    passes.add<transforms::BuildSSAPass> ();
    passes.add<transforms::ConventionalSSAPass> ();
    auto built_phase = passes.run (body, analyses);
    INFO (((built_phase).has_value () ? "" : built_phase.error ().message));
    REQUIRE ((built_phase).has_value ());
    auto phase = body.ssa_form ().value ().phase_identity ();
    CHECK (phase > previous_phase);
    previous_phase = phase;
    CHECK_FALSE ((definition.get ()).has_value ());
    ProgramEditor next ((program).value ());
    next.edit (body).ssa ().complete ();
    auto completed = next.finish ();
    INFO (((completed).has_value () ? "" : completed.error ().message));
    REQUIRE ((completed).has_value ());
    CHECK_FALSE ((definition.get ()).has_value ());
  }
  auto session
    = load_fixture (std::move ((program).value ()), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("ssa", "entry", {41})).value () == 42);
}
TEST_CASE (
  "deleted block and label handles report diagnostics without dereferencing removed nodes") {
  ProgramBuilder builder;
  auto function = builder.module ("erase").function<std::int64_t ()> ("entry");
  function.ret (42);
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  auto &body = *program.value ().modules ().begin ()->functions ().begin ();
  ProgramEditor editor ((program).value ());
  auto edit = editor.edit (body);
  auto dead_block = edit.block ("unreachable");
  auto dead_label = edit.label (dead_block, "unused");
  edit.position_at (dead_block);
  edit.ret (0);
  edit.erase_blocks (std::array{dead_block});
  CHECK_FALSE ((dead_block.get ()).has_value ());
  CHECK_FALSE ((dead_label.get ()).has_value ());
  REQUIRE ((editor.finish ()).has_value ());
  REQUIRE ((program.value ().verify ()).has_value ());
}

TEST_CASE ("operand queries return typed optional references without pointer indirection") {
  auto built = arithmetic ().finish ();
  REQUIRE ((built).has_value ());
  auto module = built.value ().find_module ("example");
  REQUIRE ((module).has_value ());
  auto function = module.value ().find_symbol ("add_one").value ().function ();
  REQUIRE ((function).has_value ());
  auto &argument = function.value ().arguments ().front ();
  Operand value (argument);
  auto reg = value.get<Register> ();
  REQUIRE ((reg).has_value ());
  CHECK (&(reg).value () == &argument);
  CHECK_FALSE ((value.get<Label> ()).has_value ());
  CHECK_FALSE ((value.get<std::int64_t> ()).has_value ());
  Operand literal (42);
  auto integer = literal.get<std::int64_t> ();
  REQUIRE ((integer).has_value ());
  CHECK ((integer).value () == 42);
  CHECK_FALSE ((literal.get<Register> ()).has_value ());
  Program moved (std::move ((built).value ()));
  CHECK_FALSE ((built.value ().find_module ("example")).has_value ());
  CHECK (&*moved.find_module ("example").value ().functions ().begin () == &(function).value ());
}

TEST_CASE ("handles from completed build and edit batches detect later node deletion") {
  ProgramBuilder builder;
  auto function = builder.module ("lifetime").function<std::int64_t ()> ("entry");
  auto temporary = function.local (Type::i64 (), "temporary");
  auto instruction = function.emit (Opcode::Mov, {temporary, 1});
  function.ret (42);
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  REQUIRE ((instruction.get ()).has_value ());
  ProgramEditor edit ((program).value ());
  auto rewritten = edit.edit (
    (program.value ().find_module ("lifetime").value ().find_symbol ("entry").value ().function ())
      .value ());
  rewritten.erase (instruction);
  REQUIRE ((edit.finish ()).has_value ());
  CHECK_FALSE ((instruction.get ()).has_value ());
  CHECK (instruction.get ().error ().message.find ("erased") != std::string::npos);
  REQUIRE ((program.value ().verify ()).has_value ());
  ProgramEditor next ((program).value ());
  auto again = next.edit (
    (program.value ().find_module ("lifetime").value ().find_symbol ("entry").value ().function ())
      .value ());
  auto ret = again.instruction (*program.value ()
                                   .find_module ("lifetime")
                                   .value ()
                                   .find_symbol ("entry")
                                   .value ()
                                   .function ()
                                   .value ()
                                   .blocks ()
                                   .begin ()
                                   ->instructions ()
                                   .begin ());
  again.insert_before (ret, Opcode::Mov,
                       {again.value ((temporary.get ().value ().get<Register> ()).value ()), 7});
  REQUIRE ((next.finish ()).has_value ());
  CHECK_FALSE ((instruction.get ()).has_value ());
}

TEST_CASE ("SSA entry predecessors are typed optional references") {
  ProgramBuilder builder;
  auto function = builder.module ("entry_phi").function<std::int64_t (std::int64_t)> ("identity");
  function.ret (function.arg (0));
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  auto &body = *program.value ().find_module ("entry_phi").value ().functions ().begin ();
  auto &block = *body.blocks ().begin ();
  auto &ret = *block.instructions ().begin ();
  ProgramEditor editor ((program).value ());
  auto ssa = editor.edit (body).create_ssa ();
  auto initial = ssa.initial (body.arguments ().front (), SSAValue::Kind::Entry);
  auto phi = ssa.phi (block, body.arguments ().front (), {std::nullopt});
  ssa.phi_input (phi, initial);
  REQUIRE ((phi.get ()).has_value ());
  ssa.input (ret, 0, SSAOperandPart::Register, phi.get ().value ().output ());
  ssa.mark_renamed ();
  ssa.mark_conventional ();
  REQUIRE ((editor.finish ()).has_value ());
  auto predecessors = phi.get ().value ().predecessors ();
  REQUIRE (predecessors.size () == 1);
  CHECK_FALSE ((predecessors.front ()).has_value ());
  CHECK_FALSE ((*predecessors.begin ()).has_value ());
  CHECK_FALSE ((*predecessors.rbegin ()).has_value ());
  ProgramEditor exit ((program).value ());
  exit.edit (body).ssa ().complete ();
  REQUIRE ((exit.finish ()).has_value ());
  CHECK_FALSE ((phi.get ()).has_value ());
  auto session
    = load_fixture (std::move ((program).value ()), {.mode = ExecutionMode::Interpreter});
  REQUIRE ((session).has_value ());
  CHECK ((session.value ().call_i64 ("entry_phi", "identity", {42})).value () == 42);
}

TEST_CASE ("retained execution handles diagnose moved-from calls and data edits") {
  ProgramBuilder builder;
  auto module = builder.module ("handles");
  module.variable<std::int64_t> ("value", 42);
  auto function = module.function<std::int64_t ()> ("entry");
  function.ret (42);
  auto session = load_fixture (std::move (builder));
  REQUIRE ((session).has_value ());
  auto data = session.value ().find_data ("handles", "value");
  auto call = session.value ().find_function ("handles", "entry");
  REQUIRE ((data).has_value ());
  REQUIRE ((call).has_value ());
  auto retained_data = std::move ((data).value ());
  auto retained_call = std::move ((call).value ());
  CHECK_FALSE ((data.value ().writable_bytes ()).has_value ());
  CHECK (data.value ().bytes ().empty ());
  CHECK (data.value ().address () == 0);
  CHECK_FALSE ((call.value ().artifact ()).has_value ());
  CHECK_FALSE ((call.value ().call_i64 ()).has_value ());
  CHECK ((retained_data.writable_bytes ()).has_value ());
  CHECK ((retained_call.call_i64 ()).value () == 42);
}
