/* Licensed under the MIT License. Native pass semantics and object lifetime. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/Expressions.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../IR/StackScopes.hpp"
#include "SimplifyFixtures.hpp"
#include <cmath>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
void simplify (Program &context) {
  for (auto &module : context.modules ())
    for (auto &function : module.functions ()) {
      analysis::FunctionAnalysisManager analyses (function);
      transforms::FunctionPassManager passes;
      passes.add<transforms::SimplifyPass> ();
      auto result = passes.run (function, analyses);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
    }
  auto valid = context.verify ();
  INFO (((valid).has_value () ? "" : valid.error ().message));
  REQUIRE ((valid).has_value ());
}
std::unique_ptr<Program> parse (std::string_view text) {
  auto parsed = serialization::parse_text (text);
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  return std::make_unique<Program> (std::move ((parsed).value ()));
}
void roundtrip (const Program &context) {
  auto original = dump (context);
  auto copied = clone (context);
  INFO (((copied).has_value () ? "" : copied.error ().message));
  REQUIRE ((copied).has_value ());
  CHECK (dump ((copied).value ()) == original);
  auto text = serialization::write_text (context);
  INFO (((text).has_value () ? "" : text.error ().message));
  REQUIRE ((text).has_value ());
  auto parsed = serialization::parse_text ((text).value ());
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  auto second = serialization::write_text ((parsed).value ());
  REQUIRE ((second).has_value ());
  CHECK ((second).value () == (text).value ());
  auto binary = serialization::write_binary (context);
  REQUIRE ((binary).has_value ());
  auto decoded = serialization::read_binary ((binary).value ());
  REQUIRE ((decoded).has_value ());
  CHECK (dump (context) == original);
  REQUIRE ((decoded.value ().verify ()).has_value ());
}
}  // namespace
TEST_CASE ("native simplify handles every public instruction and independent cloning") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = test::public_instruction_example (descriptor);
    simplify (*context);
    roundtrip (*context);
  }
}
TEST_CASE ("native simplify handles expression call global and scope fixtures") {
  for (unsigned n = 0; n <= static_cast<unsigned> (test::ExpressionExample::Initializer); ++n) {
    CAPTURE (n);
    auto context = test::expression_example (static_cast<test::ExpressionExample> (n));
    simplify (*context);
    roundtrip (*context);
  }
  for (unsigned n = 0; n <= static_cast<unsigned> (test::CallExample::Inline); ++n) {
    CAPTURE (n);
    auto context = test::call_example (static_cast<test::CallExample> (n));
    simplify (*context);
    roundtrip (*context);
  }
  for (unsigned n = 0; n <= static_cast<unsigned> (test::VariableExample::Narrow); ++n) {
    CAPTURE (n);
    auto context = test::variable_example (static_cast<test::VariableExample> (n));
    simplify (*context);
    roundtrip (*context);
  }
  for (auto mode :
       {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto context = test::stack_scope_example (mode);
    simplify (*context);
    CHECK_FALSE (needs_materialization (*context));
    roundtrip (*context);
  }
}
TEST_CASE ("native simplify verifies and roundtrips all focused rule fixtures") {
  unsigned n = 0;
  for (auto &text : test::simplify_fixtures ()) {
    CAPTURE (n);
    ++n;
    auto context = parse (text);
    simplify (*context);
    roundtrip (*context);
  }
}
TEST_CASE ("native simplify isolates hardware bindings without changing their identity") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("hardware");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto bound = mirnext::ir::detail::access (((function)).value ())
                 .create_register (Type::i64 (), "bound", "r5");
  REQUIRE ((bound).has_value ());
  auto pointer = mirnext::ir::detail::access (((function)).value ())
                   .create_register (Type::pointer (), "pointer", "r6");
  REQUIRE ((pointer).has_value ());
  InstructionBuilder builder ((block).value ());
  REQUIRE ((builder.assign ((bound).value (), 40)).has_value ());
  REQUIRE ((builder.emit (Opcode::Add,
                          {Operand ((bound).value ()), Operand ((bound).value ()), Operand (2)}))
             .has_value ());
  REQUIRE (
    (builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pointer).value (),
                                                                  &(pointer).value (), 8, 4),
                    Operand ((bound).value ())))
      .has_value ());
  auto copy
    = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 (), "copy");
  REQUIRE ((copy).has_value ());
  REQUIRE ((builder.assign ((copy).value (), Operand ((bound).value ()))).has_value ());
  REQUIRE ((builder.address_of ((copy).value ())).has_value ());
  REQUIRE ((builder.ret (Operand ((bound).value ()))).has_value ());
  simplify (context);
  CHECK (function.value ().find_register ("bound").get () == &(bound).value ());
  CHECK (function.value ().find_register ("pointer").get () == &(pointer).value ());
  unsigned bindings = 0;
  for (auto &reg : function.value ().registers ()) bindings += !reg.hardware_binding ().empty ();
  CHECK (bindings == 2);
  bool saw_address = false;
  for (auto &b : function.value ().blocks ()) {
    const Instruction *previous = nullptr;
    for (auto &i : b.instructions ()) {
      if (i.opcode () == Opcode::Add)
        for (auto &o : i.operands ())
          if (auto r = mirnext::ir::detail::operand_access (o).get<const Register *> ())
            CHECK ((*r)->hardware_binding ().empty ());
      if (i.opcode () == Opcode::Addr) {
        saw_address = true;
        auto proxy
          = mirnext::ir::detail::operand_access (i.operands ()[1]).get<const Register *> ();
        REQUIRE (proxy);
        CHECK ((*proxy)->hardware_binding ().empty ());
        REQUIRE (previous);
        REQUIRE (previous->opcode () == Opcode::Mov);
        CHECK (
          *mirnext::ir::detail::operand_access (previous->operands ()[0]).get<const Register *> ()
          == *proxy);
        CHECK (
          *mirnext::ir::detail::operand_access (previous->operands ()[1]).get<const Register *> ()
          == &(bound).value ());
      }
      for (auto &o : i.operands ())
        if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ()) {
          CHECK (mirnext::ir::detail::NodeAccess::base (*(m))->hardware_binding ().empty ());
          CHECK_FALSE (mirnext::ir::detail::NodeAccess::index (*(m)));
        }
      previous = &i;
    }
  }
  CHECK (saw_address);
  roundtrip (context);
}
TEST_CASE (
  "native simplify preserves unsigned immediate identity and does not overapply algebraic rules") {
  for (auto opcode : {Opcode::Add, Opcode::Mul, Opcode::UDiv, Opcode::And, Opcode::UMulo}) {
    Program context;
    auto m = mirnext::ir::detail::access (context).create_module ("unsigned");
    REQUIRE ((m).has_value ());
    auto f = mirnext::ir::detail::access (((m)).value ())
               .create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
    REQUIRE ((b).has_value ());
    auto r = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
    REQUIRE ((r).has_value ());
    InstructionBuilder builder ((b).value ());
    auto instruction
      = builder.emit (opcode, {Operand ((r).value ()), Operand (42),
                               Operand (std::uint64_t (opcode == Opcode::Add ? 0 : 1))});
    REQUIRE ((instruction).has_value ());
    REQUIRE ((builder.ret (Operand ((r).value ()))).has_value ());
    simplify (context);
    CHECK (instruction.value ().opcode () == opcode);
    bool found = false;
    for (auto &i : b.value ().instructions ())
      for (auto &o : i.operands ())
        found |= mirnext::ir::detail::operand_access (o).get<std::uint64_t> () != nullptr;
    CHECK (found);
    roundtrip (context);
  }
}
TEST_CASE (
  "native literal pools preserve string bytes float bits and collision-free ordered names") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("pools");
  REQUIRE ((m).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).create_data (".literal0", ZeroData{8}))
             .has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).import (".literal1")).has_value ());
  for (auto text :
       {std::string (""), std::string ("abc"), std::string ("abc\0", 4), std::string ("a\0b", 3)}) {
    auto data = mirnext::ir::detail::access (((m)).value ()).create_literal (Operand (text));
    REQUIRE ((data).has_value ());
    auto &v = std::get<InitializedData> (data.value ().initializer ()).values;
    CHECK (v.size () == text.size () + (text.empty () || text.back () != '\0'));
    CHECK ((v.back ().get<std::int64_t> ()).value () == 0);
    for (std::size_t n = 0; n < text.size (); ++n)
      CHECK ((v[n].get<std::int64_t> ()).value () == static_cast<unsigned char> (text[n]));
    CHECK (data.value ().is_literal_pool ());
    CHECK_FALSE (data.value ().writable ());
    CHECK (data.value ().symbol ().name () != ".literal0");
    CHECK (data.value ().symbol ().name () != ".literal1");
  }
  auto literal = mirnext::ir::detail::access (((m)).value ()).create_literal (Operand (-0.0));
  REQUIRE ((literal).has_value ());
  auto &value = std::get<InitializedData> (literal.value ().initializer ()).values.front ();
  CHECK (std::signbit ((value.get<double> ()).value ()));
  auto count = m.value ().items ().size ();
  CHECK_FALSE (
    (mirnext::ir::detail::access (((m)).value ()).create_literal (Operand (42))).has_value ());
  CHECK (m.value ().items ().size () == count);
  CHECK_FALSE ((m.value ().error ()).has_value ());
  auto copied = clone (context);
  REQUIRE ((copied).has_value ());
  REQUIRE ((copied.value ().verify ()).has_value ());
}
TEST_CASE ("native simplify preserves real instruction identity and invalidates cached CFG") {
  auto context
    = parse ("m:module\nf:func i64\nlocal i64:x\nadd x, 20, 22\nret x\nendfunc\nendmodule\n");
  auto &f = *context->modules ().begin ()->functions ().begin ();
  auto &add = *f.blocks ().begin ()->instructions ().begin ();
  auto identity = &add;
  analysis::FunctionAnalysisManager analyses (f);
  REQUIRE ((analyses.get<analysis::ControlFlowAnalysis> ()).has_value ());
  auto before = f.revisions ();
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
  CHECK (&add == identity);
  CHECK (add.opcode () == Opcode::Add);
  CHECK (mirnext::ir::detail::operand_access (add.operands ()[1]).get<const Register *> ());
  CHECK (mirnext::ir::detail::operand_access (add.operands ()[2]).get<const Register *> ());
  CHECK (f.revisions ().instructions > before.instructions);
  REQUIRE ((context->verify ()).has_value ());
  REQUIRE ((passes.run (f, analyses)).has_value ());
  REQUIRE ((context->verify ()).has_value ());
  roundtrip (*context);
}
TEST_CASE ("native simplify materializes float initializer literals with checked provenance") {
  auto context = parse (
    "m:module\nvalue:expr f\nf:func d\nlocal d:x\ndadd x, 20.5, 21.5\nret x\nendfunc\nendmodule\n");
  simplify (*context);
  CHECK (needs_materialization (*context));
  auto &f = *context->modules ().begin ()->functions ().begin ();
  Instruction *load = nullptr;
  const Memory *memory = nullptr;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::DMov
          && mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ()) {
        load = &i;
        memory = mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ();
        break;
      }
  REQUIRE (load);
  REQUIRE (memory);
  REQUIRE ((memory->literal_source ()).has_value ());
  roundtrip (*context);
  auto copied = clone (*context);
  REQUIRE ((copied).has_value ());
  context.reset ();
  REQUIRE ((copied.value ().verify ()).has_value ());
  roundtrip ((copied).value ());
  auto &copy_function = *copied.value ().modules ().begin ()->functions ().begin ();
  for (auto &b : copy_function.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::DMov
          && mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ()) {
        load = &i;
        memory = mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ();
        break;
      }
  REQUIRE (memory);
  auto address = mirnext::ir::detail::NodeAccess::base (*(memory));
  InstructionRewriter rewriter (copy_function);
  REQUIRE (
    (rewriter.insert_before (*load, Opcode::Mov, {Operand (*address), Operand (0)})).has_value ());
  auto invalid = copied.value ().verify ();
  REQUIRE_FALSE ((invalid).has_value ());
  CHECK (invalid.error ().message.find ("verified literal pools") != std::string::npos);
}
TEST_CASE ("native simplify keeps scalar memory alias metadata and local addresses") {
  auto context = parse (
    "m:module\nf:func i64\nlocal i64:x, i64:p, i64:i\nmov x, 42\naddr p, x\nadd x, i64:8(p,i,4), "
    "2\nret x\nendfunc\nendmodule\n");
  auto &f = *context->modules ().begin ()->functions ().begin ();
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Add) {
        auto memory = *mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ();
        memory.alias = "a";
        memory.nonalias = "b";
        REQUIRE ((InstructionRewriter (f).replace_operand (i, 1, Operand (memory))).has_value ());
      }
  simplify (*context);
  bool found = false, address = false;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      if (i.opcode () == Opcode::Addr) {
        address = true;
        CHECK ((*mirnext::ir::detail::operand_access (i.operands ()[1]).get<const Register *> ())
                 ->name ()
               == "x");
      }
      for (auto &o : i.operands ())
        if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ()) {
          found = true;
          CHECK (m->alias == "a");
          CHECK (m->nonalias == "b");
          CHECK (mirnext::ir::detail::NodeAccess::base (*(m)));
          CHECK_FALSE (mirnext::ir::detail::NodeAccess::index (*(m)));
          CHECK (m->displacement == 0);
        }
    }
  CHECK (found);
  CHECK (address);
  roundtrip (*context);
}
TEST_CASE ("native simplify returns alignment overflow diagnostics before entering a runtime") {
  for (auto text :
       {"m:module\nf:func\nlocal i64:p\nalloca p, 9223372036854775807\nret\nendfunc\nendmodule\n",
        "m:module\nf:func\nlocal i64:p, i64:q\nalloca p, 9223372036854775792\nalloca q, "
        "16\nret\nendfunc\nendmodule\n"}) {
    auto context = parse (text);
    auto &f = *context->modules ().begin ()->functions ().begin ();
    analysis::FunctionAnalysisManager analyses (f);
    transforms::FunctionPassManager passes;
    passes.add<transforms::SimplifyPass> ();
    auto result = passes.run (f, analyses);
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("m/f [simplify]") != std::string::npos);
    CHECK (result.error ().message.find ("overflows") != std::string::npos);
  }
}
TEST_CASE ("native simplify accepts the largest aligned signed allocation size") {
  // The final rounded size fits, even though value + align would overflow.
  auto context = parse (
    "m:module\nf:func\nlocal i64:p\nalloca p, 9223372036854775792\nret\nendfunc\nendmodule\n");
  auto &function = *context->modules ().begin ()->functions ().begin ();
  analysis::FunctionAnalysisManager analyses (function);
  auto result
    = ::mirnext::transforms::detail::run_pass (transforms::SimplifyPass (), function, analyses);
  REQUIRE ((result).has_value ());
  bool found = false;
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ())
      if (instruction.opcode () == Opcode::Alloca) found = true;
  // Simplify materializes the size in a register; its defining literal must
  // retain the exact boundary value rather than wrap during rounding.
  CHECK (dump (*context).find ("9223372036854775792") != std::string::npos);
  CHECK (found);
  REQUIRE ((function.verify ()).has_value ());
}
TEST_CASE ("literal provenance rejects tampering and foreign owners atomically") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("pool");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{Type (Type::Kind::F64)}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto address = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
  REQUIRE ((address).has_value ());
  auto result
    = mirnext::ir::detail::access (((function)).value ()).create_register (Type (Type::Kind::F64));
  REQUIRE ((result).has_value ());
  auto pool = mirnext::ir::detail::access (((module)).value ()).create_literal (Operand (42.0));
  REQUIRE ((pool).has_value ());
  InstructionRewriter rewriter ((function).value ());
  auto memory = rewriter.literal_memory ((pool).value (), (address).value ());
  REQUIRE ((memory).has_value ());
  InstructionBuilder builder ((block).value ());
  REQUIRE ((builder.assign ((address).value (), Operand (pool.value ().symbol ()))).has_value ());
  auto load
    = builder.emit (Opcode::DMov, {Operand ((result).value ()), Operand ((memory).value ())});
  REQUIRE ((load).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  for (unsigned n = 0; n < 3; ++n) {
    auto altered = (memory).value ();
    if (n == 0) altered.displacement = 8;
    if (n == 1) mirnext::ir::detail::NodeAccess::index (altered) = &(address).value ();
    if (n == 2) altered.type = Type::i64 ();
    auto before = dump (context);
    CHECK_FALSE ((rewriter.replace_operand ((load).value (), 1, Operand (altered))).has_value ());
    CHECK (dump (context) == before);
  }
  const Operand store[]{Operand ((memory).value ()), Operand ((result).value ())};
  CHECK_FALSE ((rewriter.replace ((load).value (), Opcode::DMov, store)).has_value ());
  auto other = mirnext::ir::detail::access (((module)).value ()).create_function ("other", {});
  REQUIRE ((other).has_value ());
  auto other_address
    = mirnext::ir::detail::access (((other)).value ()).create_register (Type::i64 ());
  REQUIRE ((other_address).has_value ());
  CHECK_FALSE ((rewriter.literal_memory ((pool).value (), (other_address).value ())).has_value ());
  auto other_block = mirnext::ir::detail::access (((other)).value ()).create_block ();
  REQUIRE ((other_block).has_value ());
  REQUIRE ((InstructionBuilder ((other_block).value ()).ret ()).has_value ());
  auto writable
    = mirnext::ir::detail::access (((module)).value ())
        .create_data ("writable", InitializedData{Type (Type::Kind::F64), {Operand (42.0)}});
  REQUIRE ((writable).has_value ());
  CHECK_FALSE ((rewriter.literal_memory ((writable).value (), (address).value ())).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("multiplication identities retain flags used through intervening moves") {
  auto context = parse (
    "m:module\nf:func i64\nlocal i64:x, i64:y\nmulo x, 42, 1\nmov y, x\nbo L\nret y\nL:\nret "
    "0\nendfunc\nendmodule\n");
  simplify (*context);
  bool producer = false;
  for (auto &block : context->modules ().begin ()->functions ().begin ()->blocks ())
    for (auto &instruction : block.instructions ())
      producer |= instruction.opcode () == Opcode::Mulo;
  CHECK (producer);
  roundtrip (*context);
}
TEST_CASE ("already simplified operands preserve storage revisions and cached analyses") {
  auto context
    = parse ("m:module\nf:func i64\nlocal i64:x\nmov x, 42\nret x\nendfunc\nendmodule\n");
  auto &function = *context->modules ().begin ()->functions ().begin ();
  auto &instruction = *function.blocks ().begin ()->instructions ().begin ();
  auto storage = instruction.operands ().data ();
  auto revisions = function.revisions ();
  analysis::FunctionAnalysisManager analyses (function);
  auto cfg = analyses.get<analysis::ControlFlowAnalysis> ();
  REQUIRE ((cfg).has_value ());
  auto identity = &(cfg).value ();
  auto result
    = ::mirnext::transforms::detail::run_pass (transforms::SimplifyPass (), function, analyses);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().transformations == 0);
  CHECK (function.revisions () == revisions);
  CHECK (instruction.operands ().data () == storage);
  analyses.invalidate (result.value ().preserved);
  auto cached = analyses.get<analysis::ControlFlowAnalysis> ();
  REQUIRE ((cached).has_value ());
  CHECK (&(cached).value () == identity);
  roundtrip (*context);
}
