/* Licensed under the MIT License. Check derived uses against authoritative initializers. */
#include "mirnext/IR/Access.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
#include <algorithm>
namespace mirnext::test {
inline void check_data_uses (const ir::Program &context) {
  using namespace ir;
  for (auto &module : context.modules ()) {
    std::unordered_map<const Symbol *, std::vector<const GlobalData *>> symbols;
    std::unordered_map<const Function *, std::vector<const GlobalData *>> labels, expressions;
    for (auto &data : module.data ()) {
      if (auto reference = std::get_if<SymbolReference> (&data.initializer ()))
        symbols[mirnext::ir::detail::NodeAccess::symbol (*(reference))].push_back (&data);
      else if (auto reference = std::get_if<LabelReference> (&data.initializer ()))
        labels[&mirnext::ir::detail::NodeAccess::label (*(reference))->function ()].push_back (
          &data);
      else if (auto reference = std::get_if<ExpressionData> (&data.initializer ()))
        expressions[mirnext::ir::detail::NodeAccess::function (*(reference))].push_back (&data);
    }
    auto check = [] (const auto &map, auto owner, std::span<const GlobalData *const> actual) {
      auto found = map.find (owner);
      if (found == map.end ())
        CHECK (actual.empty ());
      else
        CHECK (std::ranges::equal (actual, found->second));
    };
    for (auto &symbol : module.symbols ())
      check (symbols, &symbol,
             mirnext::ir::detail::ReferenceAccess::pointers (symbol.reference_data_uses ()));
    for (auto &function : module.functions ()) {
      check (labels, &function,
             mirnext::ir::detail::ReferenceAccess::pointers (function.label_data_uses ()));
      check (expressions, &function,
             mirnext::ir::detail::ReferenceAccess::pointers (function.expression_data_uses ()));
    }
  }
}
}  // namespace mirnext::test
