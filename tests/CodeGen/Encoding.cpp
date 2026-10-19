/* Licensed under the MIT License. Owned native bytes, relocations and lifetime. */
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
  return target::TargetMachine::native ()->architecture () == target::Architecture::AArch64;
}
struct Fixture {
  MachineFunction f{"owned", "encode", Signature{{Type::i64 ()}, {}},
                    *target::TargetMachine::native ()};
  MachineRewriter rw{f};
  MachineBasicBlock &entry = f.create_block ();
  Fixture () {
    auto s = f.state ();
    s.machinized = s.operands_legalized = true;
    REQUIRE (rw.set_state (s));
  }
  MachineOperand reg (unsigned n) { return MachineOperand (f.target ().registers ()[n].reg); }
  MachineOperand imm (std::uint64_t n) { return MachineOperand (n); }
  MachineInstruction *add (Opcode code, std::vector<MachineOperand> ops) {
    auto r = rw.append (entry, code, std::move (ops));
    REQUIRE (r);
    return *r;
  }
  MachineFrameResult frame (unsigned level = 0) {
    auto graph = MachineGraphAnalysis::build (f);
    REQUIRE (graph);
    auto live = MachineLiveness::analyze (f, *graph, {.optimization_level = level});
    REQUIRE (live);
    auto ranges = MachineLiveRanges::build (f, *graph, *live);
    REQUIRE (ranges);
    auto a = MachineAllocation::assign (f, *graph, *live, *ranges);
    REQUIRE (a);
    auto r = rewrite_allocated (f, *graph, *live, *ranges, std::move (*a));
    REQUIRE (r);
    auto s = split_allocated_spills (f, std::move (*r));
    REQUIRE (s);
    auto p = optimize_allocated_machine (f, std::move (*s));
    REQUIRE (p);
    auto framed = form_frame (f, std::move (*p));
    REQUIRE (framed);
    return std::move (*framed);
  }
  CompiledFunction run (MachineEncodingOptions options = {}) {
    auto framed = frame ();
    auto before = f.dump ();
    auto revision = f.revision ();
    auto code = encode_machine (f, framed, std::move (options));
    INFO ((code ? "" : code.error ().message));
    REQUIRE (code);
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
    REQUIRE (code->verify ());
    return std::move (*code);
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
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto p = transforms::optimize (**c, options);
  REQUIRE (p);
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  auto f = prepare_machine_ir (*p->context ().modules ().begin ()->functions ().begin (), *target);
  REQUIRE (f);
  c->reset ();  // No source Context survives machine preparation.
  REQUIRE (machinize (**f));
  REQUIRE (legalize_operands (**f));
  auto graph = MachineGraphAnalysis::build (**f);
  REQUIRE (graph);
  auto merged = coalesce_registers (**f, *graph, level);
  REQUIRE (merged);
  graph = MachineGraphAnalysis::build (**f);
  REQUIRE (graph);
  auto live = MachineLiveness::analyze (**f, *graph, {.optimization_level = level});
  REQUIRE (live);
  auto ranges = MachineLiveRanges::build (**f, *graph, *live);
  REQUIRE (ranges);
  auto a = MachineAllocation::assign (**f, *graph, *live, *ranges);
  REQUIRE (a);
  auto r = rewrite_allocated (**f, *graph, *live, *ranges, std::move (*a));
  REQUIRE (r);
  auto s = split_allocated_spills (**f, std::move (*r));
  REQUIRE (s);
  auto post = optimize_allocated_machine (**f, std::move (*s));
  REQUIRE (post);
  auto frame = form_frame (**f, std::move (*post), {.preserve_host_registers = host});
  REQUIRE (frame);
  auto result = encode_machine (**f, *frame);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
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
    auto symbol = t.f.intern_symbol ({"module", "data", MachineSymbol::Kind::Data});
    REQUIRE (symbol);
    t.add (Opcode::Mov, {t.reg (0), MachineOperand (MachineSymbolReference{*symbol, -7})});
    t.add (Opcode::Ret, {t.reg (0)});
    code = t.run ();
  }
  CHECK (code->relocations ().size () == 4);
  CHECK_FALSE (code->relocate (0x1000));
  auto original = std::vector (code->bytes ().begin (), code->bytes ().end ());
  for (auto address : std::array<std::uint64_t, 3>{0, 0x123456789abcdef0ull, UINT64_MAX}) {
    unsigned calls = 0;
    auto bytes = code->relocate (0x1000, [&] (auto &request) -> Result<std::uint64_t> {
      ++calls;
      REQUIRE (request.symbol);
      CHECK (request.symbol->module == "module");
      CHECK (request.symbol->name == "data");
      return address;
    });
    REQUIRE (bytes);
    CHECK (calls == 1);
    std::uint64_t value = 0;
    for (auto &r : code->relocations ())
      value |= ((word (*bytes, r.offset) >> 5) & 65535) << r.shift;
    CHECK (value == address - 7);
    CHECK (std::equal (original.begin (), original.end (), code->bytes ().begin ()));
  }
  CHECK_FALSE (code->relocate (3));
  CHECK_FALSE (code->relocate (UINT64_MAX - 3));
  auto fail = code->relocate (0, [&] (auto &) -> Result<std::uint64_t> {
    return Error{ErrorCode::InvalidArgument, "resolver failure"};
  });
  REQUIRE_FALSE (fail);
  CHECK (fail.error ().message.find ("resolver failure") != std::string::npos);
}
TEST_CASE ("owning encoding folds only explicitly resolved symbol addresses") {
  if (!arm ()) return;
  Fixture t;
  auto s = t.f.intern_symbol ({"m", "known", MachineSymbol::Kind::Import});
  REQUIRE (s);
  t.add (Opcode::Mov, {t.reg (0), MachineOperand (MachineSymbolReference{*s, 8})});
  t.add (Opcode::Ret, {t.reg (0)});
  auto frame = t.frame ();
  auto unresolved = encode_machine (t.f, frame);
  REQUIRE (unresolved);
  CHECK (unresolved->relocations ().size () == 4);
  auto known
    = encode_machine (t.f, frame,
                      {.known_addresses = [] (auto &) -> Result<std::uint64_t> { return 34; }});
  REQUIRE (known);
  CHECK (known->relocations ().empty ());
  CHECK (word (known->bytes (), 0) == 0xd2800540);
  auto before = t.f.dump ();
  auto fail = encode_machine (t.f, frame, {.known_addresses = [] (auto &) -> Result<std::uint64_t> {
                                return Error{ErrorCode::InvalidOperand, "missing address"};
                              }});
  REQUIRE_FALSE (fail);
  CHECK (t.f.dump () == before);
}
TEST_CASE ("owning builtin relocation preserves typed builtin identity") {
  if (!arm ()) return;
  Fixture t;
  t.add (Opcode::Mov, {t.reg (0), MachineOperand (BuiltinReference{Builtin::CopyBlock})});
  t.add (Opcode::Ret, {t.reg (0)});
  auto code = t.run ();
  unsigned calls = 0;
  auto r = code.relocate (0, [&] (auto &request) -> Result<std::uint64_t> {
    ++calls;
    CHECK_FALSE (request.symbol);
    REQUIRE (request.builtin);
    CHECK (*request.builtin == Builtin::CopyBlock);
    return 0x12345678;
  });
  REQUIRE (r);
  CHECK (calls == 1);
}
TEST_CASE (
  "owning compiled publication resolves before writing and keeps artifacts and mappings alive") {
  if (!arm ()) return;
  Fixture t;
  auto s = t.f.intern_symbol ({"external", "value", MachineSymbol::Kind::Data});
  REQUIRE (s);
  auto label = t.f.create_label (t.entry, "entry");
  REQUIRE (label);
  t.add (Opcode::Mov, {t.reg (0), MachineOperand (MachineSymbolReference{*s, 9})});
  t.add (Opcode::Ret, {t.reg (0)});
  auto code = t.run ();
  auto provider = std::make_shared<MemoryProvider> ();
  auto failure
    = execution::CodeMemory (provider).publish (code, [] (auto &) -> Result<std::uint64_t> {
        return Error{ErrorCode::UnknownName, "unresolved external"};
      });
  REQUIRE_FALSE (failure);
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
      REQUIRE (request.symbol);
      CHECK (request.symbol->name == "value");
      CHECK (provider->writes == 0);
      return 33;
    });
    REQUIRE (value);
    executable = std::move (*value);
  }
  CHECK (calls == 1);
  CHECK (provider->allocations == 2);
  CHECK (provider->releases == 1);
  CHECK (provider->writes == 1);
  CHECK (reinterpret_cast<std::int64_t (*) ()> (executable->address ()) () == 42);
  auto anchor = executable->artifact ().find_label ("entry");
  REQUIRE (anchor);
  LabelAddress ref{*anchor};
  auto addresses = executable->label_addresses ({&ref, 1});
  REQUIRE (addresses);
  CHECK ((*addresses)[0] == executable->address ());
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
  REQUIRE (target);
  auto copy = code;
  std::array refs{LabelAddress{*target, {}, 9}, LabelAddress{*target, *target, -4}};
  auto values = code.label_addresses (0x10000, refs);
  REQUIRE (values);
  auto offset = code.labels ()[target->index ()].offset;
  CHECK ((*values)[0] == 0x10000 + offset + 9);
  CHECK ((*values)[1] == UINT64_MAX - 3);
  CHECK (copy.label_addresses (0, refs));
  auto other = compile (text, 0);
  CHECK_FALSE (other.label_addresses (0, refs));
  CHECK_FALSE (code.find_label ("missing"));
}
TEST_CASE ("owning switch tables use local absolute relocations and duplicate label aliases") {
  if (!arm ()) return;
  Fixture t;
  auto &a_block = t.f.create_block (), &b_block = t.f.create_block ();
  auto a_label = t.f.create_label (a_block, "a"), alias_label = t.f.create_label (a_block, "alias"),
       b_label = t.f.create_label (b_block, "b");
  REQUIRE (a_label);
  REQUIRE (alias_label);
  REQUIRE (b_label);
  t.add (Opcode::Switch, {t.reg (0), MachineOperand (**a_label), MachineOperand (**b_label),
                          MachineOperand (**alias_label)});
  REQUIRE (t.rw.append (a_block, Opcode::Ret, {t.reg (0)}));
  REQUIRE (t.rw.append (b_block, Opcode::Ret, {t.reg (0)}));
  auto code = t.run ();
  auto rebased = code.relocate (0x8000);
  REQUIRE (rebased);
  unsigned count = 0;
  for (auto &r : code.relocations ())
    if (r.kind == CodeRelocation::Kind::LocalAddress) {
      ++count;
      CHECK (word (*rebased, r.offset, 8) == word (code.bytes (), r.offset, 8) + 0x8000);
    }
  CHECK (count == 3);
  auto a = code.find_label ("a"), alias = code.find_label ("alias");
  REQUIRE (a);
  REQUIRE (alias);
  CHECK (code.labels ()[a->index ()].offset == code.labels ()[alias->index ()].offset);
}

TEST_CASE ("machine labels reject duplicate source identities without mutating ownership") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  MachineFunction function ("labels", "entry", {{Type::i64 ()}, {}}, *target);
  auto &block = function.create_block ();
  auto first = function.create_label (block, {}, false, 7);
  REQUIRE (first);
  auto duplicate = function.create_label (block, {}, false, 7);
  REQUIRE_FALSE (duplicate);
  CHECK (std::distance (function.labels ().begin (), function.labels ().end ()) == 1);
  CHECK (block.labels ().size () == 1);
  CHECK ((*first)->source_ordinal () == std::optional<std::size_t> (7));
}
TEST_CASE ("owning encoding overflow conditions preserve sub mul and label barriers") {
  if (!arm ()) return;
  for (auto op : {Opcode::Addo, Opcode::Subo, Opcode::Mulo})
    for (bool label : {false, true}) {
      Fixture t;
      t.add (op, {t.reg (0), t.reg (1), t.reg (2)});
      auto &b = t.f.create_block ();
      auto l = t.f.create_label (b, "exit");
      REQUIRE (l);
      MachineBasicBlock *branch = &t.entry;
      if (label) {
        auto block = t.rw.create_block_after (t.entry);
        REQUIRE (block);
        REQUIRE (t.f.create_label (**block));
        branch = *block;
      }
      auto r = t.rw.append (*branch, Opcode::UBo, {MachineOperand (**l)});
      REQUIRE (r);
      REQUIRE (t.rw.append (b, Opcode::Ret, {t.reg (0)}));
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
    if (bad == 0) REQUIRE (t.f.create_label (t.entry));
    if (bad == 3) {
      auto s = t.f.state ();
      s.frame_formed = false;
      REQUIRE (t.rw.set_state (s));
    }
    auto &f = bad == 1 ? other.f : t.f;
    auto before = f.dump ();
    auto r = encode_machine (f, frame);
    CHECK_FALSE (r);
    CHECK (f.dump () == before);
  }
  Fixture empty;
  auto framed = empty.frame ();
  CHECK_FALSE (encode_machine (empty.f, framed));
}
TEST_CASE ("owning AArch64 branch fixups enforce both signed displacement boundaries") {
  for (bool short_branch : {false, true}) {
    auto bits = short_branch ? 19u : 26u;
    auto limit = std::int64_t{1} << (bits + 1);
    CHECK (aarch64_branch_displacement (-limit, short_branch));
    CHECK (aarch64_branch_displacement (limit - 4, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (-limit - 4, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (limit, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (1, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (-1, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (INT64_MIN, short_branch));
    CHECK_FALSE (aarch64_branch_displacement (INT64_MAX, short_branch));
    auto zero = aarch64_branch_displacement (0, short_branch);
    REQUIRE (zero);
    CHECK (*zero == 0);
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
        = execution::CodeMemory ().publish (std::move (code),
                                            [] (auto &request) -> Result<std::uint64_t> {
                                              REQUIRE (request.symbol);
                                              CHECK (request.symbol->name == "ext");
                                              return reinterpret_cast<std::uintptr_t> (
                                                &encoded_external);
                                            });
      INFO ((published ? "" : published.error ().message));
      REQUIRE (published);
      auto f
        = reinterpret_cast<std::int64_t (*) (std::int64_t, std::int64_t *)> (published->address ());
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
    REQUIRE (c);
    transforms::OptimizationOptions opt;
    opt.level = static_cast<transforms::OptimizationLevel> (level);
    auto p = transforms::optimize (**c, opt);
    REQUIRE (p);
    auto &f = *p->find_function ("m", "f");
    auto before = p->dump ();
    std::vector<CompilationStage> stages;
    auto compiled
      = compile_function (f, *target::TargetMachine::native (),
                          {.optimization_level = level,
                           .preserve_host_registers = true,
                           .observer = [&] (const CompilationSnapshot &view) -> Result<void> {
                             stages.push_back (view.stage);
                             CHECK (view.instruction_count ()
                                    == view.function.instruction_count ());
                             CHECK (view.dump () == view.function.dump ());
                             if (view.stage == CompilationStage::Encode) {
                               REQUIRE (view.compiled);
                               REQUIRE (view.compiled->verify ());
                             } else
                               CHECK_FALSE (view.compiled);
                             return {};
                           }});
    INFO ((compiled ? "" : compiled.error ().message));
    REQUIRE (compiled);
    CHECK (p->dump () == before);
    auto direct = compile (text, level, true);
    CHECK (std::equal (compiled->bytes ().begin (), compiled->bytes ().end (),
                       direct.bytes ().begin (), direct.bytes ().end ()));
    CHECK (compiled->relocations ().size () == direct.relocations ().size ());
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
    c->reset ();
    p = Error{ErrorCode::InvalidArgument, "discard source"};
    auto executable = execution::CodeMemory ().publish (std::move (*compiled));
    REQUIRE (executable);
    CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable->address ()) (17) == 153);
  }
}
TEST_CASE ("owning compilation observers can fail at each stage without mutating the input") {
  auto c = serialization::parse_text (
    "m:module\nf:func i64,i64:x\nlocal i64:r\nadd r,x,2\nret r\nendfunc\nendmodule\n");
  REQUIRE (c);
  auto p = transforms::optimize (**c);
  REQUIRE (p);
  auto &f = *p->find_function ("m", "f");
  auto before = p->dump ();
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  std::vector<CompilationStage> stages;
  auto ok = compile_function (f, *target,
                              {.observer = [&] (const CompilationSnapshot &view) -> Result<void> {
                                stages.push_back (view.stage);
                                return {};
                              }});
  REQUIRE (ok);
  stages.push_back (CompilationStage::StripProperties);
  for (auto stop : stages) {
    unsigned calls = 0;
    bool stopped = false;
    auto result
      = compile_function (f, *target,
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
    REQUIRE_FALSE (result);
    CHECK (stopped);
    CHECK (calls > 0);
    CHECK (result.error ().code == ErrorCode::UnknownName);
    CHECK (result.error ().message.find ("m/f") != std::string::npos);
    CHECK (result.error ().message.find (target->name ()) != std::string::npos);
    CHECK (result.error ().message.find (stage_name (stop)) != std::string::npos);
    CHECK (result.error ().message.find ("observer stopped") != std::string::npos);
    CHECK (p->dump () == before);
  }
  CHECK (compile_function (f, *target));
}
TEST_CASE ("owning compilation rejects invalid options targets and unmaterialized input") {
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  Context c;
  auto m = c.create_module ("pending");
  REQUIRE (m);
  auto f = (*m)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (f);
  auto b = (*f)->create_block ();
  REQUIRE (b);
  IRBuilder builder (**b);
  auto scope = builder.begin_stack_scope ();
  REQUIRE (scope);
  REQUIRE (builder.ret (Operand (std::int64_t{42})));
  REQUIRE (builder.end_stack_scope (**scope));
  auto before = dump (c);
  unsigned calls = 0;
  CompilationOptions options;
  options.observer = [&] (auto &) -> Result<void> {
    ++calls;
    return {};
  };
  options.optimization_level = 4;
  CHECK_FALSE (compile_function (**f, *target, options));
  CHECK (calls == 0);
  options.optimization_level = 2;
  auto result = compile_function (**f, *target, options);
  REQUIRE_FALSE (result);
  CHECK (calls == 0);
  CHECK (dump (c) == before);
  CHECK (result.error ().message.find ("prepare-machine-ir") != std::string::npos);
  auto p = transforms::optimize (c);
  REQUIRE (p);
  auto compiled = compile_function (*p->find_function ("pending", "f"), *target);
  REQUIRE (compiled);
}
