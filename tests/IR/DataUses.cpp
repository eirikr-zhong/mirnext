/* Licensed under the MIT License. Stable, checked IR data-reference use lists. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "DataUses.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
Function &function (Module &module, std::string_view name = "entry") {
  auto created = mirnext::ir::detail::access (module).create_function (name, {{Type::i64 ()}, {}});
  REQUIRE ((created).has_value ());
  auto block = mirnext::ir::detail::access (((created)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  REQUIRE ((InstructionBuilder ((block).value ()).ret (Operand (42))).has_value ());
  return (created).value ();
}
}  // namespace
TEST_CASE ("symbol data uses retain identity through forward definition rename and export") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("data-uses");
  REQUIRE ((m).has_value ());
  auto declaration = mirnext::ir::detail::access (((m)).value ()).forward ("target");
  REQUIRE ((declaration).has_value ());
  auto first
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("first", mirnext::ir::detail::NodeAccess::make_SymbolReference (&(declaration)
                                                                                         .value (),
                                                                                      -8));
  REQUIRE ((first).has_value ());
  auto second
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("", mirnext::ir::detail::NodeAccess::make_SymbolReference (&(declaration)
                                                                                    .value (),
                                                                                 16));
  REQUIRE ((second).has_value ());
  REQUIRE (declaration.value ().reference_data_uses ().size () == 2);
  CHECK (
    mirnext::ir::detail::ReferenceAccess::pointers (declaration.value ().reference_data_uses ())[0]
    == &(first).value ());
  CHECK (
    mirnext::ir::detail::ReferenceAccess::pointers (declaration.value ().reference_data_uses ())[1]
    == &(second).value ());
  SUBCASE ("function") {
    auto declared = mirnext::ir::detail::access (((m)).value ())
                      .declare_function ("target", {{Type::i64 ()}, {}});
    REQUIRE ((declared).has_value ());
    CHECK (&(declared).value () == &(declaration).value ());
    auto definition
      = mirnext::ir::detail::access (((m)).value ()).define_function ((declared).value ());
    REQUIRE ((definition).has_value ());
    auto b = mirnext::ir::detail::access (((definition)).value ()).create_block ();
    REQUIRE ((b).has_value ());
    REQUIRE ((InstructionBuilder ((b).value ()).ret (Operand (42))).has_value ());
    CHECK (definition.value ().symbol ().reference_data_uses ().size () == 2);
    CHECK (definition.value ().label_data_uses ().empty ());
    CHECK (definition.value ().expression_data_uses ().empty ());
  }
  SUBCASE ("data") {
    auto declared = mirnext::ir::detail::access (((m)).value ()).declare_data ("target");
    REQUIRE ((declared).has_value ());
    CHECK (&(declared).value () == &(declaration).value ());
    auto definition
      = mirnext::ir::detail::access (((m)).value ())
          .define_data ((declared).value (),
                        mirnext::ir::detail::NodeAccess::make_SymbolReference (&(declared).value (),
                                                                               0));
    REQUIRE ((definition).has_value ());
    REQUIRE (definition.value ().symbol ().reference_data_uses ().size () == 3);
    CHECK (mirnext::ir::detail::ReferenceAccess::pointers (
             definition.value ().symbol ().reference_data_uses ())[2]
           == &(definition).value ());
  }
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ()).rename_symbol ((declaration).value (), "renamed"))
      .has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).export_symbol ((declaration).value ()))
             .has_value ());
  auto imported = mirnext::ir::detail::access (((m)).value ()).import ("external");
  REQUIRE ((imported).has_value ());
  auto reference
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("external_reference",
                      mirnext::ir::detail::NodeAccess::make_SymbolReference (&(imported).value (),
                                                                             4));
  REQUIRE ((reference).has_value ());
  REQUIRE (imported.value ().reference_data_uses ().size () == 1);
  CHECK (
    mirnext::ir::detail::ReferenceAccess::pointers (imported.value ().reference_data_uses ())[0]
    == &(reference).value ());
  CHECK (mirnext::ir::detail::NodeAccess::symbol (
           std::get<SymbolReference> (first.value ().initializer ()))
         == &(declaration).value ());
  REQUIRE ((context.verify ()).has_value ());
  test::check_data_uses (context);
}
TEST_CASE ("function data uses distinguish label differences expressions and symbol addresses") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("functions");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  auto &other = function ((m).value (), "other");
  auto &block = *f.blocks ().begin ();
  auto a = mirnext::ir::detail::access (f).create_label (block, "a"),
       b = mirnext::ir::detail::access (f).create_label (block, "b");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  auto declared_first = mirnext::ir::detail::access (((m)).value ()).declare_data ("layout_first"),
       declared_second
       = mirnext::ir::detail::access (((m)).value ()).declare_data ("layout_second");
  REQUIRE ((declared_first).has_value ());
  REQUIRE ((declared_second).has_value ());
  auto second
    = mirnext::ir::detail::access (((m)).value ())
        .define_data ((declared_second).value (),
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(a).value (),
                                                                            &(a).value (), -4));
  REQUIRE ((second).has_value ());
  auto first
    = mirnext::ir::detail::access (((m)).value ())
        .define_data ((declared_first).value (),
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(a).value (),
                                                                            &(b).value (), 8));
  REQUIRE ((first).has_value ());
  auto expression
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("expression", mirnext::ir::detail::NodeAccess::make_ExpressionData (&f));
  REQUIRE ((expression).has_value ());
  auto address
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("address",
                      mirnext::ir::detail::NodeAccess::make_SymbolReference (&f.symbol (), 0));
  REQUIRE ((address).has_value ());
  REQUIRE (f.label_data_uses ().size () == 2);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]
         == &(second).value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[1]
         == &(first).value ());
  REQUIRE (f.expression_data_uses ().size () == 1);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.expression_data_uses ())[0]
         == &(expression).value ());
  REQUIRE (f.symbol ().reference_data_uses ().size () == 1);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.symbol ().reference_data_uses ())[0]
         == &(address).value ());
  CHECK (other.label_data_uses ().empty ());
  CHECK (other.expression_data_uses ().empty ());
  CHECK (other.symbol ().reference_data_uses ().empty ());
  for (unsigned n = 0; n < 256; ++n) {
    auto &unrelated = function ((m).value (), "f" + std::to_string (n));
    auto label
      = mirnext::ir::detail::access (unrelated).create_label (*unrelated.blocks ().begin ());
    REQUIRE ((label).has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("label" + std::to_string (n),
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ())))
        .has_value ());
    REQUIRE ((mirnext::ir::detail::access (((m)).value ())
                .create_data ("value" + std::to_string (n), ZeroData{8}))
               .has_value ());
  }
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]
         == &(second).value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.expression_data_uses ())[0]
         == &(expression).value ());
  REQUIRE ((context.verify ()).has_value ());
  test::check_data_uses (context);
  auto original = dump (context);
  auto copy = clone (context);
  REQUIRE ((copy).has_value ());
  REQUIRE ((copy.value ().verify ()).has_value ());
  CHECK (dump ((copy).value ()) == original);
  test::check_data_uses ((copy).value ());
}
TEST_CASE ("invalid initializers do not publish data uses") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("m"),
       other = mirnext::ir::detail::access (context).create_module ("other");
  REQUIRE ((m).has_value ());
  REQUIRE ((other).has_value ());
  auto &f = function ((m).value ());
  auto &foreign = function ((other).value ());
  auto label = mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ()),
       foreign_label
       = mirnext::ir::detail::access (foreign).create_label (*foreign.blocks ().begin ());
  REQUIRE ((label).has_value ());
  REQUIRE ((foreign_label).has_value ());
  const auto items = m.value ().items ().size ();
  const auto revision = m.value ().data_revision ();
  SUBCASE ("foreign symbol") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("bad",
                       mirnext::ir::detail::NodeAccess::make_SymbolReference (&foreign.symbol (),
                                                                              0)))
        .has_value ());
  }
  SUBCASE ("foreign label") {
    CHECK_FALSE ((mirnext::ir::detail::access (((m)).value ())
                    .create_data ("bad", mirnext::ir::detail::NodeAccess::make_LabelReference (
                                           &(foreign_label).value ())))
                   .has_value ());
  }
  SUBCASE ("foreign difference base") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("bad",
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                             &(foreign_label)
                                                                                .value ())))
        .has_value ());
  }
  SUBCASE ("foreign expression") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("bad", mirnext::ir::detail::NodeAccess::make_ExpressionData (&foreign)))
        .has_value ());
  }
  SUBCASE ("effectful expression") {
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    REQUIRE (
      (InstructionRewriter (f)
         .insert_before (ret, Opcode::Mov,
                         {Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ())),
                          Operand (1)}))
        .has_value ());
    CHECK_FALSE ((mirnext::ir::detail::access (((m)).value ())
                    .create_data ("bad", mirnext::ir::detail::NodeAccess::make_ExpressionData (&f)))
                   .has_value ());
  }
  CHECK (m.value ().items ().size () == items);
  CHECK (m.value ().data_revision () == revision);
  CHECK (f.label_data_uses ().empty ());
  CHECK (f.expression_data_uses ().empty ());
  CHECK (f.symbol ().reference_data_uses ().empty ());
  CHECK (foreign.label_data_uses ().empty ());
  CHECK (foreign.expression_data_uses ().empty ());
  CHECK (foreign.symbol ().reference_data_uses ().empty ());
  CHECK_FALSE ((m.value ().find_symbol ("bad")).has_value ());
  test::check_data_uses (context);
}
TEST_CASE ("repeated data definitions do not duplicate a registered use") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("repeat");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  auto label = mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ());
  REQUIRE ((label).has_value ());
  auto data
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("data",
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ()));
  REQUIRE ((data).has_value ());
  auto revision = m.value ().data_revision ();
  auto items = m.value ().items ().size ();
  auto &symbol = const_cast<Symbol &> (data.value ().symbol ());
  CHECK_FALSE (
    (mirnext::ir::detail::access (((m)).value ())
       .define_data (symbol,
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                           &(label).value (), 9)))
      .has_value ());
  REQUIRE (f.label_data_uses ().size () == 1);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]
         == &(data).value ());
  CHECK (m.value ().data_revision () == revision);
  CHECK (m.value ().items ().size () == items);
  test::check_data_uses (context);
}
TEST_CASE ("label data uses protect both anchors through block and label rewrites") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("anchors");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  auto target = mirnext::ir::detail::access (f).create_block ();
  REQUIRE ((target).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (0))).has_value ());
  auto a = mirnext::ir::detail::access (f).create_label ((target).value (), "a"),
       b = mirnext::ir::detail::access (f).create_label ((target).value (), "b");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (f).create_label (*f.blocks ().begin (), "unused")).has_value ());
  auto data
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("difference",
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(a).value (),
                                                                            &(b).value (), 4));
  REQUIRE ((data).has_value ());
  InstructionRewriter rewriter (f);
  auto separated = rewriter.separate_label_anchors ((target).value ());
  REQUIRE ((separated).has_value ());
  CHECK ((separated).value () == 1);
  auto erased = rewriter.erase_unused_labels ();
  REQUIRE ((erased).has_value ());
  CHECK ((erased).value () == 1);
  CHECK (&a.value ().block () != &b.value ().block ());
  CHECK (f.find_label ("a").get () == &(a).value ());
  CHECK (f.find_label ("b").get () == &(b).value ());
  auto before = f.revisions ();
  BasicBlock *removed[] = {const_cast<BasicBlock *> (&a.value ().block ())};
  CHECK_FALSE ((rewriter.erase_blocks (removed)).has_value ());
  CHECK (f.revisions () == before);
  REQUIRE (f.label_data_uses ().size () == 1);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]
         == &(data).value ());
  REQUIRE ((context.verify ()).has_value ());
  test::check_data_uses (context);
}
TEST_CASE ("cloned data-use relationships survive the complete source lifetime") {
  std::unique_ptr<Program> copied;
  {
    Program source;
    auto m = mirnext::ir::detail::access (source).create_module ("source");
    REQUIRE ((m).has_value ());
    auto &f = function ((m).value ());
    auto label = mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ());
    REQUIRE ((label).has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("label",
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                             &(label).value (), 3)))
        .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("expression", mirnext::ir::detail::NodeAccess::make_ExpressionData (&f)))
        .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ())
         .create_data ("reference",
                       mirnext::ir::detail::NodeAccess::make_SymbolReference (&f.symbol (), 2)))
        .has_value ());
    auto result = clone (source);
    REQUIRE ((result).has_value ());
    copied = std::make_unique<Program> (std::move ((result).value ()));
    auto &copy = *copied->modules ().begin ()->functions ().begin ();
    CHECK (&copy != &f);
    REQUIRE (copy.label_data_uses ().size () == 1);
    REQUIRE (copy.expression_data_uses ().size () == 1);
    CHECK (mirnext::ir::detail::ReferenceAccess::pointers (copy.label_data_uses ())[0]
           != mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]);
    CHECK (mirnext::ir::detail::ReferenceAccess::pointers (copy.expression_data_uses ())[0]
           != mirnext::ir::detail::ReferenceAccess::pointers (f.expression_data_uses ())[0]);
    CHECK (
      mirnext::ir::detail::ReferenceAccess::pointers (copy.symbol ().reference_data_uses ())[0]
      != mirnext::ir::detail::ReferenceAccess::pointers (f.symbol ().reference_data_uses ())[0]);
    test::check_data_uses (*copied);
  }
  REQUIRE ((copied->verify ()).has_value ());
  test::check_data_uses (*copied);
}
