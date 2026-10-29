/* Licensed under the MIT License. Native binary tests have no engine dependencies. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Serialization/Compression.hpp"
#include "mirnext/Serialization/BinaryProtocol.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/DataUses.hpp"
#include <bit>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::serialization;
namespace {
void roundtrip (const Program &context) {
  test::check_data_uses (context);
  for (auto encoding : {BinaryEncoding::Raw, BinaryEncoding::Compressed}) {
    auto bytes = write_binary (context, encoding);
    INFO (((bytes).has_value () ? "" : bytes.error ().message));
    REQUIRE ((bytes).has_value ());
    auto parsed = read_binary ((bytes).value (), {encoding});
    INFO (((parsed).has_value () ? "" : parsed.error ().message));
    REQUIRE ((parsed).has_value ());
    REQUIRE ((parsed.value ().verify ()).has_value ());
    test::check_data_uses ((parsed).value ());
    auto second = write_binary ((parsed).value (), encoding);
    REQUIRE ((second).has_value ());
    if ((second).value () != (bytes).value ()) {
      auto first_text = write_text (context), second_text = write_text ((parsed).value ());
      INFO (((first_text).has_value () ? (first_text).value () : first_text.error ().message));
      INFO (((second_text).has_value () ? (second_text).value () : second_text.error ().message));
      CHECK ((second).value () == (bytes).value ());
    }
  }
}
struct WireFixture {
  std::vector<std::string> strings;
  std::vector<std::byte> body;
  void byte (unsigned value) { body.push_back (static_cast<std::byte> (value)); }
  void uint (unsigned value) {
    if (value <= 127)
      byte (128 | value);
    else {
      byte (1);
      byte (value);
    }
  }
  void name (std::string value, unsigned tag = protocol::NAME1) {
    auto found = std::find (strings.begin (), strings.end (), value);
    auto number = static_cast<unsigned> (found - strings.begin ());
    if (found == strings.end ()) strings.push_back (std::move (value));
    byte (tag);
    byte (number);
  }
  void begin () {
    name ("module");
    name ("m");
    name ("func");
    name ("f");
    uint (0);
    uint (0);
    byte (protocol::EOI);
  }
  std::vector<std::byte> finish () {
    name ("endfunc");
    name ("endmodule");
    byte (protocol::EOFILE);
    auto content = std::move (body);
    body.clear ();
    uint (1);
    uint (strings.size ());
    for (auto &s : strings) {
      uint (s.size () + 1);
      for (unsigned char c : s) byte (c);
      byte (0);
    }
    body.insert (body.end (), content.begin (), content.end ());
    return std::move (body);
  }
  void instruction (Opcode code) {
    switch (code) {
#define MIRNEXT_OPCODE(name, number, spelling, roles, count, flow) \
  case Opcode::name: uint (number); break;
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
    }
  }
};
}  // namespace
TEST_CASE (
  "native binary preserves every public opcode through checked emission and wire mapping") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = mirnext::test::public_instruction_example (descriptor);
    roundtrip (*context);
    auto bytes = write_binary (*context);
    REQUIRE ((bytes).has_value ());
    auto decoded = read_binary ((bytes).value ());
    REQUIRE ((decoded).has_value ());
    auto &function = *decoded.value ().modules ().begin ()->functions ().begin ();
    if (descriptor.opcode == Opcode::Label)
      CHECK (function.labels ().begin () != function.labels ().end ());
    else {
      bool found = false;
      for (auto &block : function.blocks ())
        for (auto &instruction : block.instructions ())
          if (instruction.opcode () == descriptor.opcode) found = true;
      CHECK (found);
    }
  }
}
TEST_CASE ("native binary roundtrips existing MIR regression programs") {
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE (n);
    std::ifstream file (std::string (MIRNEXT_TEST_SOURCE_DIR) + "/tests/Fixtures/Original/test"
                        + std::to_string (n) + ".mir");
    REQUIRE (file);
    std::string text ((std::istreambuf_iterator<char> (file)), {});
    auto context = parse_text (text);
    REQUIRE ((context).has_value ());
    roundtrip ((context).value ());
  }
}
TEST_CASE ("native binary has owning source independent results and exact floating payloads") {
  std::vector<std::byte> bytes;
  {
    Program context;
    auto module = mirnext::ir::detail::access (context).create_module ("m");
    REQUIRE ((module).has_value ());
    REQUIRE ((mirnext::ir::detail::access (((module)).value ())
                .create_data ("nan", InitializedData{Type (Type::Kind::F64),
                                                     {Operand (std::bit_cast<double> (
                                                       UINT64_C (0xfff8000000000123)))}}))
               .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((module)).value ())
         .create_data ("unsigned", InitializedData{Type (Type::Kind::U64), {Operand (UINT64_MAX)}}))
        .has_value ());
    auto function = mirnext::ir::detail::access (((module)).value ())
                      .create_function ("f", {{Type (Type::Kind::F32)}, {}});
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    REQUIRE ((InstructionBuilder ((block).value ())
                .ret (Operand (std::bit_cast<float> (UINT32_C (0x80000000)))))
               .has_value ());
    auto encoded = write_binary (context);
    REQUIRE ((encoded).has_value ());
    bytes = std::move ((encoded).value ());
    roundtrip (context);
  }
  auto parsed = read_binary (bytes);
  REQUIRE ((parsed).has_value ());
  bytes.clear ();
  bytes.shrink_to_fit ();
  REQUIRE ((parsed.value ().verify ()).has_value ());
  auto &module = *parsed.value ().modules ().begin ();
  auto data = std::get_if<InitializedData> (
    &module.find_symbol ("nan").get ()->data ().get ()->initializer ());
  REQUIRE (data);
  CHECK (std::bit_cast<std::uint64_t> ((data->values[0].get<double> ()).value ())
         == UINT64_C (0xfff8000000000123));
  auto &function = *module.functions ().begin ();
  CHECK (std::bit_cast<std::uint32_t> (
           (function.blocks ().begin ()->instructions ().begin ()->operands ()[0].get<float> ())
             .value ())
         == UINT32_C (0x80000000));
}
TEST_CASE ("binary names and literals share exact terminated bytes without conflating prefixes") {
  const std::string name = "shared_identifier_beyond_small_string_capacity";
  for (bool literal_first : {false, true}) {
    CAPTURE (literal_first);
    Program context;
    auto m = mirnext::ir::detail::access (context).create_module ("m");
    REQUIRE ((m).has_value ());
    // Exercise both insertion orders: a literal in an earlier function may
    // already own the bytes later used by a register declaration.
    const std::array strings{name, name + '\0', name + std::string (2, '\0'), std::string{},
                             std::string (1, '\0')};
    auto make = [&] (std::string_view function_name, std::string_view register_name) {
      auto f = mirnext::ir::detail::access (((m)).value ()).create_function (function_name, {});
      REQUIRE ((f).has_value ());
      auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
      REQUIRE ((b).has_value ());
      auto r = mirnext::ir::detail::access (((f)).value ())
                 .create_register (Type::pointer (), register_name);
      REQUIRE ((r).has_value ());
      InstructionBuilder builder ((b).value ());
      for (auto &value : strings)
        REQUIRE ((builder.assign ((r).value (), Operand (value))).has_value ());
      REQUIRE ((builder.ret ()).has_value ());
    };
    if (literal_first) make ("first", "temporary");
    make ("entry", name);
    for (auto encoding : {BinaryEncoding::Raw, BinaryEncoding::Compressed}) {
      auto bytes = write_binary (context, encoding);
      REQUIRE ((bytes).has_value ());
      if (encoding == BinaryEncoding::Raw) {
        std::size_t at = 0;
        auto number = [&] () -> std::size_t {
          REQUIRE (at < bytes.value ().size ());
          auto tag = std::to_integer<unsigned> (((bytes).value ())[at++]);
          if (tag & 128) return tag & 127;
          REQUIRE (tag >= protocol::U1);
          REQUIRE (tag <= protocol::U8);
          std::uint64_t value = 0;
          for (unsigned n = 0; n <= tag - protocol::U1; ++n) {
            REQUIRE (at < bytes.value ().size ());
            value |= std::uint64_t (std::to_integer<unsigned> (((bytes).value ())[at++]))
                     << (8 * n);
          }
          return value;
        };
        CHECK (number () == protocol::version);
        auto count = number ();
        std::vector<std::string> table;
        for (std::size_t n = 0; n < count; ++n) {
          auto size = number ();
          REQUIRE (size <= bytes.value ().size () - at);
          table.emplace_back (reinterpret_cast<const char *> (bytes.value ().data () + at), size);
          at += size;
        }
        for (auto &value : strings) CHECK (std::count (table.begin (), table.end (), value) == 1);
      }
      auto decoded = read_binary ((bytes).value (), {encoding});
      REQUIRE ((decoded).has_value ());
      std::size_t functions = 0;
      for (auto &f : decoded.value ().modules ().begin ()->functions ()) {
        std::size_t n = 0;
        for (auto &b : f.blocks ())
          for (auto &i : b.instructions ())
            if (i.opcode () == Opcode::Mov) {
              REQUIRE (n < strings.size ());
              auto value
                = mirnext::ir::detail::operand_access (i.operands ()[1]).get<std::string> ();
              REQUIRE (value);
              CHECK (*value == strings[n++]);
            }
        CHECK (n == strings.size ());
        ++functions;
      }
      CHECK (functions == (literal_first ? 2 : 1));
      auto canonical = write_binary ((decoded).value (), encoding);
      REQUIRE ((canonical).has_value ());
      CHECK ((canonical).value () == (bytes).value ());
    }
  }
}
TEST_CASE (
  "native binary preserves typed blocks hardware identity module layout and label relocations") {
  auto parsed = parse_text (R"(m:module
head:i64 42
     i64 17
     import external
     u8 1
refs:lref origin, alias, -2
init:expr exprfn
exprfn:func i64
       ret 42
       endfunc
callee:proto i64, blk4:16(.arg), ...
f:func i64, blk4:16(.arg), ...
  global i64:a:r5, i64:b:r5
  local i64:p, i64:value
origin: alias: mov a, 42
  mov b, 0
  mov p, head
  call callee, external, value, blk4:16(p), 1.0
  ret a
  endfunc
  endmodule
)");
  INFO (((parsed).has_value () ? "" : parsed.error ().message));
  REQUIRE ((parsed).has_value ());
  roundtrip ((parsed).value ());
  auto &module = *parsed.value ().modules ().begin ();
  auto &function = *std::next (module.functions ().begin ());
  auto p = function.find_register ("p").get ();
  REQUIRE (p);
  auto instruction = std::next (function.blocks ().begin ()->instructions ().begin (), 3);
  auto old = *mirnext::ir::detail::operand_access (instruction->operands ()[3]).get<Memory> ();
  old.displacement = 12;
  mirnext::ir::detail::NodeAccess::index (old) = p;
  old.scale = 4;
  REQUIRE (
    (InstructionRewriter (function).replace_operand (*instruction, 3, Operand (old))).has_value ());
  auto snapshot = dump ((parsed).value ());
  auto bytes = write_binary ((parsed).value ());
  REQUIRE ((bytes).has_value ());
  CHECK (dump ((parsed).value ()) == snapshot);
  auto decoded = read_binary ((bytes).value ());
  INFO (((decoded).has_value () ? "" : decoded.error ().message));
  REQUIRE ((decoded).has_value ());
  roundtrip ((decoded).value ());
  auto &decoded_function = *std::next (decoded.value ().modules ().begin ()->functions ().begin ());
  CHECK (decoded_function.signature ().parameters[0] == Type::block (16, Type::BlockClass::Class4));
  bool block_found = false;
  for (auto &block : decoded_function.blocks ())
    for (auto &i : block.instructions ())
      if (i.opcode () == Opcode::Call) {
        auto mem = mirnext::ir::detail::operand_access (i.operands ()[3]).get<Memory> ();
        REQUIRE (mem);
        CHECK (mem->displacement == 0);
        CHECK (mirnext::ir::detail::NodeAccess::index (*(mem)) == nullptr);
        block_found = true;
      }
  CHECK (block_found);
}
TEST_CASE ("native binary encodes every memory address form and aliases") {
  for (unsigned form = 0; form < 7; ++form)
    for (bool aliased : {false, true}) {
      Program context;
      auto module = mirnext::ir::detail::access (context).create_module ("m");
      REQUIRE ((module).has_value ());
      auto function = mirnext::ir::detail::access (((module)).value ())
                        .create_function ("f", {{Type::i64 ()}, {}});
      REQUIRE ((function).has_value ());
      auto a
        = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 (), "a");
      REQUIRE ((a).has_value ());
      auto b
        = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 (), "b");
      REQUIRE ((b).has_value ());
      auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
      REQUIRE ((block).has_value ());
      Memory memory = mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ());
      if (form == 0 || form == 3 || form == 4 || form == 6) memory.displacement = -123;
      if (form == 1 || form == 3 || form == 5 || form == 6)
        mirnext::ir::detail::NodeAccess::base (memory) = &(a).value ();
      if (form == 2 || form == 4 || form == 5 || form == 6) {
        mirnext::ir::detail::NodeAccess::index (memory) = &(b).value ();
        memory.scale = 8;
      }
      if (aliased) {
        memory.alias = "a";
        memory.nonalias = "b";
      }
      REQUIRE ((InstructionBuilder ((block).value ()).ret (Operand (memory))).has_value ());
      roundtrip (context);
    }
}
TEST_CASE ("native formats preserve every block ABI class and implicit return block") {
  for (unsigned classification = 0; classification <= 5; ++classification) {
    CAPTURE (classification);
    Program context;
    auto module = mirnext::ir::detail::access (context).create_module ("m");
    REQUIRE ((module).has_value ());
    auto type = classification == 5
                  ? Type::return_block (16)
                  : Type::block (16, static_cast<Type::BlockClass> (classification));
    Signature signature{{}, {type}, true};
    if (classification != 5) signature.results.push_back (Type::i64 ());
    auto prototype
      = mirnext::ir::detail::access (((module)).value ()).prototype ("signature", signature);
    REQUIRE ((prototype).has_value ());
    auto callee
      = mirnext::ir::detail::access (((module)).value ()).create_function ("callee", signature);
    REQUIRE ((callee).has_value ());
    auto callee_entry = mirnext::ir::detail::access (((callee)).value ()).create_block ();
    REQUIRE ((callee_entry).has_value ());
    if (classification == 5)
      REQUIRE ((InstructionBuilder ((callee_entry).value ()).ret ()).has_value ());
    else
      REQUIRE ((InstructionBuilder ((callee_entry).value ()).ret (Operand (std::int64_t{42})))
                 .has_value ());
    auto caller
      = mirnext::ir::detail::access (((module)).value ()).create_function ("caller", signature);
    REQUIRE ((caller).has_value ());
    auto entry = mirnext::ir::detail::access (((caller)).value ()).create_block ();
    REQUIRE ((entry).has_value ());
    auto result = mirnext::ir::detail::access (((caller)).value ()).create_register (Type::i64 ());
    REQUIRE ((result).has_value ());
    Memory block = mirnext::ir::detail::NodeAccess::
      make_Memory (type, mirnext::ir::detail::ReferenceAccess::pointers (
                           caller.value ().arguments ())[0]);
    block.displacement = -12;
    mirnext::ir::detail::NodeAccess::index (block) = &(result).value ();
    block.scale = 4;
    std::vector<Operand> arguments{Operand ((prototype).value ()),
                                   Operand (callee.value ().symbol ())};
    if (classification != 5) arguments.emplace_back ((result).value ());
    arguments.emplace_back (block);
    arguments.emplace_back (1.0);
    InstructionBuilder builder ((entry).value ());
    REQUIRE ((builder.emit (Opcode::Call, arguments)).has_value ());
    if (classification == 5)
      REQUIRE ((builder.ret ()).has_value ());
    else
      REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
    auto snapshot = dump (context);
    roundtrip (context);
    auto text = write_text (context);
    REQUIRE ((text).has_value ());
    CHECK (dump (context) == snapshot);
    auto decoded_text = parse_text ((text).value ());
    INFO (((decoded_text).has_value () ? "" : decoded_text.error ().message));
    REQUIRE ((decoded_text).has_value ());
    auto bytes = write_binary (context);
    REQUIRE ((bytes).has_value ());
    auto decoded_binary = read_binary ((bytes).value ());
    REQUIRE ((decoded_binary).has_value ());
    for (auto decoded : {&(decoded_text).value (), &(decoded_binary).value ()}) {
      auto &function = *std::next (decoded->modules ().begin ()->functions ().begin ());
      CHECK (function.signature ().parameters[0] == type);
      bool found = false;
      for (auto &b : function.blocks ())
        for (auto &i : b.instructions ())
          if (i.opcode () == Opcode::Call) {
            auto memory
              = mirnext::ir::detail::operand_access (i.operands ()[classification == 5 ? 2 : 3])
                  .get<Memory> ();
            REQUIRE (memory);
            CHECK (memory->type == type);
            CHECK (memory->displacement == 0);
            CHECK (mirnext::ir::detail::NodeAccess::index (*(memory)) == nullptr);
            found = true;
          }
      CHECK (found);
      auto canonical = write_text (*decoded);
      REQUIRE ((canonical).has_value ());
      CHECK ((canonical).value () == (text).value ());
    }
  }
}
TEST_CASE (
  "native binary remaps cross module calls symbol relocations and separate function labels") {
  auto context = parse_text (R"(definitions:module
target:func i64
first: ret 42
       endfunc
       export target
       endmodule
calls:module
      import target
sig:proto i64
f:func i64
  local i64:p, i64:value
label: mov p, target
  call sig, p, value
  ret value
  endfunc
g:func i64
other: ret 17
  endfunc
firstptr:lref label, 8
secondptr:lref other, -8
targetptr:ref target, 16
       endmodule
)");
  INFO (((context).has_value () ? "" : context.error ().message));
  REQUIRE ((context).has_value ());
  roundtrip ((context).value ());
  auto &module = *std::next (context.value ().modules ().begin ());
  auto &function = *module.functions ().begin ();
  auto instruction = function.blocks ().begin ()->instructions ().begin ();
  auto symbol = module.find_symbol ("target").get ();
  REQUIRE (symbol);
  REQUIRE (
    (InstructionRewriter (function)
       .replace_operand (*instruction, 1,
                         Operand (
                           mirnext::ir::detail::NodeAccess::make_SymbolReference (symbol, -8))))
      .has_value ());
  auto snapshot = dump ((context).value ());
  auto bytes = write_binary ((context).value ());
  REQUIRE ((bytes).has_value ());
  CHECK (dump ((context).value ()) == snapshot);
  auto decoded = read_binary ((bytes).value ());
  REQUIRE ((decoded).has_value ());
  roundtrip ((decoded).value ());
}
TEST_CASE ("native compression checks block boundaries dictionary references and checksums") {
  std::mt19937 random (42);
  for (std::size_t count : {0u, 1u, 2047u, 262143u, 262144u, 262145u, 524301u}) {
    CAPTURE (count);
    std::vector<std::byte> bytes (count);
    for (std::size_t n = 0; n < count; ++n)
      bytes[n] = static_cast<std::byte> (n % 13 ? n % 71 : random ());
    auto encoded = compress (bytes);
    REQUIRE ((encoded).has_value ());
    auto decoded = decompress ((encoded).value (), count);
    REQUIRE ((decoded).has_value ());
    CHECK ((decoded).value () == bytes);
    auto corrupt = (encoded).value ();
    corrupt.back () ^= std::byte{1};
    CHECK_FALSE ((decompress (corrupt, count)).has_value ());
    encoded.value ().push_back (std::byte{0});
    CHECK_FALSE ((decompress ((encoded).value (), count)).has_value ());
  }
  std::vector<std::byte> bytes (262145, std::byte{42});
  auto encoded = compress (bytes);
  REQUIRE ((encoded).has_value ());
  CHECK_FALSE ((decompress ((encoded).value (), 262144)).has_value ());
}
TEST_CASE ("native binary rejects truncated hostile and invalid inputs without engine entry") {
  auto context = parse_text ("m:module\nf:func i64\nret 42\nendfunc\nendmodule");
  REQUIRE ((context).has_value ());
  for (auto encoding : {BinaryEncoding::Raw, BinaryEncoding::Compressed}) {
    auto bytes = write_binary ((context).value (), encoding);
    REQUIRE ((bytes).has_value ());
    for (std::size_t n = 0; n < bytes.value ().size (); ++n) {
      auto result = read_binary (std::span ((bytes).value ()).first (n), {encoding, "bad.bmir"});
      REQUIRE_FALSE ((result).has_value ());
      CHECK (result.error ().message.find ("bad.bmir") == 0);
    }
    auto trailing = (bytes).value ();
    trailing.push_back (std::byte{0});
    CHECK_FALSE ((read_binary (trailing, {encoding})).has_value ());
    CHECK_FALSE ((read_binary ((bytes).value (), {encoding, "limit", 1})).has_value ());
    BinaryOptions options;
    options.encoding = encoding;
    options.max_instructions = 0;
    CHECK_FALSE ((read_binary ((bytes).value (), options)).has_value ());
    options.max_instructions = 100;
    options.max_strings = 0;
    CHECK_FALSE ((read_binary ((bytes).value (), options)).has_value ());
  }
  auto raw = write_binary ((context).value (), BinaryEncoding::Raw);
  REQUIRE ((raw).has_value ());
  ((raw).value ())[0] = std::byte{130};
  CHECK_FALSE ((read_binary ((raw).value (), {BinaryEncoding::Raw})).has_value ());
  std::array hostile{std::byte{129}, std::byte{8},   std::byte{255}, std::byte{255},
                     std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
                     std::byte{255}, std::byte{255}};
  CHECK_FALSE ((read_binary (hostile, {BinaryEncoding::Raw})).has_value ());
  CHECK_FALSE ((read_binary ({}, {static_cast<BinaryEncoding> (-1)})).has_value ());
}
TEST_CASE ("native binary rejects hardware register addresses before execution") {
  for (auto opcode : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    CAPTURE (opcode);
    WireFixture fixture;
    fixture.begin ();
    fixture.name ("local");
    fixture.byte (protocol::TYPE + unsigned (protocol::Type::I64));
    fixture.name ("p");
    fixture.byte (protocol::EOI);
    fixture.name ("global");
    fixture.byte (protocol::TYPE + unsigned (protocol::Type::I64));
    fixture.name ("bound");
    fixture.name ("r5");
    fixture.byte (protocol::EOI);
    fixture.instruction (opcode);
    fixture.name ("p", protocol::REG1);
    fixture.name ("bound", protocol::REG1);
    fixture.instruction (Opcode::Ret);
    fixture.byte (protocol::EOI);
    auto bytes = fixture.finish ();
    auto invalid = read_binary (bytes, {BinaryEncoding::Raw, "hardware-address"});
    REQUIRE_FALSE ((invalid).has_value ());
    CHECK (invalid.error ().message.find ("copy it to an ordinary local") != std::string::npos);
  }
}
TEST_CASE ("native binary validates wire roles labels symbols and register ownership") {
  for (unsigned scenario = 0; scenario < 9; ++scenario) {
    CAPTURE (scenario);
    WireFixture fixture;
    fixture.begin ();
    if (scenario == 0) {
      fixture.instruction (Opcode::Ret);
      fixture.byte (protocol::REG1);
      fixture.byte (255);
      fixture.byte (protocol::EOI);
    }
    if (scenario == 1) {
      fixture.instruction (Opcode::Jmp);
      fixture.byte (protocol::LAB4);
      for (unsigned n = 0; n < 4; ++n) fixture.byte (255);
    }
    if (scenario == 2) {
      fixture.instruction (Opcode::Jmp);
      fixture.name ("absent");
    }
    if (scenario == 3) {
      fixture.uint (255);
    }
    if (scenario == 4) {
      fixture.instruction (Opcode::Mov);
      fixture.byte (protocol::I1);
      fixture.byte (1);
      fixture.byte (protocol::I1);
      fixture.byte (2);
    }
    if (scenario == 5) {
      fixture.byte (protocol::LAB1);
      fixture.byte (0);
      fixture.instruction (Opcode::Jmp);
      fixture.byte (protocol::LAB1);
      fixture.byte (0);
      fixture.byte (protocol::LAB1);
      fixture.byte (0);
    }
    if (scenario == 6) {
      fixture.instruction (Opcode::Label);
    }
    if (scenario == 7) {
      fixture.name ("local");
      fixture.byte (protocol::TYPE + unsigned (protocol::Type::Block));
      fixture.name ("a");
      fixture.byte (protocol::EOI);
    }
    if (scenario == 8) {
      fixture.instruction (Opcode::VaStart);
      fixture.byte (protocol::MEM_DISP);
      fixture.byte (protocol::TYPE + unsigned (protocol::Type::Undef));
      fixture.byte (protocol::I1);
      fixture.byte (0);
    }
    fixture.instruction (Opcode::Ret);
    fixture.byte (protocol::EOI);
    auto bytes = fixture.finish ();
    auto invalid = read_binary (bytes, {BinaryEncoding::Raw, "invalid-wire"});
    REQUIRE_FALSE ((invalid).has_value ());
    CHECK (invalid.error ().message.find ("invalid-wire") == 0);
  }
  std::array malformed{std::vector<unsigned>{224, 0}, std::vector<unsigned>{1, 128},
                       std::vector<unsigned>{33, 65, 129}, std::vector<unsigned>{224, 72, 0}};
  for (auto &suffix : malformed) {
    std::vector<std::byte> bytes{std::byte{'M'}, std::byte{'I'}, std::byte{'R'}};
    for (auto byte : suffix) bytes.push_back (static_cast<std::byte> (byte));
    CHECK_FALSE ((decompress (bytes, 1024 * 1024)).has_value ());
  }
}

TEST_CASE ("native formats roundtrip v128 types and literals") {
  auto source = parse_text (R"(m:module
f:func v128<i32x4>
local v128<i32x4>:x
vmov x,v128<i32x4>:0x01000000020000000300000004000000
ret x
endfunc
endmodule
)");
  REQUIRE ((source).has_value ());
  auto text = write_text ((source).value ());
  REQUIRE ((text).has_value ());
  auto text_roundtrip = parse_text ((text).value ());
  REQUIRE ((text_roundtrip).has_value ());
  CHECK ((text_roundtrip.value ().verify ()).has_value ());
  CHECK (dump ((text_roundtrip).value ()).find ("v128<i32x4>") != std::string::npos);
  auto bytes = write_binary ((source).value ());
  REQUIRE ((bytes).has_value ());
  auto binary_roundtrip = read_binary ((bytes).value ());
  REQUIRE ((binary_roundtrip).has_value ());
  CHECK ((binary_roundtrip.value ().verify ()).has_value ());
  CHECK (dump ((binary_roundtrip).value ()).find ("v128<i32x4>") != std::string::npos);
}
