/* Licensed under the MIT License. Owning BBV encoding and relocation. */
#include "mirnext/CodeGen/Access.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/CompiledBlock.hpp"
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/CodeGen/Detail/CompiledBlock.hpp"
#include "mirnext/CodeGen/Detail/CompiledFunction.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include <deque>
#include <array>
#include <sstream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
auto host () {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  return (target).value ();
}
BBVProgram program (const Program &context, unsigned level) {
  auto optimized
    = transforms::optimize (context, {.level = static_cast<transforms::OptimizationLevel> (level)});
  REQUIRE ((optimized).has_value ());
  auto p = BBVPreparation::prepare (*optimized.value ()
                                       .program ()
                                       .modules ()
                                       .begin ()
                                       ->functions ()
                                       .begin (),
                                    host (), {.optimization_level = level});
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  auto result = BBVProgram::create (std::move ((p).value ()));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
auto program (std::string_view text, unsigned level = 0) {
  auto parsed = serialization::parse_text (text);
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  return program ((parsed).value (), level);
}
std::vector<CompiledBlock> encode (BBVProgram &p) {
  auto properties = BBVProperties::create (p.preparation ().function ());
  REQUIRE ((properties).has_value ());
  auto entry
    = p.request_version (*p.preparation ().blocks ().begin (), (properties).value (), true);
  REQUIRE ((entry).has_value ());
  std::deque<const BBVVersion *> work{
    mirnext::codegen::detail::NodeAccess::version (((entry)).value ())};
  std::vector<const BBVVersion *> seen;
  std::vector<CompiledBlock> blocks;
  while (!work.empty ()) {
    auto version = work.front ();
    work.pop_front ();
    if (std::find (seen.begin (), seen.end (), version) != seen.end ()) continue;
    seen.push_back (version);
    auto before = p.preparation ().function ().dump ();
    auto revision = p.preparation ().function ().revision ();
    auto compiled = encode_bbv (p, *version);
    INFO (((compiled).has_value () ? "" : compiled.error ().message));
    REQUIRE ((compiled).has_value ());
    REQUIRE ((compiled.value ().verify ()).has_value ());
    CHECK (compiled.value ().version () == version->id ());
    CHECK (p.preparation ().function ().revision () == revision);
    CHECK (p.preparation ().function ().dump () == before);
    auto again = encode_bbv (p, *version);
    REQUIRE ((again).has_value ());
    CHECK (std::equal (again.value ().bytes ().begin (), again.value ().bytes ().end (),
                       compiled.value ().bytes ().begin (), compiled.value ().bytes ().end ()));
    for (auto &edge : version->specialization ().get ()->successors ())
      work.push_back (mirnext::codegen::detail::NodeAccess::version (edge));
    blocks.push_back (std::move ((compiled).value ()));
  }
  return blocks;
}
std::vector<execution::ExecutableCode> publish (const std::vector<CompiledBlock> &blocks) {
  std::vector<std::size_t> sizes;
  for (auto &block : blocks) sizes.push_back (block.bytes ().size ());
  auto reservation = execution::CodeMemory ().reserve (sizes);
  REQUIRE ((reservation).has_value ());
  std::vector<std::vector<std::uint8_t>> relocated;
  for (std::size_t n = 0; n < blocks.size (); ++n) {
    auto base = reservation.value ().address (n);
    REQUIRE ((base).has_value ());
    auto bytes = blocks[n].relocate ((base).value (),
                                     [&] (const BlockVersionId &id) -> Result<std::uint64_t> {
                                       for (std::size_t k = 0; k < blocks.size (); ++k)
                                         if (blocks[k].version () == id) {
                                           auto address = reservation.value ().address (k);
                                           if (!(address).has_value ()) return address.error ();
                                           return (address).value ();
                                         }
                                       return Error{ErrorCode::UnknownName, "missing test block"};
                                     });
    INFO (((bytes).has_value () ? "" : bytes.error ().message));
    REQUIRE ((bytes).has_value ());
    relocated.push_back (std::move ((bytes).value ()));
  }
  std::vector<std::span<const std::uint8_t>> views;
  for (auto &bytes : relocated) views.push_back (bytes);
  auto executable = reservation.value ().publish (views);
  REQUIRE ((executable).has_value ());
  return std::move ((executable).value ());
}
std::uint64_t word (std::span<const std::uint8_t> bytes, std::size_t at, unsigned size = 4) {
  std::uint64_t value = 0;
  for (unsigned n = 0; n < size; ++n) value |= std::uint64_t (bytes[at + n]) << (8 * n);
  return value;
}
}  // namespace
TEST_CASE (
  "owning BBV block artifacts encode all public instructions without retaining machine nodes") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto source = test::public_instruction_example (descriptor);
    auto optimized = transforms::optimize (*source, {.level = transforms::OptimizationLevel::O0});
    REQUIRE ((optimized).has_value ());
    for (auto &module : optimized.value ().program ().modules ())
      for (auto &function : module.functions ())
        for (unsigned level = 0; level < 4; ++level) {
          CAPTURE (level);
          auto prepared
            = BBVPreparation::prepare (function, host (), {.optimization_level = level});
          if (host ().abi () == target::ABI::X86Windows
              && function.signature ().results.size () > 1) {
            REQUIRE_FALSE ((prepared).has_value ());
            CHECK (
              prepared.error ().message.find ("Windows does not support multiple return values")
              != std::string::npos);
            continue;
          }
          REQUIRE ((prepared).has_value ());
          auto p = BBVProgram::create (std::move ((prepared).value ()));
          REQUIRE ((p).has_value ());
          auto blocks = encode ((p).value ());
          CHECK_FALSE (blocks.empty ());
          for (auto &block : blocks) REQUIRE ((block.verify ()).has_value ());
        }
  }
}
TEST_CASE (
  "owning BBV fragments execute after source preparation and version owners are destroyed") {
  // This publishes a test batch eagerly. It does not substitute for lazy BBV
  // resolution, register-preserving cold entries or ExecutionSession integration.
  constexpr auto text
    = "m:module\nf:func i64,i64:x\nlocal i64:r\nprset x,7\nprbeq yes,x,7\nmov r,17\nret r\nyes:mov "
      "r,42\nret r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    auto blocks = [&] {
      auto p = program (text, level);
      return encode (p);
    }();
    auto copy = blocks;
    auto executable = publish (copy);
    auto entry = reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable.front ().address ());
    CHECK (entry (0) == 42);
    CHECK (entry (99) == 42);
    blocks.clear ();
    copy.clear ();
    CHECK (entry (-1) == 42);
  }
}
TEST_CASE (
  "owning BBV fragments preserve conditional fallthrough loops and duplicate switch entries") {
  constexpr auto loop
    = "m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,0\nloop:ble done,x,0\nadd r,r,x\nsub "
      "x,x,1\njmp loop\ndone:ret r\nendfunc\nendmodule\n";
  constexpr auto table
    = "m:module\nf:func i64,i64:x\nlocal i64:r\nswitch x,a,b,a\na:mov r,11\nret r\nb:mov r,29\nret "
      "r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    auto p = program (loop, level);
    auto blocks = encode (p);
    auto executable = publish (blocks);
    auto entry = reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable.front ().address ());
    for (auto n : {0, 1, 2, 9, 20}) CHECK (entry (n) == n * (n + 1) / 2);
    auto s = program (table, level);
    auto switches = encode (s);
    auto published = publish (switches);
    auto dispatch
      = reinterpret_cast<std::int64_t (*) (std::int64_t)> (published.front ().address ());
    CHECK (dispatch (0) == 11);
    CHECK (dispatch (1) == 29);
    CHECK (dispatch (2) == 11);
    unsigned tables = 0;
    for (auto &block : switches) {
      std::vector<BlockVersionId> targets;
      for (auto &r : block.successors ())
        if (r.kind == BlockRelocation::Kind::AbsoluteAddress) targets.push_back (r.target);
      if (!targets.empty ()) {
        REQUIRE (targets.size () == 3);
        CHECK (targets[0] == targets[2]);
        CHECK_FALSE (targets[0] == targets[1]);
        ++tables;
      }
    }
    CHECK (tables == 1);
    for (auto &block : switches)
      if (block.successors ().size () == 3) {
        unsigned resolutions = 0;
        auto relocated = block.relocate (0x100000, [&] (auto &) -> Result<std::uint64_t> {
          ++resolutions;
          return 0x200000;
        });
        REQUIRE ((relocated).has_value ());
        CHECK (resolutions == 2);  // duplicate case shares one resolution
      }
  }
}
TEST_CASE ("owning BBV LADDR materializes the selected block entry for indirect control flow") {
  constexpr auto text
    = "m:module\nf:func i64\nlocal p:address,i64:r\nladdr address,target\njmpi address\ntarget:mov "
      "r,73\nret r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    auto p = program (text, level);
    auto blocks = encode (p);
    auto executable = publish (blocks);
    CHECK (reinterpret_cast<std::int64_t (*) ()> (executable.front ().address ()) () == 73);
    if (host ().architecture () == target::Architecture::AArch64)
      for (auto &block : blocks)
        for (auto &r : block.successors ())
          if (r.kind == BlockRelocation::Kind::AArch64Address21) {
            constexpr auto base = 0x400000ull;
            for (auto difference : {-0x100000ll, -4ll, 0ll, 4ll, 0xffffcll}) {
              auto target = base + r.offset + difference;
              auto bytes
                = block.relocate (base, [&] (auto &) -> Result<std::uint64_t> { return target; });
              REQUIRE ((bytes).has_value ());
              auto instruction = word ((bytes).value (), r.offset);
              auto bits = ((instruction >> 29) & 3) | (((instruction >> 5) & 0x7ffff) << 2);
              auto decoded = static_cast<std::int32_t> (bits << 11) >> 11;
              CHECK (decoded == difference);
            }
            CHECK_FALSE ((block.relocate (base,
                                          [&] (auto &) -> Result<std::uint64_t> {
                                            return base + r.offset + 0x100000;
                                          }))
                           .has_value ());
            CHECK_FALSE ((block.relocate (base,
                                          [&] (auto &) -> Result<std::uint64_t> {
                                            return base + r.offset - 0x100004;
                                          }))
                           .has_value ());
          }
  }
}
TEST_CASE (
  "owning BBV far conditional expansion executes both outcomes across separately placed "
  "fragments") {
  constexpr auto text
    = "m:module\nf:func i64,i64:x\nlocal i64:r\nbt a,x\nmov r,7\nret r\na:mov r,9\nret "
      "r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    auto p = program (text, level);
    auto blocks = encode (p);
    auto conditional = std::find_if (blocks.begin (), blocks.end (),
                                     [] (auto &b) { return b.successors ().size () == 2; });
    REQUIRE (conditional != blocks.end ());
    auto far_target = conditional->successors ().front ().target;
    std::vector<std::size_t> slots, sizes;
    for (auto &block : blocks) {
      if (block.version () == far_target) sizes.push_back (2 * 1024 * 1024);
      slots.push_back (sizes.size ());
      sizes.push_back (block.bytes ().size ());
    }
    auto reserved = execution::CodeMemory ().reserve (sizes);
    REQUIRE ((reserved).has_value ());
    std::vector<std::vector<std::uint8_t>> bytes (sizes.size ());
    for (std::size_t n = 0; n < sizes.size (); ++n) bytes[n].resize (sizes[n]);
    for (std::size_t n = 0; n < blocks.size (); ++n) {
      auto base = reserved.value ().address (slots[n]);
      REQUIRE ((base).has_value ());
      auto relocated
        = blocks[n].relocate ((base).value (), [&] (auto &id) -> Result<std::uint64_t> {
            for (std::size_t k = 0; k < blocks.size (); ++k)
              if (blocks[k].version () == id) {
                auto address = reserved.value ().address (slots[k]);
                REQUIRE ((address).has_value ());
                return (address).value ();
              }
            return Error{ErrorCode::UnknownName, "missing far test target"};
          });
      REQUIRE ((relocated).has_value ());
      bytes[slots[n]] = std::move ((relocated).value ());
    }
    std::vector<std::span<const std::uint8_t>> views;
    for (auto &b : bytes) views.push_back (b);
    auto code = reserved.value ().publish (views);
    REQUIRE ((code).has_value ());
    auto entry
      = reinterpret_cast<std::int64_t (*) (std::int64_t)> (((code).value ())[slots[0]].address ());
    CHECK (entry (0) == 7);
    CHECK (entry (1) == 9);
    CHECK (entry (-1) == 9);
  }
}
TEST_CASE (
  "owning BBV symbol relocations retain typed symbols and resolver diagnostics independently") {
  auto blocks = [&] {
    auto p = program (
      "m:module\nf:func i64\nlocal i64:r\nmov r,data\nret r\nendfunc\ndata:bss 8\nendmodule\n");
    return encode (p);
  }();
  auto symbol = std::find_if (blocks.begin (), blocks.end (),
                              [] (auto &b) { return !b.relocations ().empty (); });
  REQUIRE (symbol != blocks.end ());
  for (auto &r : symbol->relocations ()) {
    REQUIRE (r.symbol);
    CHECK (r.symbol->module == "m");
    CHECK (r.symbol->name == "data");
  }
  CHECK_FALSE ((symbol->relocate (0x10000, {})).has_value ());
  for (auto value : std::array<std::uint64_t, 3>{0, 0x123456789abcdef0ull, UINT64_MAX}) {
    unsigned resolutions = 0;
    auto bytes = symbol->relocate (0x10000, {}, [&] (auto &request) -> Result<std::uint64_t> {
      REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "data");
      ++resolutions;
      return value;
    });
    REQUIRE ((bytes).has_value ());
    CHECK (resolutions == 1);
    if (host ().architecture () == target::Architecture::AArch64) {
      std::uint64_t actual = 0;
      for (auto &r : symbol->relocations ())
        actual |= ((word ((bytes).value (), r.offset) >> 5) & 65535) << r.shift;
      CHECK (actual == value);
    } else
      CHECK (word ((bytes).value (), symbol->relocations ().front ().offset, 8) == value);
  }
  auto failed = symbol->relocate (0, {}, [] (auto &) -> Result<std::uint64_t> {
    return Error{ErrorCode::UnknownName, "missing data"};
  });
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (failed.error ().message.find ("missing data") != std::string::npos);
}
TEST_CASE (
  "owning BBV large duplicate switches preserve every operand and resolve each version once") {
  std::ostringstream text;
  text << "m:module\nf:func i64,i64:x\nlocal i64:r\nswitch x";
  constexpr unsigned cases = 4096;
  for (unsigned n = 0; n < cases; ++n) text << (n % 2 ? ",b" : ",a");
  text << "\na:mov r,11\nret r\nb:mov r,29\nret r\nendfunc\nendmodule\n";
  auto p = program (text.str ());
  auto blocks = encode (p);
  auto table = std::find_if (blocks.begin (), blocks.end (),
                             [] (auto &b) { return b.successors ().size () == cases; });
  REQUIRE (table != blocks.end ());
  unsigned resolutions = 0;
  auto bytes = table->relocate (0x100000, [&] (auto &id) -> Result<std::uint64_t> {
    ++resolutions;
    return 0x200000 + id.index () * 16;
  });
  REQUIRE ((bytes).has_value ());
  CHECK (resolutions == 2);
  for (unsigned n = 0; n < cases; ++n) {
    auto &r = table->successors ()[n];
    CHECK (r.kind == BlockRelocation::Kind::AbsoluteAddress);
    CHECK (r.target == table->successors ()[n % 2].target);
    CHECK (word ((bytes).value (), r.offset, 8) == 0x200000 + r.target.index () * 16);
  }
}
TEST_CASE (
  "owning BBV successor relocation checks ranges alignment failures and repeatable rebasing") {
  auto p = program (
    "m:module\nf:func i64,i64:x\nlocal i64:r\nbt a,x\nmov r,7\nret r\na:mov r,9\nret "
    "r\nendfunc\nendmodule\n");
  auto blocks = encode (p);
  auto branch = std::find_if (blocks.begin (), blocks.end (),
                              [] (auto &b) { return b.successors ().size () == 2; });
  REQUIRE (branch != blocks.end ());
  auto original = std::vector (branch->bytes ().begin (), branch->bytes ().end ());
  CHECK_FALSE ((branch->relocate (0x1000, {})).has_value ());
  auto failure = branch->relocate (0x1000, [] (auto &) -> Result<std::uint64_t> {
    return Error{ErrorCode::UnknownName, "missing version"};
  });
  REQUIRE_FALSE ((failure).has_value ());
  CHECK (failure.error ().code == ErrorCode::UnknownName);
  CHECK (failure.error ().message.find ("missing version") != std::string::npos);
  auto huge = branch->relocate (0, [] (auto &) -> Result<std::uint64_t> { return UINT64_MAX; });
  CHECK_FALSE ((huge).has_value ());
  if (host ().architecture () == target::Architecture::AArch64) {
    CHECK_FALSE (
      (branch->relocate (3, [] (auto &) -> Result<std::uint64_t> { return 0x1000; })).has_value ());
    CHECK_FALSE (
      (branch->relocate (0, [] (auto &) -> Result<std::uint64_t> { return 3; })).has_value ());
    for (auto base : {0x4000000ull, UINT64_MAX - 0x4000000ull + 1}) {
      auto bytes = branch->relocate (base, [&] (auto &id) -> Result<std::uint64_t> {
        return base + (id == branch->successors ().front ().target ? 0x200000 : 64);
      });
      REQUIRE ((bytes).has_value ());
      const auto &r = branch->successors ().front ();
      REQUIRE (r.kind == BlockRelocation::Kind::AArch64Branch19);
      CHECK ((word ((bytes).value (), r.offset) >> 5 & 0x7ffff) == 2);
      CHECK ((word ((bytes).value (), r.offset + 8) & 0xfc000000) == 0x14000000);
      CHECK_FALSE (
        (branch->relocate (base,
                           [&] (auto &) -> Result<std::uint64_t> { return base + 0x8000000ull; }))
          .has_value ());
    }
  }
  CHECK (std::equal (original.begin (), original.end (), branch->bytes ().begin (),
                     branch->bytes ().end ()));
  auto other = program ("m:module\nf:func i64\nlocal i64:r\nmov r,1\nret r\nendfunc\nendmodule\n");
  auto props = BBVProperties::create (other.preparation ().function ());
  REQUIRE ((props).has_value ());
  auto foreign = other.request_version (*other.preparation ().blocks ().begin (), (props).value ());
  REQUIRE ((foreign).has_value ());
  CHECK_FALSE (
    (encode_bbv (p, *mirnext::codegen::detail::NodeAccess::version (((foreign)).value ())))
      .has_value ());
  CHECK_FALSE (branch->version ().same_program (
    mirnext::codegen::detail::NodeAccess::version (((foreign)).value ())->id ()));
  auto malformed = [&] (std::vector<BlockRelocation> successors, std::vector<std::uint8_t> bytes) {
    mirnext::codegen::detail::CompiledFunctionData data (std::string (branch->module_name ()),
                                                         std::string (branch->name ()),
                                                         branch->architecture (), branch->frame ());
    data.bytes_ = std::move (bytes);
    auto code = mirnext::codegen::detail::ArtifactBuilder::finish (std::move (data));
    REQUIRE ((code).has_value ());
    return mirnext::codegen::detail::BlockArtifactBuilder::finish (branch->version (),
                                                                   std::move ((code).value ()),
                                                                   std::move (successors));
  };
  auto successors = std::vector (branch->successors ().begin (), branch->successors ().end ());
  auto bad = successors;
  bad[0].target = mirnext::codegen::detail::NodeAccess::version (((foreign)).value ())->id ();
  CHECK_FALSE ((malformed (bad, original)).has_value ());
  bad = successors;
  bad[0].offset = original.size ();
  CHECK_FALSE ((malformed (bad, original)).has_value ());
  bad = successors;
  bad.push_back (bad[0]);
  CHECK_FALSE ((malformed (bad, original)).has_value ());
  bad = successors;
  bad[0].kind = static_cast<BlockRelocation::Kind> (99);
  CHECK_FALSE ((malformed (bad, original)).has_value ());
  if (host ().architecture () == target::Architecture::AArch64) {
    auto missing = original;
    missing[successors[0].offset + 8] = 0;
    CHECK_FALSE ((malformed (successors, missing)).has_value ());
    bad = successors;
    bad[0].next_instruction = 1;
    CHECK_FALSE ((malformed (bad, original)).has_value ());
  } else {
    bad = successors;
    bad[0].next_instruction = 0;
    CHECK_FALSE ((malformed (bad, original)).has_value ());
  }
}
