/* Licensed under the MIT License. Owned native bytes, relocations and lifetime. */
#include "mirnext/IR/Construction.hpp"
#include "mirnext/IR/Builder.hpp"
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "PostAllocationFixtures.hpp"
#include <algorithm>
#include <bit>
#include <climits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool arm () {
  return target::TargetMachine::native ().value ().architecture () == target::Architecture::AArch64;
}
struct Fixture {
  MachineFunction f{"owned", "encode", Signature{{Type::i64 ()}, {}},
                    (target::TargetMachine::native ()).value ()};
  MachineRewriter rw{f};
  MachineBasicBlock &entry = mirnext::codegen::detail::access (f).create_block ();
  Fixture () {
    auto s = f.state ();
    s.machinized = s.operands_legalized = true;
    REQUIRE ((rw.set_state (s)).has_value ());
  }
  MachineOperand reg (unsigned n) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
      f.target ().registers ()[n].reg);
  }
  MachineOperand imm (std::uint64_t n) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (n);
  }
  MachineInstruction *add (Opcode code, std::vector<MachineOperand> ops) {
    auto r = rw.append (entry, code, std::move (ops));
    REQUIRE ((r).has_value ());
    return &(r).value ();
  }
  MachineFrameResult frame (unsigned level = 0) {
    auto graph = MachineGraphAnalysis::build (f);
    REQUIRE ((graph).has_value ());
    auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
    REQUIRE ((live).has_value ());
    auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
    REQUIRE ((ranges).has_value ());
    auto a = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
    REQUIRE ((a).has_value ());
    auto r = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                std::move ((a).value ()));
    REQUIRE ((r).has_value ());
    auto s = split_allocated_spills (f, std::move ((r).value ()));
    REQUIRE ((s).has_value ());
    auto p = optimize_allocated_machine (f, std::move ((s).value ()));
    REQUIRE ((p).has_value ());
    auto framed = form_frame (f, std::move ((p).value ()));
    REQUIRE ((framed).has_value ());
    return std::move ((framed).value ());
  }
  CompiledFunction run (MachineEncodingOptions options = {}) {
    auto framed = frame ();
    auto before = f.dump ();
    auto revision = f.revision ();
    auto code = encode_machine (f, framed, std::move (options));
    INFO (((code).has_value () ? "" : code.error ().message));
    REQUIRE ((code).has_value ());
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
    REQUIRE ((code.value ().verify ()).has_value ());
    return std::move ((code).value ());
  }
};
struct MemoryProvider final : execution::CodeMemoryProvider {
  std::shared_ptr<execution::CodeMemoryProvider> native = execution::native_code_memory_provider ();
  unsigned allocations = 0, releases = 0, writes = 0;
  std::size_t page_size () const override { return native->page_size (); }
  Result<execution::MappedCodeMemory> allocate (std::size_t n) override {
    ++allocations;
    return native->allocate (n);
  }
  Result<void> release (execution::MappedCodeMemory m) override {
    ++releases;
    return native->release (m);
  }
  Result<void> make_writable (execution::MappedCodeMemory m) override {
    ++writes;
    return native->make_writable (m);
  }
  Result<void> make_executable (execution::MappedCodeMemory m) override {
    return native->make_executable (m);
  }
  Result<void> flush (execution::MappedCodeMemory m) override { return native->flush (m); }
};
std::uint64_t word (std::span<const std::uint8_t> bytes, std::size_t at, unsigned count = 4) {
  std::uint64_t value = 0;
  for (unsigned n = 0; n < count; ++n) value |= std::uint64_t (bytes[at + n]) << (n * 8);
  return value;
}
CompiledFunction compile (std::string_view text, unsigned level = 2, bool host = false) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto p = transforms::optimize ((c).value (), options);
  REQUIRE ((p).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto f = prepare_machine_ir (*p.value ().program ().modules ().begin ()->functions ().begin (),
                               (target).value ());
  REQUIRE ((f).has_value ());
  (c).value () = mirnext::ir::Program{};  // No source Program survives machine preparation.
  REQUIRE ((machinize ((f).value ())).has_value ());
  REQUIRE ((legalize_operands ((f).value ())).has_value ());
  auto graph = MachineGraphAnalysis::build ((f).value ());
  REQUIRE ((graph).has_value ());
  auto merged = coalesce_registers ((f).value (), (graph).value (), level);
  REQUIRE ((merged).has_value ());
  graph = MachineGraphAnalysis::build ((f).value ());
  REQUIRE ((graph).has_value ());
  auto live
    = MachineLiveness::analyze ((f).value (), (graph).value (), {.optimization_level = level});
  REQUIRE ((live).has_value ());
  auto ranges = MachineLiveRanges::build ((f).value (), (graph).value (), (live).value ());
  REQUIRE ((ranges).has_value ());
  auto a = MachineAllocation::assign ((f).value (), (graph).value (), (live).value (),
                                      (ranges).value ());
  REQUIRE ((a).has_value ());
  auto r = rewrite_allocated ((f).value (), (graph).value (), (live).value (), (ranges).value (),
                              std::move ((a).value ()));
  REQUIRE ((r).has_value ());
  auto s = split_allocated_spills ((f).value (), std::move ((r).value ()));
  REQUIRE ((s).has_value ());
  auto post = optimize_allocated_machine ((f).value (), std::move ((s).value ()));
  REQUIRE ((post).has_value ());
  auto frame
    = form_frame ((f).value (), std::move ((post).value ()), {.preserve_host_registers = host});
  REQUIRE ((frame).has_value ());
  auto result = encode_machine ((f).value (), (frame).value ());
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
}  // namespace
TEST_CASE ("owning encoding emits known AArch64 words without changing instructions") {
  if (!arm ()) return;
  Fixture t;
  t.add (Opcode::Mov, {t.reg (0), t.imm (42)});
  t.add (Opcode::Ret, {t.reg (0)});
  auto code = t.run ();
  CHECK (code.bytes ().size () == 16);
  CHECK (word (code.bytes (), 0) == 0xd2800540);
  CHECK (word (code.bytes (), 4) == 0xd65f03c0);
  CHECK (code.relocations ().empty ());
}
TEST_CASE ("owning encoding preserves every wide immediate chunk and inversion rule") {
  if (!arm ()) return;
  for (auto v : {0ull, 1ull, 65535ull, 65536ull, 0xffffffffffffffffull, 0xffffffff0000ffffull,
                 0x1234000056780000ull, 0x123456789abcdef0ull}) {
    CAPTURE (v);
    Fixture t;
    t.add (Opcode::Mov, {t.reg (0), t.imm (v)});
    t.add (Opcode::Ret, {t.reg (0)});
    auto code = t.run ();
    std::uint64_t value = 0;
    for (std::size_t n = 0; n < code.bytes ().size (); n += 4) {
      auto w = word (code.bytes (), n);
      auto kind = w & 0xff800000;
      auto shift = ((w >> 21) & 3) * 16;
      auto chunk = (w >> 5) & 65535;
      if (kind == 0xd2800000) value = chunk << shift;
      if (kind == 0x92800000) value = ~(chunk << shift);
      if (kind == 0xf2800000) value = (value & ~(std::uint64_t{65535} << shift)) | (chunk << shift);
    }
    CHECK (value == v);
  }
}
TEST_CASE ("owning compiled symbol relocations outlive machine nodes and rebase repeatedly") {
  if (!arm ()) return;
  std::optional<CompiledFunction> code;
  {
    Fixture t;
    auto symbol = mirnext::codegen::detail::access (t.f).intern_symbol (
      {"module", "data", MachineSymbol::Kind::Data});
    REQUIRE ((symbol).has_value ());
    t.add (Opcode::Mov,
           {t.reg (0),
            mirnext::codegen::detail::NodeAccess::make_MachineOperand (
              mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(symbol).value (),
                                                                                 -7))});
    t.add (Opcode::Ret, {t.reg (0)});
    code = t.run ();
  }
  CHECK (code->relocations ().size () == 4);
  CHECK_FALSE ((code->relocate (0x1000)).has_value ());
  auto original = std::vector (code->bytes ().begin (), code->bytes ().end ());
  for (auto address : std::array<std::uint64_t, 3>{0, 0x123456789abcdef0ull, UINT64_MAX}) {
    unsigned calls = 0;
    auto bytes = code->relocate (0x1000, [&] (auto &request) -> Result<std::uint64_t> {
      ++calls;
      REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->module == "module");
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "data");
      return address;
    });
    REQUIRE ((bytes).has_value ());
    CHECK (calls == 1);
    std::uint64_t value = 0;
    for (auto &r : code->relocations ())
      value |= ((word ((bytes).value (), r.offset) >> 5) & 65535) << r.shift;
    CHECK (value == address - 7);
    CHECK (std::equal (original.begin (), original.end (), code->bytes ().begin ()));
  }
  CHECK_FALSE ((code->relocate (3)).has_value ());
  CHECK_FALSE ((code->relocate (UINT64_MAX - 3)).has_value ());
  auto fail = code->relocate (0, [&] (auto &) -> Result<std::uint64_t> {
    return Error{ErrorCode::InvalidArgument, "resolver failure"};
  });
  REQUIRE_FALSE ((fail).has_value ());
  CHECK (fail.error ().message.find ("resolver failure") != std::string::npos);
}
TEST_CASE ("owning encoding folds only explicitly resolved symbol addresses") {
  if (!arm ()) return;
  Fixture t;
  auto s = mirnext::codegen::detail::access (t.f).intern_symbol (
    {"m", "known", MachineSymbol::Kind::Import});
  REQUIRE ((s).has_value ());
  t.add (Opcode::Mov,
         {t.reg (0),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand (
            mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (), 8))});
  t.add (Opcode::Ret, {t.reg (0)});
  auto frame = t.frame ();
  auto unresolved = encode_machine (t.f, frame);
  REQUIRE ((unresolved).has_value ());
  CHECK (unresolved.value ().relocations ().size () == 4);
  auto known
    = encode_machine (t.f, frame,
                      {.known_addresses = [] (auto &) -> Result<std::uint64_t> { return 34; }});
  REQUIRE ((known).has_value ());
  CHECK (known.value ().relocations ().empty ());
  CHECK (word (known.value ().bytes (), 0) == 0xd2800540);
  auto before = t.f.dump ();
  auto fail = encode_machine (t.f, frame, {.known_addresses = [] (auto &) -> Result<std::uint64_t> {
                                return Error{ErrorCode::InvalidOperand, "missing address"};
                              }});
  REQUIRE_FALSE ((fail).has_value ());
  CHECK (t.f.dump () == before);
}
TEST_CASE ("owning builtin relocation preserves typed builtin identity") {
  if (!arm ()) return;
  Fixture t;
  t.add (Opcode::Mov, {t.reg (0), mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                    BuiltinReference{Builtin::CopyBlock})});
  t.add (Opcode::Ret, {t.reg (0)});
  auto code = t.run ();
  unsigned calls = 0;
  auto r = code.relocate (0, [&] (auto &request) -> Result<std::uint64_t> {
    ++calls;
    CHECK_FALSE (mirnext::codegen::detail::NodeAccess::symbol (request));
    REQUIRE (request.builtin);
    CHECK (*request.builtin == Builtin::CopyBlock);
    return 0x12345678;
  });
  REQUIRE ((r).has_value ());
  CHECK (calls == 1);
}
TEST_CASE (
  "owning compiled publication resolves before writing and keeps artifacts and mappings alive") {
  if (!arm ()) return;
  Fixture t;
  auto s = mirnext::codegen::detail::access (t.f).intern_symbol (
    {"external", "value", MachineSymbol::Kind::Data});
  REQUIRE ((s).has_value ());
  auto label = mirnext::codegen::detail::access (t.f).create_label (t.entry, "entry");
  REQUIRE ((label).has_value ());
  t.add (Opcode::Mov,
         {t.reg (0),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand (
            mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (), 9))});
  t.add (Opcode::Ret, {t.reg (0)});
  auto code = t.run ();
  auto provider = std::make_shared<MemoryProvider> ();
  auto failure
    = execution::CodeMemory (provider).publish (code, [] (auto &) -> Result<std::uint64_t> {
        return Error{ErrorCode::UnknownName, "unresolved external"};
      });
  REQUIRE_FALSE ((failure).has_value ());
  CHECK (failure.error ().code == ErrorCode::UnknownName);
  CHECK (failure.error ().message.find ("owned/encode") != std::string::npos);
  CHECK (failure.error ().message.find ("publish") != std::string::npos);
  CHECK (failure.error ().message.find ("unresolved external") != std::string::npos);
  CHECK (provider->allocations == 1);
  CHECK (provider->releases == 1);
  CHECK (provider->writes == 0);
  unsigned calls = 0;
  std::optional<execution::ExecutableFunction> executable;
  {
    execution::CodeMemory memory (provider);
    auto value = memory.publish (std::move (code), [&] (auto &request) -> Result<std::uint64_t> {
      ++calls;
      REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "value");
      CHECK (provider->writes == 0);
      return 33;
    });
    REQUIRE ((value).has_value ());
    executable = std::move ((value).value ());
  }
  CHECK (calls == 1);
  CHECK (provider->allocations == 2);
  CHECK (provider->releases == 1);
  CHECK (provider->writes == 1);
  CHECK (reinterpret_cast<std::int64_t (*) ()> (executable->address ()) () == 42);
  auto anchor = executable->artifact ().find_label ("entry");
  REQUIRE ((anchor).has_value ());
  LabelAddress ref{(anchor).value ()};
  auto addresses = executable->label_addresses ({&ref, 1});
  REQUIRE ((addresses).has_value ());
  CHECK (((addresses).value ())[0] == executable->address ());
  auto copied = executable;
  executable.reset ();
  CHECK (provider->releases == 1);
  CHECK (reinterpret_cast<std::int64_t (*) ()> (copied->address ()) () == 42);
  copied.reset ();
  CHECK (provider->releases == 2);
}
TEST_CASE ("owning label anchors survive machine destruction and reject foreign compiled labels") {
  if (!arm ()) return;
  auto text
    = "m:module\nf:func i64,i64:x\nlocal i64:r\nladdr r,target\nret r\ntarget:label\nret "
      "x\nendfunc\nendmodule\n";
  auto code = compile (text, 0);
  auto target = code.find_label ("target");
  REQUIRE ((target).has_value ());
  auto copy = code;
  std::array refs{LabelAddress{(target).value (), {}, 9},
                  LabelAddress{(target).value (), (target).value (), -4}};
  auto values = code.label_addresses (0x10000, refs);
  REQUIRE ((values).has_value ());
  auto offset = code.labels ()[target.value ().index ()].offset;
  CHECK (((values).value ())[0] == 0x10000 + offset + 9);
  CHECK (((values).value ())[1] == UINT64_MAX - 3);
  CHECK ((copy.label_addresses (0, refs)).has_value ());
  auto other = compile (text, 0);
  CHECK_FALSE ((other.label_addresses (0, refs)).has_value ());
  CHECK_FALSE ((code.find_label ("missing")).has_value ());
}
TEST_CASE ("owning switch tables use local absolute relocations and duplicate label aliases") {
  if (!arm ()) return;
  Fixture t;
  auto &a_block = mirnext::codegen::detail::access (t.f).create_block (),
       &b_block = mirnext::codegen::detail::access (t.f).create_block ();
  auto a_label = mirnext::codegen::detail::access (t.f).create_label (a_block, "a"),
       alias_label = mirnext::codegen::detail::access (t.f).create_label (a_block, "alias"),
       b_label = mirnext::codegen::detail::access (t.f).create_label (b_block, "b");
  REQUIRE ((a_label).has_value ());
  REQUIRE ((alias_label).has_value ());
  REQUIRE ((b_label).has_value ());
  t.add (Opcode::Switch,
         {t.reg (0), mirnext::codegen::detail::NodeAccess::make_MachineOperand ((a_label).value ()),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand ((b_label).value ()),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand ((alias_label).value ())});
  REQUIRE ((t.rw.append (a_block, Opcode::Ret, {t.reg (0)})).has_value ());
  REQUIRE ((t.rw.append (b_block, Opcode::Ret, {t.reg (0)})).has_value ());
  auto code = t.run ();
  auto rebased = code.relocate (0x8000);
  REQUIRE ((rebased).has_value ());
  unsigned count = 0;
  for (auto &r : code.relocations ())
    if (r.kind == CodeRelocation::Kind::LocalAddress) {
      ++count;
      CHECK (word ((rebased).value (), r.offset, 8) == word (code.bytes (), r.offset, 8) + 0x8000);
    }
  CHECK (count == 3);
  auto a = code.find_label ("a"), alias = code.find_label ("alias");
  REQUIRE ((a).has_value ());
  REQUIRE ((alias).has_value ());
  CHECK (code.labels ()[a.value ().index ()].offset
         == code.labels ()[alias.value ().index ()].offset);
}

TEST_CASE ("machine labels reject duplicate source identities without mutating ownership") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  MachineFunction function ("labels", "entry", {{Type::i64 ()}, {}}, (target).value ());
  auto &block = mirnext::codegen::detail::access (function).create_block ();
  auto first = mirnext::codegen::detail::access (function).create_label (block, {}, false, 7);
  REQUIRE ((first).has_value ());
  auto duplicate = mirnext::codegen::detail::access (function).create_label (block, {}, false, 7);
  REQUIRE_FALSE ((duplicate).has_value ());
  CHECK (std::distance (function.labels ().begin (), function.labels ().end ()) == 1);
  CHECK (block.labels ().size () == 1);
  CHECK (first.value ().source_ordinal () == std::optional<std::size_t> (7));
}
TEST_CASE ("owning encoding overflow conditions preserve sub mul and label barriers") {
  if (!arm ()) return;
  for (auto op : {Opcode::Addo, Opcode::Subo, Opcode::Mulo})
    for (bool label : {false, true}) {
      Fixture t;
      t.add (op, {t.reg (0), t.reg (1), t.reg (2)});
      auto &b = mirnext::codegen::detail::access (t.f).create_block ();
      auto l = mirnext::codegen::detail::access (t.f).create_label (b, "exit");
      REQUIRE ((l).has_value ());
      MachineBasicBlock *branch = &t.entry;
      if (label) {
        auto block = t.rw.create_block_after (t.entry);
        REQUIRE ((block).has_value ());
        REQUIRE (
          (mirnext::codegen::detail::access (t.f).create_label ((block).value ())).has_value ());
        branch = &(block).value ();
      }
      auto r
        = t.rw.append (*branch, Opcode::UBo,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((l).value ())});
      REQUIRE ((r).has_value ());
      REQUIRE ((t.rw.append (b, Opcode::Ret, {t.reg (0)})).has_value ());
      auto code = t.run ();
      unsigned condition = 99;
      for (std::size_t n = 0; n < code.bytes ().size (); n += 4)
        if ((word (code.bytes (), n) & 0xff000010) == 0x54000000)
          condition = word (code.bytes (), n) & 15;
      CHECK (condition == (label || op == Opcode::Addo ? 2 : op == Opcode::Subo ? 3 : 1));
    }
}
TEST_CASE ("owning encoding rejects stale foreign frames and invalid final patterns") {
  if (!arm ()) return;
  for (unsigned bad = 0; bad < 4; ++bad) {
    Fixture t, other;
    if (bad == 2) t.add (Opcode::Alloca, {t.reg (0), t.imm (UINT64_MAX)});
    t.add (Opcode::Ret, {t.reg (0)});
    other.add (Opcode::Ret, {other.reg (0)});
    auto frame = t.frame ();
    if (bad == 0)
      REQUIRE ((mirnext::codegen::detail::access (t.f).create_label (t.entry)).has_value ());
    if (bad == 3) {
      auto s = t.f.state ();
      s.frame_formed = false;
      REQUIRE ((t.rw.set_state (s)).has_value ());
    }
    auto &f = bad == 1 ? other.f : t.f;
    auto before = f.dump ();
    auto r = encode_machine (f, frame);
    CHECK_FALSE ((r).has_value ());
    CHECK (f.dump () == before);
  }
  Fixture empty;
  auto framed = empty.frame ();
  CHECK_FALSE ((encode_machine (empty.f, framed)).has_value ());
}
TEST_CASE ("owning AArch64 branch fixups enforce both signed displacement boundaries") {
  for (bool short_branch : {false, true}) {
    auto bits = short_branch ? 19u : 26u;
    auto limit = std::int64_t{1} << (bits + 1);
    CHECK ((aarch64_branch_displacement (-limit, short_branch)).has_value ());
    CHECK ((aarch64_branch_displacement (limit - 4, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (-limit - 4, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (limit, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (1, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (-1, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (INT64_MIN, short_branch)).has_value ());
    CHECK_FALSE ((aarch64_branch_displacement (INT64_MAX, short_branch)).has_value ());
    auto zero = aarch64_branch_displacement (0, short_branch);
    REQUIRE ((zero).has_value ());
    CHECK ((zero).value () == 0);
  }
}
namespace {
std::int64_t encoded_external (std::int64_t x) { return x * 7 + 9; }
}  // namespace
TEST_CASE ("owning native compiled bytes execute directly") {
  const char *texts[] = {
    "m:module\nf:func i64,i64:x\nlocal i64:r\nmul r,x,3\nadd r,r,20\nret r\nendfunc\nendmodule\n",
    "m:module\nf:func i64,i64:x,p:memory\nlocal i64:r\nalloca r,24\nmov i64:(r),x\nmov "
    "r,i64:(r)\nadd r,r,17\nmov i64:(memory),r\nret r\nendfunc\nendmodule\n",
    "m:module\nf:func i64,i64:x\nlocal i64:r,i64:n\nmov r,0\nmov n,x\nloop:label\nadd r,r,n\nsub "
    "n,n,1\nbgt loop,n,0\nret r\nendfunc\nendmodule\n",
    "m:module\nf:func i64,i64:x\nswitch x,a,b,c\na:label\nret 11\nb:label\nret 22\nc:label\nret "
    "33\nendfunc\nendmodule\n",
    "m:module\np:proto i64,i64:x\nimport ext\nf:func i64,i64:x\nlocal i64:r,i64:s\nadd s,x,5\ncall "
    "p,ext,r,x\nadd r,r,s\nret r\nendfunc\nendmodule\n",
    "m:module\nf:func i64,i64:x\nglobal i64:b:r19\nmov b,x\nadd b,b,31\nret "
    "b\nendfunc\nendmodule\n"};
  for (unsigned level = 0; level < 4; ++level)
    for (unsigned kind = 0; kind < std::size (texts); ++kind) {
      CAPTURE (level);
      CAPTURE (kind);
      std::string text (texts[kind]);
      if (!arm () && kind == 5) text.replace (text.find (":r19"), 4, ":r12");
      auto code = compile (text, level, true);
      auto published
        = execution::CodeMemory ()
            .publish (std::move (code), [] (auto &request) -> Result<std::uint64_t> {
              REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
              CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "ext");
              return reinterpret_cast<std::uintptr_t> (&encoded_external);
            });
      INFO (((published).has_value () ? "" : published.error ().message));
      REQUIRE ((published).has_value ());
      auto f = reinterpret_cast<std::int64_t (*) (std::int64_t, std::int64_t *)> (
        published.value ().address ());
      for (std::int64_t x : {0, 1, 2, 33, 257}) {
        if (kind == 3 && x > 2) continue;
        std::int64_t written = -1;
        auto actual = f (x, &written);
        auto expected = kind == 0   ? 3 * x + 20
                        : kind == 1 ? x + 17
                        : kind == 2 ? x * (x + 1) / 2
                        : kind == 3 ? 11 * (x + 1)
                        : kind == 4 ? 8 * x + 14
                                    : x + 31;
        CHECK (actual == expected);
        if (kind == 1) CHECK (written == expected);
      }
    }
}
TEST_CASE ("owning compilation entry preserves the individual machine pipeline at every level") {
  const char *text
    = "m:module\nf:func i64,i64:x\nlocal i64:r,i64:n\nmov r,0\nmov n,x\nloop:label\nadd r,r,n\nsub "
      "n,n,1\nbgt loop,n,0\nret r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    auto c = serialization::parse_text (text);
    REQUIRE ((c).has_value ());
    transforms::OptimizationOptions opt;
    opt.level = static_cast<transforms::OptimizationLevel> (level);
    auto p = transforms::optimize ((c).value (), opt);
    REQUIRE ((p).has_value ());
    auto &f = *p.value ().find_function ("m", "f").get ();
    auto before = p.value ().dump ();
    std::vector<CompilationStage> stages;
    auto compiled
      = compile_function (f, (target::TargetMachine::native ()).value (),
                          {.optimization_level = level,
                           .preserve_host_registers = true,
                           .observer = [&] (const CompilationSnapshot &view) -> Result<void> {
                             stages.push_back (view.stage);
                             CHECK (view.instruction_count ()
                                    == view.function.instruction_count ());
                             CHECK (view.dump () == view.function.dump ());
                             if (view.stage == CompilationStage::Encode) {
                               REQUIRE ((view.compiled).has_value ());
                               REQUIRE ((view.compiled.value ().verify ()).has_value ());
                             } else
                               CHECK_FALSE ((view.compiled).has_value ());
                             return {};
                           }});
    INFO (((compiled).has_value () ? "" : compiled.error ().message));
    REQUIRE ((compiled).has_value ());
    CHECK (p.value ().dump () == before);
    auto direct = compile (text, level, true);
    CHECK (std::equal (compiled.value ().bytes ().begin (), compiled.value ().bytes ().end (),
                       direct.bytes ().begin (), direct.bytes ().end ()));
    CHECK (compiled.value ().relocations ().size () == direct.relocations ().size ());
    std::vector<CompilationStage> expected{CompilationStage::PrepareMachineIR,
                                           CompilationStage::Machinize,
                                           CompilationStage::LegalizeOperands,
                                           CompilationStage::Coalesce,
                                           CompilationStage::Liveness,
                                           CompilationStage::LiveRanges,
                                           CompilationStage::Allocate,
                                           CompilationStage::Rewrite,
                                           CompilationStage::EdgeSpills};
    if (level < 2) expected.push_back (CompilationStage::StripProperties);
    if (level) {
      expected.push_back (CompilationStage::PostLiveness);
      expected.push_back (CompilationStage::Combine);
      expected.push_back (CompilationStage::DeadCode);
    }
    expected.push_back (CompilationStage::Frame);
    expected.push_back (CompilationStage::Encode);
    CHECK (stages == expected);
    (c).value () = mirnext::ir::Program{};
    p = Error{ErrorCode::InvalidArgument, "discard source"};
    auto executable = execution::CodeMemory ().publish (std::move ((compiled).value ()));
    REQUIRE ((executable).has_value ());
    CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable.value ().address ()) (17)
           == 153);
  }
}
TEST_CASE ("owning compilation observers can fail at each stage without mutating the input") {
  auto c = serialization::parse_text (
    "m:module\nf:func i64,i64:x\nlocal i64:r\nadd r,x,2\nret r\nendfunc\nendmodule\n");
  REQUIRE ((c).has_value ());
  auto p = transforms::optimize ((c).value ());
  REQUIRE ((p).has_value ());
  auto &f = *p.value ().find_function ("m", "f").get ();
  auto before = p.value ().dump ();
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  std::vector<CompilationStage> stages;
  auto ok = compile_function (f, (target).value (),
                              {.observer = [&] (const CompilationSnapshot &view) -> Result<void> {
                                stages.push_back (view.stage);
                                return {};
                              }});
  REQUIRE ((ok).has_value ());
  stages.push_back (CompilationStage::StripProperties);
  for (auto stop : stages) {
    unsigned calls = 0;
    bool stopped = false;
    auto result
      = compile_function (f, (target).value (),
                          {.optimization_level
                           = stop == CompilationStage::StripProperties ? 0u : 2u,
                           .observer = [&] (const CompilationSnapshot &view) -> Result<void> {
                             ++calls;
                             CHECK_FALSE (stopped);
                             if (view.stage == stop) {
                               stopped = true;
                               return Error{ErrorCode::UnknownName, "observer stopped"};
                             }
                             return {};
                           }});
    REQUIRE_FALSE ((result).has_value ());
    CHECK (stopped);
    CHECK (calls > 0);
    CHECK (result.error ().code == ErrorCode::UnknownName);
    CHECK (result.error ().message.find ("m/f") != std::string::npos);
    CHECK (result.error ().message.find (target.value ().name ()) != std::string::npos);
    CHECK (result.error ().message.find (stage_name (stop)) != std::string::npos);
    CHECK (result.error ().message.find ("observer stopped") != std::string::npos);
    CHECK (p.value ().dump () == before);
  }
  CHECK ((compile_function (f, (target).value ())).has_value ());
}
TEST_CASE ("owning compilation rejects invalid options targets and unmaterialized input") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  Program c;
  auto m = mirnext::ir::detail::access (c).create_module ("pending");
  REQUIRE ((m).has_value ());
  auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  InstructionBuilder builder ((b).value ());
  auto scope = builder.begin_stack_scope ();
  REQUIRE ((scope).has_value ());
  REQUIRE ((builder.ret (Operand (std::int64_t{42}))).has_value ());
  REQUIRE ((builder.end_stack_scope ((scope).value ())).has_value ());
  auto before = dump (c);
  unsigned calls = 0;
  CompilationOptions options;
  options.observer = [&] (auto &) -> Result<void> {
    ++calls;
    return {};
  };
  options.optimization_level = 4;
  CHECK_FALSE ((compile_function ((f).value (), (target).value (), options)).has_value ());
  CHECK (calls == 0);
  options.optimization_level = 2;
  auto result = compile_function ((f).value (), (target).value (), options);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (calls == 0);
  CHECK (dump (c) == before);
  CHECK (result.error ().message.find ("prepare-machine-ir") != std::string::npos);
  auto p = transforms::optimize (c);
  REQUIRE ((p).has_value ());
  auto compiled
    = compile_function (*p.value ().find_function ("pending", "f").get (), (target).value ());
  REQUIRE ((compiled).has_value ());
}

TEST_CASE (
  "incremental machine compilation owns one body and preserves canonical stage decisions") {
  for (unsigned level = 0; level != 4; ++level) {
    std::vector<CompilationStage> incremental, whole;
    auto make = [&] (std::vector<CompilationStage> &events) -> Result<MachineCompilation> {
      ProgramBuilder builder;
      auto function = builder.module ("steps").function<std::int64_t (std::int64_t)> ("entry");
      function.ret ((function.arg (0) + 1) * 2);
      auto program = builder.finish ();
      if (!(program).has_value ()) return program.error ();
      auto optimized = transforms::optimize (std::move ((program).value ()));
      if (!(optimized).has_value ()) return optimized.error ();
      auto target = target::TargetMachine::native ();
      if (!(target).has_value ()) return target.error ();
      return MachineCompilation::create ((optimized.value ().find_function ("steps", "entry"))
                                           .value (),
                                         (target).value (),
                                         {.optimization_level = level,
                                          .preserve_host_registers = true,
                                          .observer = [&events] (const CompilationSnapshot &view)
                                            -> Result<void> {
                                            events.push_back (view.stage);
                                            return {};
                                          }});
    };
    auto compilation = make (incremental);
    REQUIRE ((compilation).has_value ());
    auto identity = &*compilation.value ().function ().blocks ().begin ();
    MachineCompilation moved (std::move ((compilation).value ()));
    CHECK (&*moved.function ().blocks ().begin () == identity);
    REQUIRE ((moved.run_until (CompilationStage::Allocate)).has_value ());
    REQUIRE ((moved.graph ()).has_value ());
    REQUIRE ((moved.liveness ()).has_value ());
    REQUIRE ((moved.live_ranges ()).has_value ());
    REQUIRE ((moved.allocation ()).has_value ());
    REQUIRE ((moved.step ()).has_value ());
    CHECK (moved.stage () == CompilationStage::Rewrite);
    CHECK_FALSE ((moved.graph ()).has_value ());
    CHECK_FALSE ((moved.liveness ()).has_value ());
    CHECK_FALSE ((moved.live_ranges ()).has_value ());
    CHECK_FALSE ((moved.allocation ()).has_value ());
    while (moved.next_stage ()) REQUIRE ((moved.step ()).has_value ());
    REQUIRE ((moved.frame ()).has_value ());
    auto code = moved.finish ();
    REQUIRE ((code).has_value ());
    CHECK_FALSE ((moved.finish ()).has_value ());
    auto ordinary = make (whole);
    REQUIRE ((ordinary).has_value ());
    auto reference = ordinary.value ().finish ();
    REQUIRE ((reference).has_value ());
    CHECK (whole == incremental);
    CHECK (std::equal (code.value ().bytes ().begin (), code.value ().bytes ().end (),
                       reference.value ().bytes ().begin (), reference.value ().bytes ().end ()));
    auto executable = execution::CodeMemory ().publish (std::move ((code).value ()));
    REQUIRE ((executable).has_value ());
    CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable.value ().address ()) (21)
           == 44);
  }
}
TEST_CASE ("machine compilation edits and observer failures obey stage boundaries") {
  ProgramBuilder builder;
  auto function = builder.module ("steps").function<std::int64_t ()> ("entry");
  function.ret (1);
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto optimized = transforms::optimize (std::move ((program).value ()));
  REQUIRE ((optimized).has_value ());
  unsigned observed = 0;
  auto compilation
    = MachineCompilation::create ((optimized.value ().find_function ("steps", "entry")).value (),
                                  (target).value (),
                                  {.observer
                                   = [&] (const CompilationSnapshot &view) -> Result<void> {
                                    ++observed;
                                    if (view.stage == CompilationStage::LegalizeOperands)
                                      return Error{ErrorCode::InvalidOperand,
                                                   "stop at legalization"};
                                    return {};
                                  }});
  REQUIRE ((compilation).has_value ());
  auto edits = compilation.value ().edit ();
  REQUIRE ((edits).has_value ());
  auto &first = *compilation.value ().function ().blocks ().begin ()->instructions ().begin ();
  auto output = first.operands ()[0].get<MachineVirtualRegister> ();
  REQUIRE ((output).has_value ());
  edits.value ().replace (first, Opcode::Mov, {(output).value (), std::int64_t (42)});
  REQUIRE ((edits.value ().finish ()).has_value ());
  REQUIRE ((compilation.value ().step ()).has_value ());
  CHECK_FALSE ((compilation.value ().edit ()).has_value ());
  auto stopped = compilation.value ().step ();
  REQUIRE_FALSE ((stopped).has_value ());
  CHECK (stopped.error ().message.find ("steps/entry") != std::string::npos);
  CHECK (stopped.error ().message.find ("legalize-operands") != std::string::npos);
  auto before = compilation.value ().function ().dump ();
  CHECK_FALSE ((compilation.value ().step ()).has_value ());
  CHECK_FALSE ((compilation.value ().finish ()).has_value ());
  CHECK (compilation.value ().function ().dump () == before);
  CHECK (observed == 3);
}
