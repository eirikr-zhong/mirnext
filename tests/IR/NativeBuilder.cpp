/* Licensed under the MIT License. Explicit construction preserves existing MIR semantics. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/IR/SSARewriter.hpp"
#include "mirnext/IR/Construction.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/Execution/Session.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include <array>
#include <algorithm>
#include <type_traits>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
template <class T>
concept HasArithmetic = requires (T a) {
  a + a;
  a < a;
  -a;
};
template <class T>
concept HasDereference = requires (T a) { *a; };
template <class T>
concept HasArrow = requires (T a) { a.operator->(); };
static_assert (!HasArithmetic<Value>);
static_assert (!HasDereference<Result<int>>);
static_assert (!HasArrow<Result<int>>);
static_assert (!HasDereference<OptionalRef<int>>);
static_assert (!std::is_constructible_v<bool, Result<int>>);
static_assert (!std::is_constructible_v<bool, Value>);

Program equivalent (bool explicit_api) {
  if (!explicit_api) {
    // Private implementation fixture locks down the previous construction rules.
    ProgramBuilder previous;
    auto module = previous.module ("same");
    auto total = module.variable<std::int64_t> ("total", 0);
    auto add = module.function<std::int64_t (std::int64_t)> ("add");
    auto next = add.load (total) + add.arg (0);
    add.store (total, next);
    add.ret (next);
    auto entry = module.function<std::int64_t (std::int64_t)> ("entry");
    auto first = entry.call (add, {entry.arg (0)});
    auto second = entry.call (add, {entry.arg (0)});
    entry.ret (first + second);
    auto built = previous.finish ();
    REQUIRE (built.has_value ());
    return std::move (built).value ();
  }
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("same");
  auto *total = builder.create_variable (module, "total", std::int64_t{0});
  auto *add = builder.create_function<std::int64_t (std::int64_t)> (module, "add");
  auto next = builder.create_add (builder.create_load (total), builder.argument (0));
  builder.create_store (total, next);
  builder.create_return (next);
  builder.create_function<std::int64_t (std::int64_t)> (module, "entry");
  auto first = builder.create_call (add, {builder.argument (0)});
  auto second = builder.create_call (add, {builder.argument (0)});
  builder.create_return (builder.create_add (first.value (), second.value ()));
  REQUIRE (builder.finish ().has_value ());
  return program;
}
}  // namespace
TEST_CASE ("explicit builder produces the same IR, wire formats and compiled bytes") {
  auto previous = equivalent (false);
  auto modern = equivalent (true);
  CHECK (dump (modern) == dump (previous));
  auto old_text = serialization::write_text (previous),
       new_text = serialization::write_text (modern);
  REQUIRE (old_text.has_value ());
  REQUIRE (new_text.has_value ());
  CHECK (new_text.value () == old_text.value ());
  auto old_binary = serialization::write_binary (previous),
       new_binary = serialization::write_binary (modern);
  REQUIRE (old_binary.has_value ());
  REQUIRE (new_binary.has_value ());
  CHECK (new_binary.value () == old_binary.value ());
  auto roundtrip = serialization::read_binary (new_binary.value ());
  REQUIRE (roundtrip.has_value ());
  CHECK (serialization::write_binary (roundtrip.value ()).value () == old_binary.value ());
  auto target = target::TargetMachine::native ();
  REQUIRE (target.has_value ());
  for (auto level : {transforms::OptimizationLevel::O0, transforms::OptimizationLevel::O1,
                     transforms::OptimizationLevel::O2, transforms::OptimizationLevel::O3}) {
    transforms::OptimizationOptions options;
    options.level = level;
    auto old_opt = transforms::optimize (previous, options),
         new_opt = transforms::optimize (modern, options);
    REQUIRE (old_opt.has_value ());
    REQUIRE (new_opt.has_value ());
    CHECK (new_opt.value ().dump () == old_opt.value ().dump ());
    auto old_code
      = codegen::compile_function (old_opt.value ().find_function ("same", "entry").value (),
                                   target.value ());
    auto new_code
      = codegen::compile_function (new_opt.value ().find_function ("same", "entry").value (),
                                   target.value ());
    REQUIRE (old_code.has_value ());
    REQUIRE (new_code.has_value ());
    CHECK (std::ranges::equal (new_code.value ().bytes (), old_code.value ().bytes ()));
  }
}
TEST_CASE ("explicit builder executes unchanged in every engine") {
  using namespace execution;
  for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                      ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
    CAPTURE (engine);
    ExecutionOptions options;
    options.select_engine
      = [engine] (const Function &) -> Result<ExecutionEngine> { return engine; };
    auto session = ExecutionSession::create (equivalent (true), options);
    REQUIRE (session.has_value ());
    CHECK (session.value ().call_i64 ("same", "entry", {14}).value () == 42);
    CHECK (session.value ().call_i64 ("same", "add", {0}).value () == 28);
  }
}
TEST_CASE ("explicit values and insertion state survive moves without owning nodes") {
  Program source;
  MIRBuilder original (source);
  auto *module = original.create_module ("moves");
  auto *function = original.create_function<std::int64_t (std::int64_t)> (module, "entry");
  auto arg = original.argument (0);
  auto *block = original.insert_block ();
  Program program (std::move (source));
  MIRBuilder builder (std::move (original));
  CHECK (builder.function () == function);
  CHECK (builder.insert_block () == block);
  builder.create_return (builder.create_add (arg, builder.constant_i64 (1)));
  REQUIRE (builder.finish ().has_value ());
  auto before = function->instruction_count ();
  builder.create_return (arg);
  CHECK_FALSE (builder.status ().has_value ());
  CHECK (function->instruction_count () == before);
}
TEST_CASE ("explicit batches retain their first error and stop subsequent writes") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("errors");
  auto *first = builder.create_function<std::int64_t ()> (module, "first");
  auto foreign = builder.create_register (Type::i64 ());
  builder.create_return (builder.constant_i64 (1));
  auto *second = builder.create_function<std::int64_t ()> (module, "second");
  SUBCASE ("foreign value") { builder.create_return (foreign); }
  SUBCASE ("argument out of range") { builder.argument (1); }
  SUBCASE ("duplicate symbol") { builder.create_function<void ()> (module, "first"); }
  SUBCASE ("null insertion block") { builder.set_insert_point (nullptr); }
  SUBCASE ("invalid call result") {
    auto call = builder.create_call (first);
    builder.create_return (call.result (1));
  }
  REQUIRE (builder.error ().has_value ());
  auto message = builder.error ().value ().message;
  auto count = second->instruction_count ();
  builder.create_return (builder.constant_i64 (42));
  builder.create_function<void ()> (nullptr, "later");
  CHECK (second->instruction_count () == count);
  CHECK (builder.error ().value ().message == message);
  CHECK_FALSE (builder.finish ().has_value ());
  CHECK_FALSE (program.verify ().has_value ());
}
TEST_CASE ("explicit batches reject read-only writes and propagate missing function safely") {
  SUBCASE ("read-only variable") {
    Program program;
    MIRBuilder builder (program);
    auto *m = builder.create_module ("readonly");
    auto *value = builder.create_variable (m, "value", std::int64_t{7}, false);
    auto *f = builder.create_function<void ()> (m, "entry");
    builder.create_store (value, builder.constant_i64 (9));
    builder.create_return ();
    CHECK_FALSE (builder.finish ().has_value ());
    CHECK (f->instruction_count () == 0);
  }
  SUBCASE ("no selected function") {
    Program program;
    MIRBuilder builder (program);
    builder.create_add ({}, {});
    builder.ssa ();
    CHECK_FALSE (builder.finish ().has_value ());
  }
}
TEST_CASE ("explicit insertion and rewriting preserve the canonical nodes") {
  Program program;
  MIRBuilder builder (program);
  auto *m = builder.create_module ("edit");
  auto *f = builder.create_function<std::int64_t ()> (m, "entry");
  auto value = builder.create_register (Type::i64 (), "answer");
  builder.create_assign (value, builder.constant_i64 (42));
  builder.create_return (value);
  REQUIRE (builder.finish ().has_value ());
  const Instruction *ret = &f->blocks ().back ().instructions ().back ();
  {
    MIRBuilder edit (*f);
    auto *block = edit.split_before (ret, "return");
    REQUIRE (block != nullptr);
    CHECK (&block->instructions ().front () == ret);
    edit.merge_fallthrough (&f->blocks ().front ());
    REQUIRE (edit.finish ().has_value ());
  }
  CHECK (&f->blocks ().back ().instructions ().back () == ret);
  CHECK (program.verify ().has_value ());
}

TEST_CASE ("explicit arithmetic preserves type promotion and instruction selection") {
  struct Operation {
    const char *name;
    Value (MIRBuilder::*modern) (Value, Value);
    ConstructionValue (*previous) (ConstructionValue, ConstructionValue);
  };
#define OP(name, token)                                                    \
  Operation {                                                              \
    #name, &MIRBuilder::create_##name,                                     \
      +[] (ConstructionValue a, ConstructionValue b) { return a token b; } \
  }
  std::array operations{OP (add, +),  OP (sub, -),  OP (mul, *), OP (div, /),
                        OP (mod, %),  OP (and, &),  OP (or, |),  OP (xor, ^),
                        OP (shl, <<), OP (shr, >>), OP (eq, ==), OP (ne, !=),
                        OP (lt, <),   OP (le, <=),  OP (gt, >),  OP (ge, >=)};
#undef OP
  for (auto kind :
       {Type::Kind::Bool, Type::Kind::I8, Type::Kind::U8, Type::Kind::I16, Type::Kind::U16,
        Type::Kind::I32, Type::Kind::U32, Type::Kind::I64, Type::Kind::U64, Type::Kind::F32,
        Type::Kind::F64, Type::Kind::LongDouble, Type::Kind::Pointer})
    for (auto rhs_type : {Type (kind), Type::i64 (), Type (Type::Kind::F64)})
      for (auto &op : operations) {
        CAPTURE (kind);
        CAPTURE (rhs_type.kind ());
        CAPTURE (op.name);
        ProgramBuilder reference;
        auto old = reference.module ("types").function<void ()> ("entry");
        auto lhs = old.local (Type (kind), "a"), rhs = old.local (rhs_type, "b");
        op.previous (lhs, rhs);
        old.ret ();
        auto baseline = reference.finish ();
        Program program;
        MIRBuilder builder (program);
        auto *module = builder.create_module ("types");
        builder.create_function<void ()> (module, "entry");
        auto a = builder.create_register (Type (kind), "a");
        auto b = builder.create_register (rhs_type, "b");
        (builder.*op.modern) (a, b);
        builder.create_return ();
        auto built = builder.finish ();
        REQUIRE (built.has_value () == baseline.has_value ());
        if (built.has_value ()) {
          CHECK (dump (program) == dump (baseline.value ()));
          CHECK (serialization::write_binary (program).value ()
                 == serialization::write_binary (baseline.value ()).value ());
        }
      }
}

TEST_CASE ("explicit declarations and data retain symbol identity and contiguous layout") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("data");
  auto *declared = builder.create_forward (module, "initializer");
  auto *initializer = builder.create_function<std::int64_t ()> (module, "initializer");
  REQUIRE (initializer != nullptr);
  CHECK (&initializer->symbol () == declared);
  auto *first = builder.create_label (builder.insert_block (), "first");
  auto *exit = builder.create_block ("exit");
  auto *last = builder.create_label (exit, "last");
  builder.create_jump (exit);
  builder.set_insert_point (exit);
  builder.create_return (builder.constant_i64 (42));
  auto *slot = builder.declare_data (module, "layout");
  auto *layout = builder.define_data (slot, InitializedData{Type::i64 (), {Operand (20)}});
  builder.create_anonymous_data (module, InitializedData{Type::i64 (), {Operand (22)}});
  REQUIRE (layout != nullptr);
  CHECK (&layout->symbol () == slot);
  builder.create_data (module, "reference", SymbolReference (*slot, 8));
  builder.create_data (module, "address", LabelReference (*first));
  builder.create_data (module, "difference", LabelReference (*last, *first, 4));
  builder.create_data (module, "zero", ZeroData{16});
  auto *computed = builder.create_expression_variable (module, "computed", initializer);
  builder.create_function<std::int64_t ()> (module, "entry");
  auto next
    = builder.create_load (builder.memory (Type::i64 (), builder.create_address_of (layout), 8));
  builder.create_return (builder.create_add (next, builder.create_load (computed)));
  REQUIRE (builder.finish ().has_value ());
  auto binary = serialization::write_binary (program);
  REQUIRE (binary.has_value ());
  auto decoded = serialization::read_binary (binary.value ());
  REQUIRE (decoded.has_value ());
  CHECK (serialization::write_binary (decoded.value ()).value () == binary.value ());
  auto session
    = execution::ExecutionSession::create (std::move (program),
                                           {.mode = execution::ExecutionMode::Interpreter});
  INFO ((session.has_value () ? "" : session.error ().message));
  REQUIRE (session.has_value ());
  CHECK (session.value ().call_i64 ("data", "entry").value () == 64);
}

TEST_CASE ("values borrow program nodes across builder lifetimes") {
  Program program;
  Function *function = nullptr;
  Value saved;
  {
    MIRBuilder builder (program);
    auto *module = builder.create_module ("lifetimes");
    function = builder.create_function<std::int64_t ()> (module, "entry");
    saved = builder.create_register (Type::i64 (), "saved");
    builder.create_assign (saved, builder.constant_i64 (41));
    builder.create_return (saved);
    REQUIRE (builder.finish ().has_value ());
  }
  {
    MIRBuilder builder (*function);
    builder.erase_instruction (&function->blocks ().front ().instructions ().back ());
    builder.create_return (builder.create_add (saved, builder.constant_i64 (1)));
    REQUIRE (builder.finish ().has_value ());
  }
  auto session = execution::ExecutionSession::create (std::move (program));
  REQUIRE (session.has_value ());
  CHECK (session.value ().call_i64 ("lifetimes", "entry").value () == 42);
}

TEST_CASE ("direct passes preserve the first construction error without rewriting") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("failed_pass");
  auto *function = builder.create_function<std::int64_t ()> (module, "entry");
  builder.create_return (builder.create_add (builder.constant_i64 (40), builder.constant_i64 (2)));
  auto before = dump (*function);
  builder.argument (0);
  REQUIRE (builder.error ().has_value ());
  auto first = builder.error ().value ().message;
  analysis::FunctionAnalysisManager analyses (*function);
  auto result = transforms::SimplifyPass ().run (builder, analyses);
  REQUIRE_FALSE (result.has_value ());
  CHECK (result.error ().message == first);
  analysis::ModuleAnalysisManager module_analyses (*module);
  transforms::PassObserver observer;
  transforms::ModulePassContext context{module_analyses, builder, observer};
  transforms::SimplifyPass pass;
  auto adapted = transforms::FunctionPassAdaptor (pass).run (*module, context);
  REQUIRE_FALSE (adapted.has_value ());
  CHECK (adapted.error ().message == first);
  CHECK (dump (*function) == before);
  CHECK_FALSE (builder.finish ().has_value ());
}

TEST_CASE ("direct passes reject a finished construction batch without rewriting") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("closed_pass");
  auto *function = builder.create_function<std::int64_t ()> (module, "entry");
  builder.create_return (builder.create_add (builder.constant_i64 (40), builder.constant_i64 (2)));
  REQUIRE (builder.finish ().has_value ());
  auto before = dump (*function);
  analysis::FunctionAnalysisManager analyses (*function);
  auto result = transforms::SimplifyPass ().run (builder, analyses);
  CHECK_FALSE (result.has_value ());
  CHECK (dump (*function) == before);
  CHECK (program.verify ().has_value ());
}

TEST_CASE ("module pass adaptation rejects a builder from another program") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("module_pass");
  REQUIRE (builder.finish ().has_value ());
  Program other;
  MIRBuilder foreign (other);
  foreign.create_module ("other");
  analysis::ModuleAnalysisManager analyses (*module);
  transforms::PassObserver observer;
  transforms::ModulePassContext context{analyses, foreign, observer};
  transforms::SimplifyPass pass;
  auto result = transforms::FunctionPassAdaptor (pass).run (*module, context);
  CHECK_FALSE (result.has_value ());
  CHECK (module->functions ().empty ());
  CHECK (foreign.finish ().has_value ());
  CHECK (program.verify ().has_value ());
}

TEST_CASE ("call result queries diagnose a rewritten instruction") {
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("rewritten_call");
  auto *callee = builder.create_function<std::int64_t ()> (module, "callee");
  builder.create_return (builder.constant_i64 (41));
  auto *entry = builder.create_function<std::int64_t ()> (module, "entry");
  auto call = builder.create_call (callee);
  auto output = call.value ();
  builder.replace_instruction (call.instruction (), Opcode::Mov,
                               {output.operand ().value (), Operand (42)});
  REQUIRE (builder.status ().has_value ());
  CHECK_FALSE (call.value ().is_valid ());
  REQUIRE (builder.error ().has_value ());
  auto count = entry->instruction_count ();
  builder.create_return (builder.constant_i64 (42));
  CHECK (entry->instruction_count () == count);
  CHECK_FALSE (builder.finish ().has_value ());
}
