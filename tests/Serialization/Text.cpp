/* Licensed under the MIT License. Text format tests. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/DataUses.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::serialization;

namespace {
void roundtrip (const Context &source) {
  test::check_data_uses (source);
  auto text = write_text (source);
  REQUIRE (text);
  auto parsed = parse_text (*text);
  INFO ((*text));
  REQUIRE (parsed);
  REQUIRE ((*parsed)->verify ());
  test::check_data_uses (**parsed);
  auto second = write_text (**parsed);
  REQUIRE (second);
  CHECK (*second == *text);
}
Operand integer (std::int64_t value) { return Operand (value); }
}  // namespace
TEST_CASE ("native text canonicalizes unsigned immediate bits without changing binary modes") {
  Context context;
  auto module = context.create_module ("bits");
  REQUIRE (module);
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  auto value = builder.binary (Opcode::Xor, Operand (UINT64_MAX), Operand (UINT64_C (1) << 63));
  REQUIRE (value);
  REQUIRE (builder.ret (Operand (**value)));
  REQUIRE (context.verify ());
  auto text = write_text (context);
  REQUIRE (text);
  CHECK (text->find ("-1, -9223372036854775808") != std::string::npos);
  roundtrip (context);
  auto bytes = write_binary (context);
  REQUIRE (bytes);
  auto decoded = read_binary (*bytes);
  REQUIRE (decoded);
  auto &instruction = *(*decoded)
                         ->modules ()
                         .begin ()
                         ->functions ()
                         .begin ()
                         ->blocks ()
                         .begin ()
                         ->instructions ()
                         .begin ();
  REQUIRE (instruction.operands ()[1].get<std::uint64_t> ());
  CHECK (*instruction.operands ()[1].get<std::uint64_t> () == UINT64_MAX);
  REQUIRE (instruction.operands ()[2].get<std::uint64_t> ());
  CHECK (*instruction.operands ()[2].get<std::uint64_t> () == (UINT64_C (1) << 63));
}
TEST_CASE ("native text reads MIR regression corpus into canonical owned objects") {
  for (unsigned n = 1; n <= 16; ++n) {
    auto path = std::string (MIRNEXT_TEST_SOURCE_DIR) + "/tests/Fixtures/Original/test"
                + std::to_string (n) + ".mir";
    CAPTURE (path);
    std::ifstream stream (path);
    REQUIRE (stream);
    std::string input ((std::istreambuf_iterator<char> (stream)), {});
    auto parsed = parse_text (input, {path});
    INFO ((parsed ? "" : parsed.error ().message));
    REQUIRE (parsed);
    input.clear ();
    input.shrink_to_fit ();
    REQUIRE ((*parsed)->verify ());
    roundtrip (**parsed);
  }
}
TEST_CASE ("native text roundtrips each public opcode with validated operands") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = mirnext::test::public_instruction_example (descriptor);
    auto text = write_text (*context);
    REQUIRE (text);
    auto parsed = parse_text (*text);
    INFO ((*text));
    INFO ((parsed ? "" : parsed.error ().message));
    REQUIRE (parsed);
    roundtrip (**parsed);
    auto &function = *(*parsed)->modules ().begin ()->functions ().begin ();
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
TEST_CASE ("native floating parsing ignores locale and preserves canonical nonfinite values") {
  struct CommaLocale : std::numpunct<char> {
    char do_decimal_point () const override { return ','; }
  };
  auto original = std::locale ();
  std::locale::global (std::locale (std::locale::classic (), new CommaLocale));
  CHECK (std::use_facet<std::numpunct<char>> (std::locale ()).decimal_point () == ',');
  auto parsed = parse_text (
    "m:module\nf:func d\nret 1.25\nendfunc\ng:func ld\nret 2.5L\nendfunc\nh:func f\nret "
    "3.75f\nendfunc\nendmodule");
  std::locale::global (original);
  REQUIRE (parsed);
  roundtrip (**parsed);
  auto &f = *(*parsed)->modules ().begin ()->functions ().begin ();
  CHECK (*f.blocks ().begin ()->instructions ().begin ()->operands ()[0].get<double> () == 1.25);
  auto functions = (*parsed)->modules ().begin ()->functions ();
  auto it = functions.begin ();
  ++it;
  CHECK (*it->blocks ().begin ()->instructions ().begin ()->operands ()[0].get<long double> ()
         == 2.5L);
  ++it;
  CHECK (*it->blocks ().begin ()->instructions ().begin ()->operands ()[0].get<float> () == 3.75f);
  for (auto type :
       {Type (Type::Kind::F32), Type (Type::Kind::F64), Type (Type::Kind::LongDouble)}) {
    for (bool negative : {false, true})
      for (bool nan : {false, true}) {
        Context context;
        auto module = context.create_module ("m");
        REQUIRE (module);
        auto function = (*module)->create_function ("f", {{type}, {}});
        REQUIRE (function);
        auto block = (*function)->create_block ();
        REQUIRE (block);
        auto number = [negative, nan]<class T> () {
          return std::copysign (nan ? std::numeric_limits<T>::quiet_NaN ()
                                    : std::numeric_limits<T>::infinity (),
                                negative ? T{-1} : T{1});
        };
        Operand value = type.kind () == Type::Kind::F32
                          ? Operand (number.template operator()<float> ())
                        : type.kind () == Type::Kind::F64
                          ? Operand (number.template operator()<double> ())
                          : Operand (number.template operator()<long double> ());
        REQUIRE ((*module)->create_data ("inf", InitializedData{type, {value}}));
        REQUIRE (IRBuilder (**block).ret (value));
        roundtrip (context);
      }
  }
}
TEST_CASE ("native text preserves module data order forward labels and expression references") {
  constexpr auto text = R"(m: module
head: u8 1, 2
      bss 3
      import host
      u8 4
ptr:  ref fn, 8
lab:  lref left, right, -2
expr: expr fn
fn:   func i64
left: right: ret 42
      endfunc
      export fn
      endmodule
)";
  auto parsed = parse_text (text);
  INFO ((parsed ? "" : parsed.error ().message));
  REQUIRE (parsed);
  auto &module = *(*parsed)->modules ().begin ();
  REQUIRE (module.items ().size () == 9);
  CHECK (std::holds_alternative<const GlobalData *> (module.items ()[0]));
  CHECK (std::holds_alternative<const GlobalData *> (module.items ()[1]));
  CHECK (std::holds_alternative<const Symbol *> (module.items ()[2]));
  CHECK (std::holds_alternative<const GlobalData *> (module.items ()[3]));
  auto &function = *module.functions ().begin ();
  CHECK (function.labels ().begin ()->block ().labels ().size () == 2);
  auto output = write_text (**parsed);
  REQUIRE (output);
  CHECK (output->find (".boundary") != output->npos);
  roundtrip (**parsed);
  auto clone_result = clone (**parsed);
  REQUIRE (clone_result);
  CHECK (dump (**clone_result) == dump (**parsed));
}
TEST_CASE ("native data declaration fills its original module slot with stable symbol identity") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto symbol = (*module)->reserve_symbol ("data");
  REQUIRE (symbol);
  CHECK ((*module)->items ().empty ());
  auto declaration = (*module)->declare_data ("data");
  REQUIRE (declaration);
  CHECK (*declaration == *symbol);
  REQUIRE_FALSE (context.verify ());
  REQUIRE ((*module)->prototype ("between", {}));
  REQUIRE ((*module)->create_data ("", ZeroData{4}));
  auto data = (*module)->define_data (**declaration, InitializedData{Type::i64 (), {integer (42)}});
  REQUIRE (data);
  CHECK (&(*data)->symbol () == *symbol);
  REQUIRE (context.verify ());
  CHECK (std::get<const GlobalData *> ((*module)->items ()[0]) == *data);
  CHECK (std::holds_alternative<const Symbol *> ((*module)->items ()[1]));
  roundtrip (context);
}
TEST_CASE ("native text handles hardware identity block classes varargs and special memory") {
  auto parsed = parse_text (R"(m: module
p: proto i64, blk4:16(.a), ...
f: func i64, blk4:16(.a), ...
   global i64:a:r5, i64:b:r5
   local i64:va, i64:v
   mov a, 42
   mov b, 0
   va_start undef:(va)
   va_arg v, va, d:0
   va_end va
   ret a
   endfunc
   endmodule
)");
  INFO ((parsed ? "" : parsed.error ().message));
  REQUIRE (parsed);
  auto &function = *(*(*parsed)->modules ().begin ()).functions ().begin ();
  CHECK (function.find_register ("a") == function.find_register ("b"));
  CHECK (function.signature ().parameters[0] == Type::block (16, Type::BlockClass::Class4));
  roundtrip (**parsed);
}
TEST_CASE ("native text materializes relocated memory without mutating write source") {
  auto parsed = parse_text (R"(m: module
d: i64 42
f: func i64
   local i64:base, i64:value
   mov value, i64:d(base)
   ret value
   endfunc
   endmodule
)");
  INFO ((parsed ? "" : parsed.error ().message));
  REQUIRE (parsed);
  auto &module = *(*parsed)->modules ().begin ();
  auto &function = *module.functions ().begin ();
  CHECK (function.instruction_count () == 4);
  auto assignment = function.blocks ().begin ()->instructions ().begin ();
  CHECK (assignment->opcode () == Opcode::Mov);
  CHECK (assignment->operands ()[1].get<SymbolReference> () != nullptr);
  auto symbol = module.find_symbol ("d");
  REQUIRE (symbol);
  auto rewritten
    = IRRewriter (function).replace_operand (*assignment, 1, Operand (SymbolReference{symbol, 8}));
  REQUIRE (rewritten);
  auto before = dump (**parsed);
  auto revisions = function.revisions ();
  auto output = write_text (**parsed);
  REQUIRE (output);
  CHECK (output->find (".addr") != output->npos);
  CHECK (dump (**parsed) == before);
  CHECK (function.revisions () == revisions);
  auto decoded = parse_text (*output);
  INFO ((decoded ? "" : decoded.error ().message));
  REQUIRE (decoded);
  roundtrip (**decoded);
}
TEST_CASE ("native text roundtrips floating bits strings and reports unrepresentable NaN payload") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto function = (*module)->create_function ("f", {{Type (Type::Kind::F64)}, {}});
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  auto result = builder.ret (Operand (-0.0));
  REQUIRE (result);
  auto output = write_text (context);
  REQUIRE (output);
  auto parsed = parse_text (*output);
  REQUIRE (parsed);
  auto &decoded_function = *(*(*parsed)->modules ().begin ()).functions ().begin ();
  auto value
    = decoded_function.blocks ().begin ()->instructions ().begin ()->operands ()[0].get<double> ();
  REQUIRE (value);
  CHECK (std::bit_cast<std::uint64_t> (*value) == UINT64_C (0x8000000000000000));
  REQUIRE (IRRewriter (**function)
             .replace_operand (**result, 0,
                               Operand (std::bit_cast<double> (UINT64_C (0x7ff8000000000123)))));
  auto unrepresentable = write_text (context);
  REQUIRE_FALSE (unrepresentable);
  CHECK (unrepresentable.error ().message.find ("NaN payload") != std::string::npos);
  auto bytes = parse_text ("m:module\nd:string \"A\\000B\\x00\"\ne:string \"\"\nendmodule\n");
  REQUIRE (bytes);
  roundtrip (**bytes);
  auto &data = *(*bytes)->modules ().begin ()->data ().begin ();
  auto initializer = std::get_if<InitializedData> (&data.initializer ());
  REQUIRE (initializer);
  CHECK (initializer->values.size () == 4);
}
TEST_CASE ("native text returns bounded contextual diagnostics for malformed input") {
  for (auto input :
       {"m:module\nf:func i64\nret missing\nendfunc\nendmodule",
        "m:module\nf:func i64\njmp absent\nendfunc\nendmodule",
        "m:module\nf:func i64\nret 1,\nendfunc\nendmodule", "m:module\nx:u8 1\nx:u8 2\nendmodule",
        "m:module\nf:func ...\nret\nendfunc\nendmodule",
        "m:module\nf:func\nphi\nendfunc\nendmodule", "m:module\nx:string \"bad\\x0z\"\nendmodule",
        "m:module\nx:u64 18446744073709551616\nendmodule", "m:module\nf:func\nret\n",
        "m:module\nf:func\nlocal i64:x\nmulo x, 1, 2\nubo end\nend: ret\nendfunc\nendmodule",
        "m:module\nf:func\njret 0\nret\nendfunc\nendmodule",
        "m:module\nf:func i64\nret 0\nendfunc\nx:expr f\nexport unknown\nendmodule"}) {
    CAPTURE (input);
    auto failed = parse_text (input, {"malformed.mir"});
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("malformed.mir:") == 0);
  }
  ParseOptions options;
  options.max_tokens = 2;
  CHECK_FALSE (parse_text ("m: module\nendmodule", options));
  options.max_tokens = 100;
  options.max_instructions = 0;
  CHECK_FALSE (parse_text ("m:module\nf:func\nret\nendfunc\nendmodule", options));
  CHECK_FALSE (parse_text (std::string_view ("m:module\0endmodule", 18)));
}
