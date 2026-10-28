/* Licensed under the MIT License. Canonical combine and target-predicate regressions. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/SSACombine.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "CombineFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include <limits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
std::unique_ptr<Program> parse (std::string_view text) {
  auto result = serialization::parse_text (text);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::make_unique<Program> (std::move ((result).value ()));
}
Function &entry (Program &c) {
  return const_cast<Function &> (
    *c.modules ().begin ()->find_symbol ("entry").get ()->function ().get ());
}
void prepare (Function &f, FunctionAnalysisManager &a) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> ();
  passes.add<ConventionalSSAPass> ();
  auto status = passes.run (f, a);
  INFO (((status).has_value () ? "" : status.error ().message));
  REQUIRE ((status).has_value ());
}
void finish (Program &c) {
  FunctionPassManager passes;
  passes.add<DestroySSAPass> ();
  REQUIRE ((passes.run (c)).has_value ());
  REQUIRE ((c.verify ()).has_value ());
  auto binary = serialization::write_binary (c);
  REQUIRE ((binary).has_value ());
  REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
  auto text = serialization::write_text (c);
  REQUIRE ((text).has_value ());
  REQUIRE ((serialization::parse_text ((text).value ())).has_value ());
}
}  // namespace
TEST_CASE ("SSA combine covers all integer and floating comparison directions") {
  for (auto &text : test::combine_fixtures ())
    if (text.find (" yes, c") != text.npos) {
      CAPTURE (text);
      auto c = parse (text);
      auto &f = entry (*c);
      FunctionAnalysisManager a (f);
      prepare (f, a);
      auto before = f.instruction_count ();
      auto result = ::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      bool floating = text.find (" c, x, y") != text.npos && text.find (" i64:x") == text.npos;
      bool prohibited = floating && text.find ("bf yes") != text.npos;
      CHECK ((f.instruction_count () < before) == !prohibited);
      REQUIRE ((f.verify ()).has_value ());
      finish (*c);
    }
}
TEST_CASE ("SSA combine target rules preserve backend boundary decisions") {
  auto x86 = target::AddressingRules::get (target::Architecture::X86_64);
  auto arm = target::AddressingRules::get (target::Architecture::AArch64);
  REQUIRE ((x86).has_value ());
  REQUIRE ((arm).has_value ());
  CHECK (x86.value ().accepts (Type::i64 (), INT32_MAX, true, 8));
  CHECK (x86.value ().accepts (Type::i64 (), INT32_MIN, false, 99));
  CHECK_FALSE (x86.value ().accepts (Type::i64 (), std::int64_t (INT32_MAX) + 1, false, 1));
  CHECK_FALSE (x86.value ().accepts (Type::i64 (), 0, true, 3));
  CHECK (arm.value ().accepts (Type::i64 (), 0, true, 3));
  CHECK_FALSE (arm.value ().accepts (Type::i64 (), 0, true, 8));
  CHECK (arm.value ().accepts (Type::i64 (), 12285, false, 1));
  CHECK_FALSE (arm.value ().accepts (Type::i64 (), 12288, false, 1));
  CHECK (arm.value ().accepts (Type::i64 (), 3, false, 1));
  CHECK_FALSE (arm.value ().accepts (Type::i64 (), 8, false, 1));
  CHECK (arm.value ().accepts (Type (Type::Kind::Bool), 3, false, 1));
  CHECK (arm.value ().accepts (Type (Type::Kind::Bool), 0, true, 3));
  CHECK (arm.value ().accepts (Type (Type::Kind::U8), 0, true, 1));
  CHECK_FALSE (arm.value ().accepts (Type (Type::Kind::U8), 1, false, 1));
  CHECK_FALSE (arm.value ().accepts (Type (Type::Kind::Undef), 0, false, 1));
  CHECK_FALSE (
    (target::AddressingRules::get (static_cast<target::Architecture> (99))).has_value ());
}
TEST_CASE ("SSA combine preserves independent ownership and formats on both targets") {
  auto fixtures = test::combine_fixtures ();
  auto random = test::random_combine_fixtures ();
  fixtures.insert (fixtures.end (), random.begin (), random.end ());
  for (auto &text : fixtures)
    for (auto architecture : {target::Architecture::X86_64, target::Architecture::AArch64}) {
      CAPTURE (text);
      auto source = parse (text);
      auto before = dump (*source);
      auto c = clone (*source);
      REQUIRE ((c).has_value ());
      auto &f = entry ((c).value ());
      FunctionAnalysisManager a (f);
      prepare (f, a);
      auto result = ::mirnext::transforms::detail::run_pass (SSACombinePass (architecture), f, a);
      INFO (((result).has_value () ? "" : result.error ().message));
      REQUIRE ((result).has_value ());
      CHECK (dump (*source) == before);
      source.reset ();
      REQUIRE ((f.verify ()).has_value ());
      finish ((c).value ());
    }
}
TEST_CASE ("SSA memory rewriting preserves unrelated input identities and fails atomically") {
  auto c = parse (
    "m: module\nentry: func i64, i64:p, i64:x\nmov i64:0(p):a:b, x\nret x\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto rw = SSAUpdater::get (f);
  REQUIRE ((rw).has_value ());
  Instruction *store = nullptr;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (mirnext::ir::detail::operand_access (i.operands ()[0]).get<Memory> ()) store = &i;
  REQUIRE (store);
  auto form = f.ssa_form ().get ();
  SSAUse *unrelated = nullptr;
  SSAValue *base = nullptr;
  for (auto use : mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (*store)))
    if (std::get<SSAInstructionUse> (use->site ()).operand == 1)
      unrelated = use;
    else
      base = const_cast<SSAValue *> (&use->value ());
  REQUIRE (base);
  REQUIRE (unrelated);
  auto memory = *mirnext::ir::detail::operand_access (store->operands ()[0]).get<Memory> ();
  mirnext::ir::detail::NodeAccess::index (memory) = mirnext::ir::detail::NodeAccess::base (memory);
  memory.scale = 3;
  auto before = dump_ssa (f);
  CHECK_FALSE ((rw.value ().replace_memory (*store, 0, memory, base, nullptr)).has_value ());
  CHECK (dump_ssa (f) == before);
  auto invalid = memory;
  invalid.scale = 256;
  CHECK_FALSE ((rw.value ().replace_memory (*store, 0, invalid, base, base)).has_value ());
  CHECK (dump_ssa (f) == before);
  auto foreign = parse ("m: module\nentry: func i64, i64:p\nret p\nendfunc\nendmodule\n");
  auto &other = entry (*foreign);
  FunctionAnalysisManager other_analyses (other);
  prepare (other, other_analyses);
  SSAValue *foreign_value = nullptr;
  for (auto &value : other.ssa_form ().get ()->values ())
    if (value.alive ()) foreign_value = const_cast<SSAValue *> (&value);
  REQUIRE (foreign_value);
  invalid = memory;
  mirnext::ir::detail::NodeAccess::base (invalid) = &foreign_value->reg ();
  CHECK_FALSE ((rw.value ().replace_memory (*store, 0, invalid, foreign_value, base)).has_value ());
  CHECK (dump_ssa (f) == before);
  REQUIRE ((rw.value ().replace_memory (*store, 0, memory, base, base)).has_value ());
  CHECK (unrelated->alive ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (*store)).front ()
         == unrelated);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (base->uses ()).front ()
         == mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (*store)).back ());
  CHECK (store->operands ()[0].get<Memory> ().value ().alias == "a");
  REQUIRE ((f.verify ()).has_value ());
  before = dump_ssa (f);
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a)).has_value ());
  CHECK (dump_ssa (f) == before);
  finish (*c);
}
TEST_CASE (
  "SSA combine rejects unsupported targets phases owners and indexed input before mutation") {
  auto text = test::combine_fixtures ()[0];
  auto c = parse (text), other = parse (text);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f), foreign (entry (*other));
  auto before = dump (f);
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, a);
  before = dump_ssa (f);
  CHECK_FALSE (
    (::mirnext::transforms::detail::run_pass (SSACombinePass (), f, foreign)).has_value ());
  CHECK_FALSE ((::mirnext::transforms::detail::run_pass (SSACombinePass (
                                                           static_cast<target::Architecture> (99)),
                                                         f, a))
                 .has_value ());
  CHECK (dump_ssa (f) == before);
  REQUIRE ((::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a)).has_value ());
  finish (*c);
}
TEST_CASE ("SSA combine constructs verifies and encodes every public opcode") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto c = test::public_instruction_example (descriptor);
    auto &f = entry (*c);
    FunctionAnalysisManager a (f);
    prepare (f, a);
    auto result = ::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a);
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    finish (*c);
  }
}
TEST_CASE (
  "SSA combine preserves hardware address identity and diagnoses malformed definition cycles") {
  auto inputs = test::combine_fixtures ();
  auto c = parse (inputs[inputs.size () - 2]);
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  REQUIRE ((::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a)).has_value ());
  bool bound = false;
  for (auto &block : f.blocks ())
    for (auto &i : block.instructions ())
      for (auto &operand : i.operands ())
        if (auto memory = mirnext::ir::detail::operand_access (operand).get<Memory> ();
            memory && mirnext::ir::detail::NodeAccess::base (*(memory)))
          bound |= !mirnext::ir::detail::NodeAccess::base (*(memory))->hardware_binding ().empty ();
  CHECK (bound);
  finish (*c);
  c = parse (
    "m: module\nentry: func i64, i64:p\nlocal i64:a, i64:r\nadd a, p, 3\nmov r, i64:0(a)\nret "
    "r\nendfunc\nendmodule\n");
  auto &cycle = entry (*c);
  FunctionAnalysisManager ca (cycle);
  prepare (cycle, ca);
  auto form = cycle.ssa_form ().get ();
  auto rw = SSAUpdater::get (cycle);
  REQUIRE ((rw).has_value ());
  for (auto &block : cycle.blocks ())
    for (auto &i : block.instructions ())
      if (i.opcode () == Opcode::Add) {
        auto value
          = mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (i)).front ();
        SSAUse *source = nullptr;
        for (auto use : mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (i)))
          if (std::get<SSAInstructionUse> (use->site ()).operand == 1) source = use;
        REQUIRE (source);
        REQUIRE ((rw.value ().retarget (*source, *value)).has_value ());
      }
  auto before = dump_ssa (cycle);
  auto result = ::mirnext::transforms::detail::run_pass (SSACombinePass (), cycle, ca);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("cyclic address definition") != std::string::npos);
  CHECK (dump_ssa (cycle) == before);
}
TEST_CASE ("SSA combine retains shared comparison values and surviving branch identities") {
  auto c = parse (
    "m: module\nentry: func i64, i64:x, i64:y\nlocal i64:c, i64:r\ngt c, x, y\nbt yes, c\nret "
    "c\nyes:\nadd r, c, 7\nret r\nendfunc\nendmodule\n");
  auto &f = entry (*c);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  const Instruction *compare = nullptr, *branch = nullptr;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      if (i.opcode () == Opcode::Gt) compare = &i;
      if (i.opcode () == Opcode::Bt) branch = &i;
    }
  REQUIRE (compare);
  REQUIRE (branch);
  auto block = &branch->block ();
  REQUIRE ((::mirnext::transforms::detail::run_pass (SSACombinePass (), f, a)).has_value ());
  CHECK (compare->opcode () == Opcode::Gt);
  CHECK (branch->opcode () == Opcode::Bgt);
  CHECK (&branch->block () == block);
  REQUIRE ((f.verify ()).has_value ());
  finish (*c);
}
