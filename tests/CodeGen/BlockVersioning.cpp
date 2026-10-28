/* Licensed under the MIT License. Owning framed BBV preparation. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include "PostAllocationFixtures.hpp"
#include <set>
#include <deque>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
auto host () {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  return (target).value ();
}
auto parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
auto prepared (std::string_view text, unsigned level = 2) {
  auto c = parse (text);
  auto &f = *c->modules ().begin ()->functions ().begin ();
  auto p = BBVPreparation::prepare (f, host (), {.optimization_level = level});
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  REQUIRE ((p.value ().verify ()).has_value ());
  return std::move ((p).value ());
}
const BBVBlock &named (const BBVPreparation &p, std::string_view name) {
  for (auto &label : p.function ().labels ())
    if (label.name () == name) {
      auto found = p.block_for (label);
      REQUIRE ((found).has_value ());
      return (found).value ();
    }
  FAIL ("missing BBV label");
  return *p.blocks ().begin ();
}
std::size_t count (const MachineFunction &f, Opcode opcode) {
  std::size_t n = 0;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) n += i.opcode () == MachineOpcode (opcode);
  return n;
}
constexpr auto properties
  = "m:module\nf:func i64,i64:x\nlocal i64:r\nprset x,1\nprbeq yes,x,1\nmov r,17\nret r\nyes:mov "
    "r,42\nret r\nendfunc\nendmodule\n";
}  // namespace
TEST_CASE (
  "prepared functions retain canonical frame and machine identities after source destruction and "
  "moves") {
  for (unsigned level = 0; level < 4; ++level) {
    std::vector<CompilationStage> stages;
    std::weak_ptr<int> callback;
    auto owner = [&] {
      auto c = parse (properties);
      auto state = std::make_shared<int> ();
      callback = state;
      auto p = prepare_machine_function (*c->modules ().begin ()->functions ().begin (), host (),
                                         {.optimization_level = level,
                                          .observer
                                          = [state, &stages] (const auto &view) -> Result<void> {
                                            stages.push_back (view.stage);
                                            CHECK_FALSE (view.compiled.has_value ());
                                            return {};
                                          }},
                                         MachinePreparationKind::BlockVersioning);
      INFO (((p).has_value () ? "" : p.error ().message));
      REQUIRE ((p).has_value ());
      return std::move ((p).value ());
    }();
    CHECK (callback.expired ());
    REQUIRE ((owner.verify ()).has_value ());
    CHECK (stages.back () == CompilationStage::Frame);
    CHECK (std::find (stages.begin (), stages.end (), CompilationStage::Encode) == stages.end ());
    CHECK (std::find (stages.begin (), stages.end (), CompilationStage::StripProperties)
           == stages.end ());
    CHECK (count (owner.function (), Opcode::PrSet) == 1);
    CHECK (count (owner.function (), Opcode::PrBeq) == 1);
    auto f = &owner.function ();
    auto body = f->dump ();
    REQUIRE (mirnext::codegen::detail::NodeAccess::blocks (owner.frame ().cfg ()).size () > 2);
    auto frame
      = &mirnext::codegen::detail::NodeAccess::blocks (owner.frame ().cfg ())[2]->function ();
    CHECK (frame == f);
    auto moved = std::move (owner);
    CHECK_FALSE (owner.valid ());
    CHECK_FALSE ((owner.verify ()).has_value ());
    REQUIRE ((moved.verify ()).has_value ());
    CHECK (&moved.function () == f);
    CHECK (moved.function ().dump () == body);
    auto illegal = encode_machine (moved.function (), moved.frame ());
    REQUIRE_FALSE ((illegal).has_value ());
    CHECK (illegal.error ().message.find ("requires block specialization") != std::string::npos);
    auto whole_source = parse (properties);
    auto whole
      = prepare_machine_function (*whole_source->modules ().begin ()->functions ().begin (),
                                  host (), {.optimization_level = level});
    REQUIRE ((whole).has_value ());
    CHECK (count (whole.value ().function (), Opcode::PrSet) == 0);
    CHECK (count (whole.value ().function (), Opcode::PrBeq) == 0);
    auto artifact = encode_machine (whole.value ().function (), whole.value ().frame ());
    REQUIRE ((artifact).has_value ());
    REQUIRE ((artifact.value ().verify ()).has_value ());
    CHECK_FALSE ((BBVPreparation::create (std::move ((whole).value ()))).has_value ());
  }
}
TEST_CASE (
  "BBV layout preserves branches label aliases label addresses duplicate switches and indirect "
  "exits") {
  auto p = prepared (
    "m:module\nf:func i64,i64:x\nlocal p:address\nentry:prset x,7\nprbeq alias,x,7\nladdr "
    "address,alias\njmpi address\nalias:\nsecond:\nswitch x,done,done,entry\ndone:ret "
    "x\nendfunc\nendmodule\n",
    0);
  REQUIRE ((p.verify ()).has_value ());
  auto &entry = named (p, "entry"), &alias = named (p, "alias"), &second = named (p, "second"),
       &done = named (p, "done");
  CHECK (&alias == &second);
  REQUIRE (entry.successors ().size () == 2);
  CHECK (entry.successors ()[0].kind == BBVSuccessor::Kind::Branch);
  CHECK (mirnext::codegen::detail::NodeAccess::block (entry.successors ()[0]) == &alias);
  CHECK (entry.successors ()[1].kind == BBVSuccessor::Kind::Fallthrough);
  auto &indirect = *mirnext::codegen::detail::NodeAccess::block (entry.successors ()[1]);
  REQUIRE (indirect.successors ().size () == 1);
  CHECK (indirect.successors ()[0].kind == BBVSuccessor::Kind::LabelAddress);
  CHECK (mirnext::codegen::detail::NodeAccess::block (indirect.successors ()[0]) == &alias);
  CHECK (indirect.instructions ().back ().opcode () == MachineOpcode (Opcode::JmpIndirect));
  REQUIRE (alias.successors ().size () == 3);
  CHECK (mirnext::codegen::detail::NodeAccess::block (alias.successors ()[0]) == &done);
  CHECK (mirnext::codegen::detail::NodeAccess::block (alias.successors ()[1]) == &done);
  CHECK (mirnext::codegen::detail::NodeAccess::block (alias.successors ()[2]) == &entry);
  CHECK (alias.successors ()[0].operand == 1);
  CHECK (alias.successors ()[1].operand == 2);
  CHECK (alias.successors ()[2].operand == 3);
  CHECK (done.successors ().empty ());
  std::set<const MachineInstruction *> instructions;
  for (auto &block : p.blocks ())
    for (auto i : mirnext::ir::detail::ReferenceAccess::pointers (block.instructions ())) {
      CHECK (instructions.insert (i).second);
      REQUIRE ((p.block_for (*i)).has_value ());
      CHECK (&(p.block_for (*i)).value () == &block);
    }
  CHECK (instructions.size () == p.function ().instruction_count ());
  auto text = p.dump ();
  CHECK (text.find ("switch ->") != std::string::npos);
  auto address = &alias;
  auto moved = std::move (p);
  CHECK_FALSE ((p.verify ()).has_value ());
  REQUIRE ((moved.verify ()).has_value ());
  CHECK (&named (moved, "alias") == address);
  CHECK (moved.dump () == text);
  auto other = prepared (properties);
  auto foreign
    = mirnext::ir::detail::ReferenceAccess::pointers (other.blocks ().begin ()->instructions ())
        .front ();
  CHECK_FALSE ((moved.block_for (*foreign)).has_value ());
  CHECK_FALSE ((p.block_for (*foreign)).has_value ());
  for (auto &label : other.function ().labels ())
    CHECK_FALSE ((moved.block_for (label)).has_value ());
}
TEST_CASE (
  "BBV preparation preserves calls inside original blocks and does not split at plain container "
  "boundaries") {
  auto p = prepared (
    "m:module\np:proto i64,i64:x\nimport host\nf:func i64,i64:x\nlocal i64:r\ncall p,host,r,x\nadd "
    "r,r,1\nret r\nendfunc\nendmodule\n");
  std::size_t calls = 0;
  for (auto &block : p.blocks ())
    for (auto i : mirnext::ir::detail::ReferenceAccess::pointers (block.instructions ()))
      if (i->opcode () == MachineOpcode (Opcode::Call)) {
        ++calls;
        CHECK (i != &block.instructions ().back ());
      }
  CHECK (calls == 1);
  REQUIRE ((p.verify ()).has_value ());
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("m");
  REQUIRE ((m).has_value ());
  auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto first = mirnext::ir::detail::access (((f)).value ()).create_block (),
       second = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((first).has_value ());
  REQUIRE ((second).has_value ());
  auto r = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
  REQUIRE ((r).has_value ());
  REQUIRE ((InstructionBuilder ((first).value ()).assign ((r).value (), 42)).has_value ());
  REQUIRE ((InstructionBuilder ((second).value ()).ret (Operand ((r).value ()))).has_value ());
  auto joined = BBVPreparation::prepare ((f).value (), host (), {.optimization_level = 0});
  REQUIRE ((joined).has_value ());
  CHECK (std::distance (joined.value ().blocks ().begin (), joined.value ().blocks ().end ()) == 1);
}
TEST_CASE (
  "BBV preparation propagates observer errors at every observed stage without mutating source") {
  auto c = parse (properties);
  auto before = dump (*c);
  auto &f = *c->modules ().begin ()->functions ().begin ();
  std::vector<CompilationStage> stages;
  auto p = BBVPreparation::prepare (f, host (), {.observer = [&] (const auto &s) -> Result<void> {
                                      stages.push_back (s.stage);
                                      return {};
                                    }});
  REQUIRE ((p).has_value ());
  for (auto stop : stages) {
    auto failed
      = BBVPreparation::prepare (f, host (), {.observer = [&] (const auto &s) -> Result<void> {
                                   if (s.stage == stop)
                                     return Error{ErrorCode::UnknownName, "stop BBV preparation"};
                                   return {};
                                 }});
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().code == ErrorCode::UnknownName);
    CHECK (failed.error ().message.find (stage_name (stop)) != std::string::npos);
    CHECK (dump (*c) == before);
  }
  CHECK_FALSE ((prepare_machine_function (f, host (), {}, static_cast<MachinePreparationKind> (99)))
                 .has_value ());
  CHECK_FALSE ((BBVPreparation::prepare (f, host (), {.optimization_level = 4})).has_value ());
}
TEST_CASE ("BBV preparation covers public instructions and ABI fixtures at all backend levels") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto c = test::public_instruction_example (descriptor);
    // Materialize the same simplified input at every backend level. In
    // particular, keep property operations for the BBV preparation boundary.
    auto program = transforms::optimize (*c, {.level = transforms::OptimizationLevel::O0});
    REQUIRE ((program).has_value ());
    for (auto &module : program.value ().program ().modules ())
      for (auto &f : module.functions ())
        for (unsigned level = 0; level < 4; ++level) {
          CAPTURE (level);
          auto p = BBVPreparation::prepare (f, host (), {.optimization_level = level});
          if (host ().abi () == target::ABI::X86Windows && f.signature ().results.size () > 1) {
            REQUIRE_FALSE ((p).has_value ());
            CHECK (p.error ().message.find ("Windows does not support multiple return values")
                   != std::string::npos);
            continue;
          }
          INFO (((p).has_value () ? "" : p.error ().message));
          REQUIRE ((p).has_value ());
          REQUIRE ((p.value ().verify ()).has_value ());
          std::size_t n = 0;
          for (auto &block : p.value ().blocks ())
            n += mirnext::ir::detail::ReferenceAccess::pointers (block.instructions ()).size ();
          CHECK (n == p.value ().function ().instruction_count ());
          auto native = BBVProgram::create (std::move ((p).value ()));
          REQUIRE ((native).has_value ());
          auto empty = BBVProperties::create (native.value ().preparation ().function ());
          REQUIRE ((empty).has_value ());
          auto entry
            = native.value ().request_version (*native.value ().preparation ().blocks ().begin (),
                                               (empty).value (), true);
          REQUIRE ((entry).has_value ());
          std::deque<const BBVVersion *> pending{
            mirnext::codegen::detail::NodeAccess::version (((entry)).value ())};
          std::set<const BBVVersion *> seen;
          while (!pending.empty ()) {
            auto version = pending.front ();
            pending.pop_front ();
            if (!seen.insert (version).second) continue;
            auto plan = native.value ().specialize (*version);
            INFO (((plan).has_value () ? "" : plan.error ().message));
            INFO (native.value ().dump ());
            INFO (native.value ().preparation ().function ().dump ());
            REQUIRE ((plan).has_value ());
            for (auto &edge : plan.value ().successors ())
              pending.push_back (mirnext::codegen::detail::NodeAccess::version (edge));
          }
          REQUIRE ((native.value ().verify ()).has_value ());
        }
  }
}
TEST_CASE (
  "BBV property branches specialize without changing authoritative instructions at O0 through O3") {
  for (unsigned level = 0; level < 4; ++level)
    for (auto branch : {"prbeq", "prbne"})
      for (auto value : {0, 1}) {
        auto p
          = prepared (std::string ("m:module\nf:func i64,i64:x\nlocal i64:r\nentry:prset x,1\n")
                        + branch + " yes,x," + std::to_string (value)
                        + "\nmov r,17\nret r\nyes:mov r,42\nret r\nendfunc\nendmodule\n",
                      level);
        auto &block = named (p, "entry");
        auto before = p.function ().dump ();
        auto program = BBVProgram::create (std::move (p));
        REQUIRE ((program).has_value ());
        auto empty = BBVProperties::create (program.value ().preparation ().function ());
        REQUIRE ((empty).has_value ());
        auto version = program.value ().request_version (block, (empty).value ());
        REQUIRE ((version).has_value ());
        CHECK (version.value ().created);
        auto plan = program.value ().specialize (
          *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
        REQUIRE ((plan).has_value ());
        REQUIRE (plan.value ().successors ().size () == 1);
        bool taken = (std::string_view (branch) == "prbeq") == bool (value);
        CHECK (plan.value ().successors ()[0].kind
               == (taken ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
        if (taken) {
          REQUIRE_FALSE (plan.value ().emissions ().empty ());
          CHECK (plan.value ().emissions ().back ().opcode == MachineOpcode (Opcode::Jmp));
        }
        for (auto &e : plan.value ().emissions ())
          CHECK ((e.opcode != MachineOpcode (Opcode::PrSet)
                  && e.opcode != MachineOpcode (Opcode::PrBeq)
                  && e.opcode != MachineOpcode (Opcode::PrBne)));
        auto repeated = program.value ().specialize (
          *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
        REQUIRE ((repeated).has_value ());
        CHECK (&(repeated).value () == &(plan).value ());
        CHECK (program.value ().preparation ().function ().dump () == before);
        REQUIRE ((program.value ().verify ()).has_value ());
      }
}
TEST_CASE (
  "BBV versions preserve first request properties and stable identities across loops and moves") {
  auto p
    = prepared ("m:module\nf:func i64,i64:x\nentry:prset x,7\njmp entry\nendfunc\nendmodule\n", 0);
  auto &block = named (p, "entry");
  auto r = host ().registers ()[0].reg;
  BBVProperty property{r, 19, {}};
  auto incoming = BBVProperties::create (p.function (), {&property, 1});
  REQUIRE ((incoming).has_value ());
  auto program = BBVProgram::create (std::move (p));
  REQUIRE ((program).has_value ());
  auto version = program.value ().request_version (block, (incoming).value (), true);
  REQUIRE ((version).has_value ());
  CHECK (version.value ().created);
  auto empty = BBVProperties::create (program.value ().preparation ().function ());
  REQUIRE ((empty).has_value ());
  auto reused = program.value ().request_version (block, (empty).value (), false);
  REQUIRE ((reused).has_value ());
  CHECK_FALSE (reused.value ().created);
  CHECK (mirnext::codegen::detail::NodeAccess::version (((reused)).value ())
         == mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
  CHECK (mirnext::codegen::detail::NodeAccess::version (((reused)).value ())->call_entry ());
  REQUIRE (
    (mirnext::codegen::detail::NodeAccess::version (((reused)).value ())->incoming ().find (r))
      .has_value ());
  CHECK (mirnext::codegen::detail::NodeAccess::version (((reused)).value ())
           ->incoming ()
           .find (r)
           .value ()
           .value
         == 19);
  auto plan = program.value ().specialize (
    *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
  REQUIRE ((plan).has_value ());
  REQUIRE (plan.value ().successors ().size () == 1);
  CHECK (mirnext::codegen::detail::NodeAccess::version (plan.value ().successors ()[0])
         == mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
  CHECK_FALSE (plan.value ().successors ()[0].newly_created);
  auto text = program.value ().dump ();
  auto moved = std::move ((program).value ());
  CHECK_FALSE ((program.value ().verify ()).has_value ());
  REQUIRE ((moved.verify ()).has_value ());
  CHECK (moved.dump () == text);
  CHECK_FALSE ((program.value ().request_version (block, (empty).value ())).has_value ());
  CHECK_FALSE ((program.value ().specialize (
                  *mirnext::codegen::detail::NodeAccess::version (((version)).value ())))
                 .has_value ());
  auto other = BBVProgram::create (prepared (properties));
  REQUIRE ((other).has_value ());
  CHECK_FALSE ((other.value ().request_version (block, (empty).value ())).has_value ());
  CHECK_FALSE ((other.value ().specialize (
                  *mirnext::codegen::detail::NodeAccess::version (((version)).value ())))
                 .has_value ());
  auto foreign_properties = BBVProperties::create (other.value ().preparation ().function ());
  REQUIRE ((foreign_properties).has_value ());
  CHECK_FALSE ((moved.request_version (block, (foreign_properties).value ())).has_value ());
  BBVProperty duplicate[]{property, property};
  CHECK_FALSE ((BBVProperties::create (moved.preparation ().function (), duplicate)).has_value ());
}
TEST_CASE ("BBV label address reserves the properties at that instruction rather than block exit") {
  auto p = prepared (
    "m:module\nf:func i64,i64:x\nlocal p:address\nentry:prset x,3\nladdr address,yes\nprset "
    "x,8\njmp yes\nyes:ret x\nendfunc\nendmodule\n",
    0);
  auto &block = named (p, "entry");
  const MachineInstruction *set = nullptr;
  for (auto i : mirnext::ir::detail::ReferenceAccess::pointers (block.instructions ()))
    if (i->opcode () == MachineOpcode (Opcode::PrSet)) {
      set = i;
      break;
    }
  REQUIRE (set);
  auto r = mirnext::codegen::detail::operand_access (set->operands ()[0])
             .get<target::PhysicalRegister> ();
  REQUIRE (r);
  auto program = BBVProgram::create (std::move (p));
  REQUIRE ((program).has_value ());
  auto empty = BBVProperties::create (program.value ().preparation ().function ());
  REQUIRE ((empty).has_value ());
  auto version = program.value ().request_version (block, (empty).value ());
  REQUIRE ((version).has_value ());
  auto plan = program.value ().specialize (
    *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
  REQUIRE ((plan).has_value ());
  REQUIRE (plan.value ().successors ().size () == 2);
  auto edges = plan.value ().successors ();
  CHECK (edges[0].kind == BBVSuccessor::Kind::LabelAddress);
  CHECK (edges[0].newly_created);
  CHECK (mirnext::codegen::detail::NodeAccess::version (edges[1])
         == mirnext::codegen::detail::NodeAccess::version (edges[0]));
  CHECK_FALSE (edges[1].newly_created);
  auto in = mirnext::codegen::detail::NodeAccess::version (edges[0])->incoming ().find (*r).get (),
       out = plan.value ().outgoing ().find (*r).get ();
  REQUIRE (in);
  REQUIRE (out);
  CHECK (in->value == 3);
  CHECK (out->value == 8);
}
TEST_CASE ("BBV property immediates retain full branch bits and uint32 assignment behavior") {
  for (auto set : {"0", "4294967296", "4294967297", "-1"})
    for (auto match : {"0", "1", "4294967296", "4294967295", "-1"}) {
      auto p = prepared (std::string ("m:module\nf:func i64,i64:x\nentry:prset x,") + set
                           + "\nprbeq yes,x," + match + "\nret x\nyes:ret x\nendfunc\nendmodule\n",
                         0);
      auto &block = named (p, "entry");
      auto program = BBVProgram::create (std::move (p));
      REQUIRE ((program).has_value ());
      auto empty = BBVProperties::create (program.value ().preparation ().function ());
      REQUIRE ((empty).has_value ());
      auto version = program.value ().request_version (block, (empty).value ());
      REQUIRE ((version).has_value ());
      auto plan = program.value ().specialize (
        *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
      REQUIRE ((plan).has_value ());
      REQUIRE (plan.value ().successors ().size () == 1);
      auto expected = static_cast<std::uint64_t> (std::stoll (match))
                      == static_cast<std::uint32_t> (std::stoll (set));
      CHECK (plan.value ().successors ()[0].kind
             == (expected ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
    }
}
TEST_CASE (
  "GVN memory identities survive SSA exit cloning and machine allocation without serialization") {
  auto source = parse (
    "m:module\nf:func i64,p:p,p:q,i64:x\nmov i64:(p),x\nmov i64:(q),x\nret "
    "x\nendfunc\nendmodule\n");
  for (auto level : {transforms::OptimizationLevel::O2, transforms::OptimizationLevel::O3}) {
    auto program = transforms::optimize (*source, {.level = level});
    REQUIRE ((program).has_value ());
    auto f = program.value ().find_function ("m", "f").get ();
    REQUIRE (f);
    std::set<const MemoryLocation *> locations;
    for (auto &b : f->blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ()) {
            REQUIRE ((m->location ()).has_value ());
            CHECK (&m->location ().value ().function () == f);
            locations.insert (m->location ().get ());
          }
    REQUIRE (locations.size () == 2);
    auto copy = clone (program.value ().program ());
    REQUIRE ((copy).has_value ());
    REQUIRE ((copy.value ().verify ()).has_value ());
    auto &copied = *copy.value ().modules ().begin ()->functions ().begin ();
    std::set<const MemoryLocation *> copied_locations;
    for (auto &b : copied.blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ()) {
            REQUIRE ((m->location ()).has_value ());
            CHECK (&m->location ().value ().function () == &copied);
            CHECK_FALSE (locations.contains (m->location ().get ()));
            copied_locations.insert (m->location ().get ());
          }
    CHECK (copied_locations.size () == 2);
    auto p = BBVPreparation::prepare (copied, host ());
    REQUIRE ((p).has_value ());
    std::set<const MachineMemoryLocation *> machine_locations;
    for (auto &b : p.value ().function ().blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = mirnext::codegen::detail::operand_access (o).get<MachineMemory> ();
              m && mirnext::codegen::detail::NodeAccess::location (*(m))) {
            CHECK (&mirnext::codegen::detail::NodeAccess::location (*(m))->function ()
                   == &p.value ().function ());
            machine_locations.insert (mirnext::codegen::detail::NodeAccess::location (*(m)));
          }
    CHECK (machine_locations.size () == 2);
    auto text = serialization::write_text (program.value ().program ());
    REQUIRE ((text).has_value ());
    auto read = parse ((text).value ());
    for (auto &m : read->modules ())
      for (auto &f : m.functions ())
        for (auto &b : f.blocks ())
          for (auto &i : b.instructions ())
            for (auto &o : i.operands ())
              if (auto m = mirnext::ir::detail::operand_access (o).get<Memory> ())
                CHECK_FALSE ((m->location ()).has_value ());
    auto repeated = transforms::optimize (program.value ().program (), {.level = level});
    REQUIRE ((repeated).has_value ());
    REQUIRE ((repeated.value ().verify ()).has_value ());
    auto size = std::
      distance (repeated.value ().find_function ("m", "f").get ()->memory_locations ().begin (),
                repeated.value ().find_function ("m", "f").get ()->memory_locations ().end ());
    for (unsigned n = 0; n < 3; ++n) {
      auto next = transforms::optimize (repeated.value ().program (), {.level = level});
      REQUIRE ((next).has_value ());
      REQUIRE ((next.value ().verify ()).has_value ());
      auto f = next.value ().find_function ("m", "f").get ();
      CHECK (std::distance (f->memory_locations ().begin (), f->memory_locations ().end ())
             <= size);
      repeated = std::move (next);
    }
  }
}
TEST_CASE ("BBV copies properties with sparse typed memory identities and alias exclusions") {
  for (bool distinct_alias : {false, true})
    for (bool distinct_nonalias : {false, true}) {
      auto source = parse (
        "m:module\nf:func i64,p:p,p:q,i64:x\nentry:prset x,7\nmov i64:(q),x\nprbeq "
        "yes,i64:(p),11\nret x\nyes:ret x\nendfunc\nendmodule\n");
      auto &f = *source->modules ().begin ()->functions ().begin ();
      InstructionRewriter rw (f);
      const MemoryLocation *p_location = nullptr, *q_location = nullptr;
      // Unused classes stress remapping. Register and memory properties occupy
      // separate typed domains, never offsets into a compact attribute array.
      for (unsigned n = 0; n < 700; ++n) rw.create_memory_location ();
      p_location = &rw.create_memory_location ();
      q_location = &rw.create_memory_location ();
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ())
          for (std::size_t n = 0; n < i.operands ().size (); ++n)
            if (auto m = mirnext::ir::detail::operand_access (i.operands ()[n]).get<Memory> ()) {
              auto memory = *m;
              bool p = mirnext::ir::detail::NodeAccess::base (memory)->name () == "p";
              if (distinct_alias) memory.alias = p ? "left" : "right";
              if (distinct_nonalias) memory.nonalias = "separate";
              REQUIRE ((rw.replace_operand (i, n, Operand (memory))).has_value ());
              REQUIRE ((rw.set_memory_location (i, n, p ? p_location : q_location)).has_value ());
            }
      auto prep = BBVPreparation::prepare (f, host (), {.optimization_level = 0});
      INFO (((prep).has_value () ? "" : prep.error ().message));
      REQUIRE ((prep).has_value ());
      auto &entry = named ((prep).value (), "entry");
      const MachineMemory *memory_p = nullptr;
      for (auto i : mirnext::ir::detail::ReferenceAccess::pointers (entry.instructions ()))
        if (i->opcode () == MachineOpcode (Opcode::Mov))
          if (auto m
              = mirnext::codegen::detail::operand_access (i->operands ()[1]).get<MachineMemory> ();
              m && mirnext::codegen::detail::NodeAccess::location (*(m)))
            memory_p = m;
      REQUIRE (memory_p);
      BBVProperty property{*mirnext::codegen::detail::NodeAccess::location (*(memory_p)), 11,
                           *memory_p};
      auto incoming = BBVProperties::create (prep.value ().function (), {&property, 1});
      REQUIRE ((incoming).has_value ());
      auto before = prep.value ().function ().dump ();
      auto program = BBVProgram::create (std::move ((prep).value ()));
      REQUIRE ((program).has_value ());
      auto version = program.value ().request_version (entry, (incoming).value ());
      REQUIRE ((version).has_value ());
      auto plan = program.value ().specialize (
        *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
      REQUIRE ((plan).has_value ());
      REQUIRE (plan.value ().successors ().size () == 1);
      bool retained = distinct_alias || distinct_nonalias;
      CHECK (plan.value ().successors ()[0].kind
             == (retained ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
      std::size_t memories = 0;
      for (auto &p : plan.value ().outgoing ().entries ())
        if (p.memory) {
          ++memories;
          REQUIRE (mirnext::codegen::detail::NodeAccess::location (*(p.memory)));
          CHECK (mirnext::codegen::detail::NodeAccess::location (*(p.memory))
                 == std::get<const MachineMemoryLocation *> (
                   mirnext::codegen::detail::NodeAccess::storage (p.location)));
        }
      CHECK (memories == (retained ? 2 : 1));
      CHECK (program.value ().preparation ().function ().dump () == before);
      REQUIRE ((program.value ().verify ()).has_value ());
      auto foreign = prepared (properties);
      MachineMemory memory
        = mirnext::codegen::detail::NodeAccess::make_MachineMemory (Type::i64 ());
      BBVProperty bad{mirnext::codegen::detail::NodeAccess::make_BBVPropertyLocation (nullptr), 1,
                      memory};
      CHECK_FALSE ((BBVProperties::create (foreign.function (), {&bad, 1})).has_value ());
    }
}
TEST_CASE (
  "BBV register copy rules distinguish absent properties immediate inputs and explicit zero") {
  auto reg_a = host ().architecture () == target::Architecture::AArch64 ? "r19" : "r12";
  auto reg_b = host ().architecture () == target::Architecture::AArch64 ? "r20" : "r13";
  for (auto action : {"mov a,b", "mov a,9", "prset a,0", "prset b,8\nmov a,b", "add a,a,b"}) {
    auto source = std::string ("m:module\nf:func i64,i64:x\nglobal i64:a:") + reg_a
                  + ",i64:b:" + reg_b + "\nentry:prset a,3\n" + action
                  + "\nprbeq yes,a,3\nret a\nyes:ret a\nendfunc\nendmodule\n";
    auto p = prepared (source, 0);
    auto &entry = named (p, "entry");
    auto program = BBVProgram::create (std::move (p));
    REQUIRE ((program).has_value ());
    auto empty = BBVProperties::create (program.value ().preparation ().function ());
    REQUIRE ((empty).has_value ());
    auto v = program.value ().request_version (entry, (empty).value ());
    REQUIRE ((v).has_value ());
    auto plan = program.value ().specialize (
      *mirnext::codegen::detail::NodeAccess::version (((v)).value ()));
    REQUIRE ((plan).has_value ());
    REQUIRE (plan.value ().successors ().size () == 1);
    bool retained
      = std::string_view (action) == "mov a,b" || std::string_view (action) == "add a,a,b";
    CHECK (plan.value ().successors ()[0].kind
           == (retained ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
  }
}
TEST_CASE (
  "BBV clears memory properties only for a terminal call preserving the original call boundary "
  "rule") {
  for (bool terminal : {false, true}) {
    auto source = parse (
      std::string (
        "m:module\np:proto\nimport host\nf:func i64,p:address\nlocal i64:r\nentry:call p,host\n")
      + (terminal ? "" : "mov r,1\n") + "after:mov r,i64:(address)\nret r\nendfunc\nendmodule\n");
    auto &f = *source->modules ().begin ()->functions ().begin ();
    InstructionRewriter rw (f);
    auto &location = rw.create_memory_location ();
    for (auto &b : f.blocks ())
      for (auto &i : b.instructions ())
        if (i.opcode () == Opcode::Mov
            && mirnext::ir::detail::operand_access (i.operands ()[1]).get<Memory> ())
          REQUIRE ((rw.set_memory_location (i, 1, &location)).has_value ());
    auto p = BBVPreparation::prepare (f, host (), {.optimization_level = 0});
    REQUIRE ((p).has_value ());
    const MachineMemory *memory = nullptr;
    for (auto &b : p.value ().function ().blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = mirnext::codegen::detail::operand_access (o).get<MachineMemory> ();
              m && mirnext::codegen::detail::NodeAccess::location (*(m)))
            memory = m;
    REQUIRE (memory);
    BBVProperty property{*mirnext::codegen::detail::NodeAccess::location (*(memory)), 29, *memory};
    auto incoming = BBVProperties::create (p.value ().function (), {&property, 1});
    REQUIRE ((incoming).has_value ());
    auto &entry = named ((p).value (), "entry");
    REQUIRE_FALSE (entry.instructions ().empty ());
    // Windows restores its mandatory home space after CALL, so even a call
    // at the end of a public IR block is not a terminal machine instruction.
    const bool terminal_machine_call = terminal && host ().abi () != target::ABI::X86Windows;
    CHECK ((entry.instructions ().back ().opcode () == MachineOpcode (Opcode::Call))
           == terminal_machine_call);
    auto program = BBVProgram::create (std::move ((p).value ()));
    REQUIRE ((program).has_value ());
    auto version = program.value ().request_version (entry, (incoming).value ());
    REQUIRE ((version).has_value ());
    auto plan = program.value ().specialize (
      *mirnext::codegen::detail::NodeAccess::version (((version)).value ()));
    REQUIRE ((plan).has_value ());
    CHECK (bool ((plan.value ().outgoing ().find (property.location)).has_value ())
           == !terminal_machine_call);
    REQUIRE ((program.value ().verify ()).has_value ());
  }
}
