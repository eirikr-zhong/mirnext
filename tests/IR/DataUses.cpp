/* Licensed under the MIT License. Stable, checked IR data-reference use lists. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "DataUses.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
Function &function (Module &module, std::string_view name = "entry") {
  auto created = module.create_function (name, {{Type::i64 ()}, {}});
  REQUIRE (created);
  auto block = (*created)->create_block ();
  REQUIRE (block);
  REQUIRE (IRBuilder (**block).ret (Operand (42)));
  return **created;
}
}  // namespace
TEST_CASE ("symbol data uses retain identity through forward definition rename and export") {
  Context context;
  auto m = context.create_module ("data-uses");
  REQUIRE (m);
  auto declaration = (*m)->forward ("target");
  REQUIRE (declaration);
  auto first = (*m)->create_data ("first", SymbolReference{*declaration, -8});
  REQUIRE (first);
  auto second = (*m)->create_data ("", SymbolReference{*declaration, 16});
  REQUIRE (second);
  REQUIRE ((*declaration)->reference_data_uses ().size () == 2);
  CHECK ((*declaration)->reference_data_uses ()[0] == *first);
  CHECK ((*declaration)->reference_data_uses ()[1] == *second);
  SUBCASE ("function") {
    auto declared = (*m)->declare_function ("target", {{Type::i64 ()}, {}});
    REQUIRE (declared);
    CHECK (*declared == *declaration);
    auto definition = (*m)->define_function (**declared);
    REQUIRE (definition);
    auto b = (*definition)->create_block ();
    REQUIRE (b);
    REQUIRE (IRBuilder (**b).ret (Operand (42)));
    CHECK ((*definition)->symbol ().reference_data_uses ().size () == 2);
    CHECK ((*definition)->label_data_uses ().empty ());
    CHECK ((*definition)->expression_data_uses ().empty ());
  }
  SUBCASE ("data") {
    auto declared = (*m)->declare_data ("target");
    REQUIRE (declared);
    CHECK (*declared == *declaration);
    auto definition = (*m)->define_data (**declared, SymbolReference{*declared, 0});
    REQUIRE (definition);
    REQUIRE ((*definition)->symbol ().reference_data_uses ().size () == 3);
    CHECK ((*definition)->symbol ().reference_data_uses ()[2] == *definition);
  }
  REQUIRE ((*m)->rename_symbol (**declaration, "renamed"));
  REQUIRE ((*m)->export_symbol (**declaration));
  auto imported = (*m)->import ("external");
  REQUIRE (imported);
  auto reference = (*m)->create_data ("external_reference", SymbolReference{*imported, 4});
  REQUIRE (reference);
  REQUIRE ((*imported)->reference_data_uses ().size () == 1);
  CHECK ((*imported)->reference_data_uses ()[0] == *reference);
  CHECK (std::get<SymbolReference> ((*first)->initializer ()).symbol == *declaration);
  REQUIRE (context.verify ());
  test::check_data_uses (context);
}
TEST_CASE ("function data uses distinguish label differences expressions and symbol addresses") {
  Context context;
  auto m = context.create_module ("functions");
  REQUIRE (m);
  auto &f = function (**m);
  auto &other = function (**m, "other");
  auto &block = *f.blocks ().begin ();
  auto a = f.create_label (block, "a"), b = f.create_label (block, "b");
  REQUIRE (a);
  REQUIRE (b);
  auto declared_first = (*m)->declare_data ("layout_first"),
       declared_second = (*m)->declare_data ("layout_second");
  REQUIRE (declared_first);
  REQUIRE (declared_second);
  auto second = (*m)->define_data (**declared_second, LabelReference{*a, *a, -4});
  REQUIRE (second);
  auto first = (*m)->define_data (**declared_first, LabelReference{*a, *b, 8});
  REQUIRE (first);
  auto expression = (*m)->create_data ("expression", ExpressionData{&f});
  REQUIRE (expression);
  auto address = (*m)->create_data ("address", SymbolReference{&f.symbol (), 0});
  REQUIRE (address);
  REQUIRE (f.label_data_uses ().size () == 2);
  CHECK (f.label_data_uses ()[0] == *second);
  CHECK (f.label_data_uses ()[1] == *first);
  REQUIRE (f.expression_data_uses ().size () == 1);
  CHECK (f.expression_data_uses ()[0] == *expression);
  REQUIRE (f.symbol ().reference_data_uses ().size () == 1);
  CHECK (f.symbol ().reference_data_uses ()[0] == *address);
  CHECK (other.label_data_uses ().empty ());
  CHECK (other.expression_data_uses ().empty ());
  CHECK (other.symbol ().reference_data_uses ().empty ());
  for (unsigned n = 0; n < 256; ++n) {
    auto &unrelated = function (**m, "f" + std::to_string (n));
    auto label = unrelated.create_label (*unrelated.blocks ().begin ());
    REQUIRE (label);
    REQUIRE ((*m)->create_data ("label" + std::to_string (n), LabelReference{*label}));
    REQUIRE ((*m)->create_data ("value" + std::to_string (n), ZeroData{8}));
  }
  CHECK (f.label_data_uses ()[0] == *second);
  CHECK (f.expression_data_uses ()[0] == *expression);
  REQUIRE (context.verify ());
  test::check_data_uses (context);
  auto original = dump (context);
  auto copy = clone (context);
  REQUIRE (copy);
  REQUIRE ((*copy)->verify ());
  CHECK (dump (**copy) == original);
  test::check_data_uses (**copy);
}
TEST_CASE ("invalid initializers do not publish data uses") {
  Context context;
  auto m = context.create_module ("m"), other = context.create_module ("other");
  REQUIRE (m);
  REQUIRE (other);
  auto &f = function (**m);
  auto &foreign = function (**other);
  auto label = f.create_label (*f.blocks ().begin ()),
       foreign_label = foreign.create_label (*foreign.blocks ().begin ());
  REQUIRE (label);
  REQUIRE (foreign_label);
  const auto items = (*m)->items ().size ();
  const auto revision = (*m)->data_revision ();
  SUBCASE ("foreign symbol") {
    CHECK_FALSE ((*m)->create_data ("bad", SymbolReference{&foreign.symbol (), 0}));
  }
  SUBCASE ("foreign label") {
    CHECK_FALSE ((*m)->create_data ("bad", LabelReference{*foreign_label}));
  }
  SUBCASE ("foreign difference base") {
    CHECK_FALSE ((*m)->create_data ("bad", LabelReference{*label, *foreign_label}));
  }
  SUBCASE ("foreign expression") {
    CHECK_FALSE ((*m)->create_data ("bad", ExpressionData{&foreign}));
  }
  SUBCASE ("effectful expression") {
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    REQUIRE (IRRewriter (f).insert_before (ret, Opcode::Mov,
                                           {Operand (Memory{Type::i64 ()}), Operand (1)}));
    CHECK_FALSE ((*m)->create_data ("bad", ExpressionData{&f}));
  }
  CHECK ((*m)->items ().size () == items);
  CHECK ((*m)->data_revision () == revision);
  CHECK (f.label_data_uses ().empty ());
  CHECK (f.expression_data_uses ().empty ());
  CHECK (f.symbol ().reference_data_uses ().empty ());
  CHECK (foreign.label_data_uses ().empty ());
  CHECK (foreign.expression_data_uses ().empty ());
  CHECK (foreign.symbol ().reference_data_uses ().empty ());
  CHECK_FALSE ((*m)->find_symbol ("bad"));
  test::check_data_uses (context);
}
TEST_CASE ("repeated data definitions do not duplicate a registered use") {
  Context context;
  auto m = context.create_module ("repeat");
  REQUIRE (m);
  auto &f = function (**m);
  auto label = f.create_label (*f.blocks ().begin ());
  REQUIRE (label);
  auto data = (*m)->create_data ("data", LabelReference{*label});
  REQUIRE (data);
  auto revision = (*m)->data_revision ();
  auto items = (*m)->items ().size ();
  auto &symbol = const_cast<Symbol &> ((*data)->symbol ());
  CHECK_FALSE ((*m)->define_data (symbol, LabelReference{*label, *label, 9}));
  REQUIRE (f.label_data_uses ().size () == 1);
  CHECK (f.label_data_uses ()[0] == *data);
  CHECK ((*m)->data_revision () == revision);
  CHECK ((*m)->items ().size () == items);
  test::check_data_uses (context);
}
TEST_CASE ("label data uses protect both anchors through block and label rewrites") {
  Context context;
  auto m = context.create_module ("anchors");
  REQUIRE (m);
  auto &f = function (**m);
  auto target = f.create_block ();
  REQUIRE (target);
  REQUIRE (IRBuilder (**target).ret (Operand (0)));
  auto a = f.create_label (**target, "a"), b = f.create_label (**target, "b");
  REQUIRE (a);
  REQUIRE (b);
  REQUIRE (f.create_label (*f.blocks ().begin (), "unused"));
  auto data = (*m)->create_data ("difference", LabelReference{*a, *b, 4});
  REQUIRE (data);
  IRRewriter rewriter (f);
  auto separated = rewriter.separate_label_anchors (**target);
  REQUIRE (separated);
  CHECK (*separated == 1);
  auto erased = rewriter.erase_unused_labels ();
  REQUIRE (erased);
  CHECK (*erased == 1);
  CHECK (&(*a)->block () != &(*b)->block ());
  CHECK (f.find_label ("a") == *a);
  CHECK (f.find_label ("b") == *b);
  auto before = f.revisions ();
  BasicBlock *removed[] = {const_cast<BasicBlock *> (&(*a)->block ())};
  CHECK_FALSE (rewriter.erase_blocks (removed));
  CHECK (f.revisions () == before);
  REQUIRE (f.label_data_uses ().size () == 1);
  CHECK (f.label_data_uses ()[0] == *data);
  REQUIRE (context.verify ());
  test::check_data_uses (context);
}
TEST_CASE ("cloned data-use relationships survive the complete source lifetime") {
  std::unique_ptr<Context> copied;
  {
    Context source;
    auto m = source.create_module ("source");
    REQUIRE (m);
    auto &f = function (**m);
    auto label = f.create_label (*f.blocks ().begin ());
    REQUIRE (label);
    REQUIRE ((*m)->create_data ("label", LabelReference{*label, *label, 3}));
    REQUIRE ((*m)->create_data ("expression", ExpressionData{&f}));
    REQUIRE ((*m)->create_data ("reference", SymbolReference{&f.symbol (), 2}));
    auto result = clone (source);
    REQUIRE (result);
    copied = std::move (*result);
    auto &copy = *copied->modules ().begin ()->functions ().begin ();
    CHECK (&copy != &f);
    REQUIRE (copy.label_data_uses ().size () == 1);
    REQUIRE (copy.expression_data_uses ().size () == 1);
    CHECK (copy.label_data_uses ()[0] != f.label_data_uses ()[0]);
    CHECK (copy.expression_data_uses ()[0] != f.expression_data_uses ()[0]);
    CHECK (copy.symbol ().reference_data_uses ()[0] != f.symbol ().reference_data_uses ()[0]);
    test::check_data_uses (*copied);
  }
  REQUIRE (copied->verify ());
  test::check_data_uses (*copied);
}
