/* Licensed under the MIT License. Owning framed BBV preparation. */
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
  REQUIRE (target);
  return *target;
}
auto parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  return std::move (*c);
}
auto prepared (std::string_view text, unsigned level = 2) {
  auto c = parse (text);
  auto &f = *c->modules ().begin ()->functions ().begin ();
  auto p = BBVPreparation::prepare (f, host (), {.optimization_level = level});
  INFO ((p ? "" : p.error ().message));
  REQUIRE (p);
  REQUIRE (p->verify ());
  return std::move (*p);
}
const BBVBlock &named (const BBVPreparation &p, std::string_view name) {
  for (auto &label : p.function ().labels ())
    if (label.name () == name) {
      auto found = p.block_for (label);
      REQUIRE (found);
      return **found;
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
                                            CHECK_FALSE (view.compiled);
                                            return {};
                                          }},
                                         MachinePreparationKind::BlockVersioning);
      INFO ((p ? "" : p.error ().message));
      REQUIRE (p);
      return std::move (*p);
    }();
    CHECK (callback.expired ());
    REQUIRE (owner.verify ());
    CHECK (stages.back () == CompilationStage::Frame);
    CHECK (std::find (stages.begin (), stages.end (), CompilationStage::Encode) == stages.end ());
    CHECK (std::find (stages.begin (), stages.end (), CompilationStage::StripProperties)
           == stages.end ());
    CHECK (count (owner.function (), Opcode::PrSet) == 1);
    CHECK (count (owner.function (), Opcode::PrBeq) == 1);
    auto f = &owner.function ();
    auto body = f->dump ();
    REQUIRE (owner.frame ().cfg ().blocks.size () > 2);
    auto frame = &owner.frame ().cfg ().blocks[2]->function ();
    CHECK (frame == f);
    auto moved = std::move (owner);
    CHECK_FALSE (owner.valid ());
    CHECK_FALSE (owner.verify ());
    REQUIRE (moved.verify ());
    CHECK (&moved.function () == f);
    CHECK (moved.function ().dump () == body);
    auto illegal = encode_machine (moved.function (), moved.frame ());
    REQUIRE_FALSE (illegal);
    CHECK (illegal.error ().message.find ("requires block specialization") != std::string::npos);
    auto whole_source = parse (properties);
    auto whole
      = prepare_machine_function (*whole_source->modules ().begin ()->functions ().begin (),
                                  host (), {.optimization_level = level});
    REQUIRE (whole);
    CHECK (count (whole->function (), Opcode::PrSet) == 0);
    CHECK (count (whole->function (), Opcode::PrBeq) == 0);
    auto artifact = encode_machine (whole->function (), whole->frame ());
    REQUIRE (artifact);
    REQUIRE (artifact->verify ());
    CHECK_FALSE (BBVPreparation::create (std::move (*whole)));
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
  REQUIRE (p.verify ());
  auto &entry = named (p, "entry"), &alias = named (p, "alias"), &second = named (p, "second"),
       &done = named (p, "done");
  CHECK (&alias == &second);
  REQUIRE (entry.successors ().size () == 2);
  CHECK (entry.successors ()[0].kind == BBVSuccessor::Kind::Branch);
  CHECK (entry.successors ()[0].block == &alias);
  CHECK (entry.successors ()[1].kind == BBVSuccessor::Kind::Fallthrough);
  auto &indirect = *entry.successors ()[1].block;
  REQUIRE (indirect.successors ().size () == 1);
  CHECK (indirect.successors ()[0].kind == BBVSuccessor::Kind::LabelAddress);
  CHECK (indirect.successors ()[0].block == &alias);
  CHECK (indirect.instructions ().back ()->opcode () == MachineOpcode (Opcode::JmpIndirect));
  REQUIRE (alias.successors ().size () == 3);
  CHECK (alias.successors ()[0].block == &done);
  CHECK (alias.successors ()[1].block == &done);
  CHECK (alias.successors ()[2].block == &entry);
  CHECK (alias.successors ()[0].operand == 1);
  CHECK (alias.successors ()[1].operand == 2);
  CHECK (alias.successors ()[2].operand == 3);
  CHECK (done.successors ().empty ());
  std::set<const MachineInstruction *> instructions;
  for (auto &block : p.blocks ())
    for (auto i : block.instructions ()) {
      CHECK (instructions.insert (i).second);
      REQUIRE (p.block_for (*i));
      CHECK (*p.block_for (*i) == &block);
    }
  CHECK (instructions.size () == p.function ().instruction_count ());
  auto text = p.dump ();
  CHECK (text.find ("switch ->") != std::string::npos);
  auto address = &alias;
  auto moved = std::move (p);
  CHECK_FALSE (p.verify ());
  REQUIRE (moved.verify ());
  CHECK (&named (moved, "alias") == address);
  CHECK (moved.dump () == text);
  auto other = prepared (properties);
  auto foreign = other.blocks ().begin ()->instructions ().front ();
  CHECK_FALSE (moved.block_for (*foreign));
  CHECK_FALSE (p.block_for (*foreign));
  for (auto &label : other.function ().labels ()) CHECK_FALSE (moved.block_for (label));
}
TEST_CASE (
  "BBV preparation preserves calls inside original blocks and does not split at plain container "
  "boundaries") {
  auto p = prepared (
    "m:module\np:proto i64,i64:x\nimport host\nf:func i64,i64:x\nlocal i64:r\ncall p,host,r,x\nadd "
    "r,r,1\nret r\nendfunc\nendmodule\n");
  std::size_t calls = 0;
  for (auto &block : p.blocks ())
    for (auto i : block.instructions ())
      if (i->opcode () == MachineOpcode (Opcode::Call)) {
        ++calls;
        CHECK (i != block.instructions ().back ());
      }
  CHECK (calls == 1);
  REQUIRE (p.verify ());
  Context c;
  auto m = c.create_module ("m");
  REQUIRE (m);
  auto f = (*m)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (f);
  auto first = (*f)->create_block (), second = (*f)->create_block ();
  REQUIRE (first);
  REQUIRE (second);
  auto r = (*f)->create_register (Type::i64 ());
  REQUIRE (r);
  REQUIRE (IRBuilder (**first).assign (**r, 42));
  REQUIRE (IRBuilder (**second).ret (Operand (**r)));
  auto joined = BBVPreparation::prepare (**f, host (), {.optimization_level = 0});
  REQUIRE (joined);
  CHECK (std::distance (joined->blocks ().begin (), joined->blocks ().end ()) == 1);
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
  REQUIRE (p);
  for (auto stop : stages) {
    auto failed
      = BBVPreparation::prepare (f, host (), {.observer = [&] (const auto &s) -> Result<void> {
                                   if (s.stage == stop)
                                     return Error{ErrorCode::UnknownName, "stop BBV preparation"};
                                   return {};
                                 }});
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().code == ErrorCode::UnknownName);
    CHECK (failed.error ().message.find (stage_name (stop)) != std::string::npos);
    CHECK (dump (*c) == before);
  }
  CHECK_FALSE (prepare_machine_function (f, host (), {}, static_cast<MachinePreparationKind> (99)));
  CHECK_FALSE (BBVPreparation::prepare (f, host (), {.optimization_level = 4}));
}
TEST_CASE ("BBV preparation covers public instructions and ABI fixtures at all backend levels") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto c = test::public_instruction_example (descriptor);
    // Materialize the same simplified input at every backend level. In
    // particular, keep property operations for the BBV preparation boundary.
    auto program = transforms::optimize (*c, {.level = transforms::OptimizationLevel::O0});
    REQUIRE (program);
    for (auto &module : program->context ().modules ())
      for (auto &f : module.functions ())
        for (unsigned level = 0; level < 4; ++level) {
          CAPTURE (level);
          auto p = BBVPreparation::prepare (f, host (), {.optimization_level = level});
          if (host ().abi () == target::ABI::X86Windows && f.signature ().results.size () > 1) {
            REQUIRE_FALSE (p);
            CHECK (p.error ().message.find ("Windows does not support multiple return values")
                   != std::string::npos);
            continue;
          }
          INFO ((p ? "" : p.error ().message));
          REQUIRE (p);
          REQUIRE (p->verify ());
          std::size_t n = 0;
          for (auto &block : p->blocks ()) n += block.instructions ().size ();
          CHECK (n == p->function ().instruction_count ());
          auto native = BBVProgram::create (std::move (*p));
          REQUIRE (native);
          auto empty = BBVProperties::create (native->preparation ().function ());
          REQUIRE (empty);
          auto entry
            = native->request_version (*native->preparation ().blocks ().begin (), *empty, true);
          REQUIRE (entry);
          std::deque<const BBVVersion *> pending{entry->version};
          std::set<const BBVVersion *> seen;
          while (!pending.empty ()) {
            auto version = pending.front ();
            pending.pop_front ();
            if (!seen.insert (version).second) continue;
            auto plan = native->specialize (*version);
            INFO ((plan ? "" : plan.error ().message));
            INFO (native->dump ());
            INFO (native->preparation ().function ().dump ());
            REQUIRE (plan);
            for (auto &edge : (*plan)->successors ()) pending.push_back (edge.version);
          }
          REQUIRE (native->verify ());
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
        REQUIRE (program);
        auto empty = BBVProperties::create (program->preparation ().function ());
        REQUIRE (empty);
        auto version = program->request_version (block, *empty);
        REQUIRE (version);
        CHECK (version->created);
        auto plan = program->specialize (*version->version);
        REQUIRE (plan);
        REQUIRE ((*plan)->successors ().size () == 1);
        bool taken = (std::string_view (branch) == "prbeq") == bool (value);
        CHECK ((*plan)->successors ()[0].kind
               == (taken ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
        if (taken) {
          REQUIRE_FALSE ((*plan)->emissions ().empty ());
          CHECK ((*plan)->emissions ().back ().opcode == MachineOpcode (Opcode::Jmp));
        }
        for (auto &e : (*plan)->emissions ())
          CHECK ((e.opcode != MachineOpcode (Opcode::PrSet)
                  && e.opcode != MachineOpcode (Opcode::PrBeq)
                  && e.opcode != MachineOpcode (Opcode::PrBne)));
        auto repeated = program->specialize (*version->version);
        REQUIRE (repeated);
        CHECK (*repeated == *plan);
        CHECK (program->preparation ().function ().dump () == before);
        REQUIRE (program->verify ());
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
  REQUIRE (incoming);
  auto program = BBVProgram::create (std::move (p));
  REQUIRE (program);
  auto version = program->request_version (block, *incoming, true);
  REQUIRE (version);
  CHECK (version->created);
  auto empty = BBVProperties::create (program->preparation ().function ());
  REQUIRE (empty);
  auto reused = program->request_version (block, *empty, false);
  REQUIRE (reused);
  CHECK_FALSE (reused->created);
  CHECK (reused->version == version->version);
  CHECK (reused->version->call_entry ());
  REQUIRE (reused->version->incoming ().find (r));
  CHECK (reused->version->incoming ().find (r)->value == 19);
  auto plan = program->specialize (*version->version);
  REQUIRE (plan);
  REQUIRE ((*plan)->successors ().size () == 1);
  CHECK ((*plan)->successors ()[0].version == version->version);
  CHECK_FALSE ((*plan)->successors ()[0].newly_created);
  auto text = program->dump ();
  auto moved = std::move (*program);
  CHECK_FALSE (program->verify ());
  REQUIRE (moved.verify ());
  CHECK (moved.dump () == text);
  CHECK_FALSE (program->request_version (block, *empty));
  CHECK_FALSE (program->specialize (*version->version));
  auto other = BBVProgram::create (prepared (properties));
  REQUIRE (other);
  CHECK_FALSE (other->request_version (block, *empty));
  CHECK_FALSE (other->specialize (*version->version));
  auto foreign_properties = BBVProperties::create (other->preparation ().function ());
  REQUIRE (foreign_properties);
  CHECK_FALSE (moved.request_version (block, *foreign_properties));
  BBVProperty duplicate[]{property, property};
  CHECK_FALSE (BBVProperties::create (moved.preparation ().function (), duplicate));
}
TEST_CASE ("BBV label address reserves the properties at that instruction rather than block exit") {
  auto p = prepared (
    "m:module\nf:func i64,i64:x\nlocal p:address\nentry:prset x,3\nladdr address,yes\nprset "
    "x,8\njmp yes\nyes:ret x\nendfunc\nendmodule\n",
    0);
  auto &block = named (p, "entry");
  const MachineInstruction *set = nullptr;
  for (auto i : block.instructions ())
    if (i->opcode () == MachineOpcode (Opcode::PrSet)) {
      set = i;
      break;
    }
  REQUIRE (set);
  auto r = set->operands ()[0].get<target::PhysicalRegister> ();
  REQUIRE (r);
  auto program = BBVProgram::create (std::move (p));
  REQUIRE (program);
  auto empty = BBVProperties::create (program->preparation ().function ());
  REQUIRE (empty);
  auto version = program->request_version (block, *empty);
  REQUIRE (version);
  auto plan = program->specialize (*version->version);
  REQUIRE (plan);
  REQUIRE ((*plan)->successors ().size () == 2);
  auto edges = (*plan)->successors ();
  CHECK (edges[0].kind == BBVSuccessor::Kind::LabelAddress);
  CHECK (edges[0].newly_created);
  CHECK (edges[1].version == edges[0].version);
  CHECK_FALSE (edges[1].newly_created);
  auto in = edges[0].version->incoming ().find (*r), out = (*plan)->outgoing ().find (*r);
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
      REQUIRE (program);
      auto empty = BBVProperties::create (program->preparation ().function ());
      REQUIRE (empty);
      auto version = program->request_version (block, *empty);
      REQUIRE (version);
      auto plan = program->specialize (*version->version);
      REQUIRE (plan);
      REQUIRE ((*plan)->successors ().size () == 1);
      auto expected = static_cast<std::uint64_t> (std::stoll (match))
                      == static_cast<std::uint32_t> (std::stoll (set));
      CHECK ((*plan)->successors ()[0].kind
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
    REQUIRE (program);
    auto f = program->find_function ("m", "f");
    REQUIRE (f);
    std::set<const MemoryLocation *> locations;
    for (auto &b : f->blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = o.get<Memory> ()) {
            REQUIRE (m->location ());
            CHECK (&m->location ()->function () == f);
            locations.insert (m->location ());
          }
    REQUIRE (locations.size () == 2);
    auto copy = clone (program->context ());
    REQUIRE (copy);
    REQUIRE ((*copy)->verify ());
    auto &copied = *(*copy)->modules ().begin ()->functions ().begin ();
    std::set<const MemoryLocation *> copied_locations;
    for (auto &b : copied.blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = o.get<Memory> ()) {
            REQUIRE (m->location ());
            CHECK (&m->location ()->function () == &copied);
            CHECK_FALSE (locations.contains (m->location ()));
            copied_locations.insert (m->location ());
          }
    CHECK (copied_locations.size () == 2);
    auto p = BBVPreparation::prepare (copied, host ());
    REQUIRE (p);
    std::set<const MachineMemoryLocation *> machine_locations;
    for (auto &b : p->function ().blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = o.get<MachineMemory> (); m && m->location) {
            CHECK (&m->location->function () == &p->function ());
            machine_locations.insert (m->location);
          }
    CHECK (machine_locations.size () == 2);
    auto text = serialization::write_text (program->context ());
    REQUIRE (text);
    auto read = parse (*text);
    for (auto &m : read->modules ())
      for (auto &f : m.functions ())
        for (auto &b : f.blocks ())
          for (auto &i : b.instructions ())
            for (auto &o : i.operands ())
              if (auto m = o.get<Memory> ()) CHECK_FALSE (m->location ());
    auto repeated = transforms::optimize (program->context (), {.level = level});
    REQUIRE (repeated);
    REQUIRE (repeated->verify ());
    auto size = std::distance (repeated->find_function ("m", "f")->memory_locations ().begin (),
                               repeated->find_function ("m", "f")->memory_locations ().end ());
    for (unsigned n = 0; n < 3; ++n) {
      auto next = transforms::optimize (repeated->context (), {.level = level});
      REQUIRE (next);
      REQUIRE (next->verify ());
      auto f = next->find_function ("m", "f");
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
      IRRewriter rw (f);
      const MemoryLocation *p_location = nullptr, *q_location = nullptr;
      // Unused classes stress remapping. Register and memory properties occupy
      // separate typed domains, never offsets into a compact attribute array.
      for (unsigned n = 0; n < 700; ++n) rw.create_memory_location ();
      p_location = &rw.create_memory_location ();
      q_location = &rw.create_memory_location ();
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ())
          for (std::size_t n = 0; n < i.operands ().size (); ++n)
            if (auto m = i.operands ()[n].get<Memory> ()) {
              auto memory = *m;
              bool p = memory.base->name () == "p";
              if (distinct_alias) memory.alias = p ? "left" : "right";
              if (distinct_nonalias) memory.nonalias = "separate";
              REQUIRE (rw.replace_operand (i, n, Operand (memory)));
              REQUIRE (rw.set_memory_location (i, n, p ? p_location : q_location));
            }
      auto prep = BBVPreparation::prepare (f, host (), {.optimization_level = 0});
      INFO ((prep ? "" : prep.error ().message));
      REQUIRE (prep);
      auto &entry = named (*prep, "entry");
      const MachineMemory *memory_p = nullptr;
      for (auto i : entry.instructions ())
        if (i->opcode () == MachineOpcode (Opcode::Mov))
          if (auto m = i->operands ()[1].get<MachineMemory> (); m && m->location) memory_p = m;
      REQUIRE (memory_p);
      BBVProperty property{memory_p->location, 11, *memory_p};
      auto incoming = BBVProperties::create (prep->function (), {&property, 1});
      REQUIRE (incoming);
      auto before = prep->function ().dump ();
      auto program = BBVProgram::create (std::move (*prep));
      REQUIRE (program);
      auto version = program->request_version (entry, *incoming);
      REQUIRE (version);
      auto plan = program->specialize (*version->version);
      REQUIRE (plan);
      REQUIRE ((*plan)->successors ().size () == 1);
      bool retained = distinct_alias || distinct_nonalias;
      CHECK ((*plan)->successors ()[0].kind
             == (retained ? BBVSuccessor::Kind::Branch : BBVSuccessor::Kind::Fallthrough));
      std::size_t memories = 0;
      for (auto &p : (*plan)->outgoing ().entries ())
        if (p.memory) {
          ++memories;
          REQUIRE (p.memory->location);
          CHECK (p.memory->location == std::get<const MachineMemoryLocation *> (p.location));
        }
      CHECK (memories == (retained ? 2 : 1));
      CHECK (program->preparation ().function ().dump () == before);
      REQUIRE (program->verify ());
      auto foreign = prepared (properties);
      MachineMemory memory{Type::i64 ()};
      BBVProperty bad{static_cast<const MachineMemoryLocation *> (nullptr), 1, memory};
      CHECK_FALSE (BBVProperties::create (foreign.function (), {&bad, 1}));
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
    REQUIRE (program);
    auto empty = BBVProperties::create (program->preparation ().function ());
    REQUIRE (empty);
    auto v = program->request_version (entry, *empty);
    REQUIRE (v);
    auto plan = program->specialize (*v->version);
    REQUIRE (plan);
    REQUIRE ((*plan)->successors ().size () == 1);
    bool retained
      = std::string_view (action) == "mov a,b" || std::string_view (action) == "add a,a,b";
    CHECK ((*plan)->successors ()[0].kind
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
    IRRewriter rw (f);
    auto &location = rw.create_memory_location ();
    for (auto &b : f.blocks ())
      for (auto &i : b.instructions ())
        if (i.opcode () == Opcode::Mov && i.operands ()[1].get<Memory> ())
          REQUIRE (rw.set_memory_location (i, 1, &location));
    auto p = BBVPreparation::prepare (f, host (), {.optimization_level = 0});
    REQUIRE (p);
    const MachineMemory *memory = nullptr;
    for (auto &b : p->function ().blocks ())
      for (auto &i : b.instructions ())
        for (auto &o : i.operands ())
          if (auto m = o.get<MachineMemory> (); m && m->location) memory = m;
    REQUIRE (memory);
    BBVProperty property{memory->location, 29, *memory};
    auto incoming = BBVProperties::create (p->function (), {&property, 1});
    REQUIRE (incoming);
    auto &entry = named (*p, "entry");
    REQUIRE_FALSE (entry.instructions ().empty ());
    CHECK ((entry.instructions ().back ()->opcode () == MachineOpcode (Opcode::Call)) == terminal);
    auto program = BBVProgram::create (std::move (*p));
    REQUIRE (program);
    auto version = program->request_version (entry, *incoming);
    REQUIRE (version);
    auto plan = program->specialize (*version->version);
    REQUIRE (plan);
    CHECK (bool ((*plan)->outgoing ().find (property.location)) == !terminal);
    REQUIRE (program->verify ());
  }
}
