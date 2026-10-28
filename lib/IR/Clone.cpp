/* Licensed under the MIT License. Two-pass typed remapping; no runtime objects. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/IR.hpp"
#include <unordered_map>

namespace mirnext::ir {
class IRCloner {
 public:
  static Result<Program> run (std::span<const Module *const> input) {
    auto sources = input | std::views::transform ([] (const Module *module) -> const Module & {
                     return *module;
                   });
    for (auto &module : sources) {
      for (auto &function : module.functions ()) {
        auto phase = function.require_register_ir ();
        if (!(phase).has_value ()) return phase.error ();
      }
      auto valid = module.verify ();
      if (!(valid).has_value ()) return valid.error ();
    }
    Program result;
    std::unordered_map<const Module *, Module *> modules;
    std::unordered_map<const Symbol *, Symbol *> symbols;
    std::unordered_map<const Function *, Function *> functions;
    std::unordered_map<const Register *, const Register *> registers;
    std::unordered_map<const BasicBlock *, BasicBlock *> blocks;
    std::unordered_map<const Label *, const Label *> labels;
    std::unordered_map<const StackScope *, StackScope *> scopes;
    std::unordered_map<const GlobalData *, const GlobalData *> data;
    std::unordered_map<const MemoryLocation *, const MemoryLocation *> locations;
    for (auto &old_module : sources) {
      auto created = detail::access (result).create_module (old_module.name ());
      if (!(created).has_value ()) return created.error ();
      auto &module = (created).value ();
      modules.emplace (&old_module, &module);
      for (auto &symbol : old_module.symbols ()) {
        if (symbol.kind () == Symbol::Kind::Data && symbol.name ().empty ()) continue;
        Result<Symbol &> copied
          = symbol.kind () == Symbol::Kind::Prototype
              ? detail::access (module).prototype (symbol.name (), *symbol.signature ().get ())
            : symbol.kind () == Symbol::Kind::Import
              ? detail::access (module).import (symbol.name (),
                                                symbol.signature ().get ()
                                                  ? std::optional (*symbol.signature ().get ())
                                                  : std::nullopt)
            : symbol.kind () == Symbol::Kind::Function
              ? detail::access (module).declare_function (symbol.name (),
                                                          *symbol.signature ().get ())
              : detail::access (module).forward (symbol.name ());
        if (!(copied).has_value ()) return copied.error ();
        symbols.emplace (&symbol, &(copied).value ());
      }
      for (auto &old : old_module.functions ()) {
        std::vector<std::string_view> names;
        bool all_named = true;
        for (auto reg : detail::ReferenceAccess::pointers (old.arguments ())) {
          names.push_back (reg->name ());
          all_named &= !reg->name ().empty ();
        }
        auto copied
          = detail::access (module).define_function (*symbols.at (&old.symbol ()),
                                                     all_named
                                                       ? std::span<const std::string_view> (names)
                                                       : std::span<const std::string_view>{});
        if (!(copied).has_value ()) return copied.error ();
        auto &function = (copied).value ();
        functions.emplace (&old, &function);
        for (std::size_t n = 0; n < detail::ReferenceAccess::pointers (old.arguments ()).size ();
             ++n)
          registers.emplace (detail::ReferenceAccess::pointers (old.arguments ())[n],
                             detail::ReferenceAccess::pointers (function.arguments ())[n]);
        for (auto &reg : old.registers ())
          if (!reg.is_argument ()) {
            auto mapped = detail::access (function).create_register (reg.type (), reg.name (),
                                                                     reg.hardware_binding ());
            if (!(mapped).has_value ()) return mapped.error ();
            registers.emplace (&reg, &(mapped).value ());
          }
        for (auto &reg : old.registers ())
          const_cast<Register *> (registers.at (&reg))->memory_backed_ = reg.is_memory_backed ();
        // Preserve aliases of an interned hardware register without declaring a second binding.
        for (auto &[name, reg] : old.register_names_)
          function.register_names_.insert_or_assign (module.program ().intern_string (name),
                                                     const_cast<Register *> (registers.at (reg)));
        for (auto &scope : old.stack_scopes ()) {
          auto copied_scope
            = detail::access (function).create_stack_scope (scope.parent ().get ()
                                                              ? scopes.at (scope.parent ().get ())
                                                              : nullptr,
                                                            scope.name ());
          if (!(copied_scope).has_value ()) return copied_scope.error ();
          scopes.emplace (&scope, &(copied_scope).value ());
        }
        for (auto &block : old.blocks ()) {
          auto mapped
            = detail::access (function).create_block (block.name (),
                                                      block.stack_scope ().get ()
                                                        ? scopes.at (block.stack_scope ().get ())
                                                        : nullptr);
          if (!(mapped).has_value ()) return mapped.error ();
          blocks.emplace (&block, &(mapped).value ());
        }
        for (auto &scope : old.stack_scopes ())
          scopes.at (&scope)->materialized_ = scope.materialized ();
        for (auto &label : old.labels ()) {
          auto mapped
            = detail::access (function).create_label (*blocks.at (&label.block ()), label.name ());
          if (!(mapped).has_value ()) return mapped.error ();
          labels.emplace (&label, &(mapped).value ());
        }
      }
    }
    auto operand = [&] (const Operand &old) -> Operand {
      if (auto reg = detail::operand_access (old).get<const Register *> ())
        return Operand (*registers.at (*reg));
      if (auto label = detail::operand_access (old).get<const Label *> ())
        return Operand (*labels.at (*label));
      if (auto symbol = detail::operand_access (old).get<SymbolReference> ())
        return Operand (
          detail::NodeAccess::make_SymbolReference (symbols.at (
                                                      detail::NodeAccess::symbol (*(symbol))),
                                                    symbol->displacement));
      if (auto address = detail::operand_access (old).get<Memory> ()) {
        auto memory = *address;
        if (detail::NodeAccess::base (memory))
          detail::NodeAccess::base (memory) = registers.at (detail::NodeAccess::base (memory));
        if (detail::NodeAccess::index (memory))
          detail::NodeAccess::index (memory) = registers.at (detail::NodeAccess::index (memory));
        if (memory.literal_source_) memory.literal_source_ = data.at (memory.literal_source_);
        if (memory.location_) {
          // Clone only referenced analysis identities. Copying dead classes from
          // earlier optimization runs makes repeated optimize grow without bound.
          auto [at, fresh] = locations.try_emplace (memory.location_);
          if (fresh)
            at->second = &InstructionRewriter (*functions.at (&memory.location_->function ()))
                            .create_memory_location ();
          memory.location_ = at->second;
        }
        return Operand (std::move (memory));
      }
      return old;
    };
    auto copy_data = [&] (const Module &old_module, const GlobalData &old) -> Result<void> {
      auto initializer = std::visit (
        [&] (const auto &value) -> DataInitializer {
          using T = std::decay_t<decltype (value)>;
          if constexpr (std::is_same_v<T, SymbolReference>)
            return detail::NodeAccess::make_SymbolReference (symbols.at (
                                                               detail::NodeAccess::symbol (value)),
                                                             value.displacement);
          else if constexpr (std::is_same_v<T, LabelReference>)
            return detail::NodeAccess::make_LabelReference (labels.at (
                                                              detail::NodeAccess::label (value)),
                                                            detail::NodeAccess::base (value)
                                                              ? labels.at (
                                                                  detail::NodeAccess::base (value))
                                                              : nullptr,
                                                            value.displacement);
          else if constexpr (std::is_same_v<T, ExpressionData>)
            return detail::NodeAccess::make_ExpressionData (
              functions.at (detail::NodeAccess::function (value)));
          else
            return value;
        },
        old.initializer ());
      auto mapped
        = detail::access (*(modules.at (&old_module)))
            .create_data (old.symbol ().name (), std::move (initializer), old.writable ());
      if (!(mapped).has_value ()) return mapped.error ();
      mapped.value ().literal_pool_ = old.literal_pool_;
      data.emplace (&old, &(mapped).value ());
      if (old.symbol ().name ().empty ())
        symbols.emplace (&old.symbol (), const_cast<Symbol *> (&mapped.value ().symbol ()));
      return {};
    };
    // Literal provenance is remapped when cloning bodies, before expression data is verified.
    for (auto &old_module : sources)
      for (auto &old : old_module.data ())
        if (!std::holds_alternative<ExpressionData> (old.initializer ())) {
          auto copied = copy_data (old_module, old);
          if (!(copied).has_value ()) return copied.error ();
        }
    for (auto &old_module : sources) {
      auto &module = *modules.at (&old_module);
      for (auto &old_function : old_module.functions ())
        for (auto &old_block : old_function.blocks ()) {
          InstructionBuilder builder (*blocks.at (&old_block));
          for (auto &old : old_block.instructions ()) {
            std::vector<Operand> operands;
            operands.reserve (old.operands ().size ());
            for (auto &value : old.operands ()) operands.push_back (operand (value));
            auto copied = builder.emit (old.opcode (), operands);
            if (!(copied).has_value ()) return copied.error ();
          }
        }
      for (auto &old : old_module.data ())
        if (std::holds_alternative<ExpressionData> (old.initializer ())) {
          auto copied = copy_data (old_module, old);
          if (!(copied).has_value ()) return copied.error ();
        }
      for (auto &symbol : old_module.symbols ())
        if (symbol.exported ()) {
          auto exported = detail::access (module).export_symbol (*symbols.at (&symbol));
          if (!(exported).has_value ()) return exported.error ();
        }
      for (auto &symbol : old_module.symbols ())
        if (auto variable = symbol.variable ().get ()) {
          auto bound
            = detail::access (module).bind_variable (*symbols.at (&symbol), variable->type (),
                                                     variable->writable ());
          if (!(bound).has_value ()) return bound.error ();
        }
      module.items_.clear ();
      for (auto &item : detail::NodeAccess::items (old_module))
        module.items_.push_back (std::visit (
          [&] (auto value) -> ModuleItem {
            using T = decltype (value);
            if constexpr (std::is_same_v<T, const Function *>)
              return *functions.at (value);
            else if constexpr (std::is_same_v<T, const GlobalData *>)
              return *data.at (value);
            else if constexpr (std::is_same_v<T, const Symbol *>)
              return *symbols.at (value);
            else if constexpr (std::is_same_v<T, ForwardDeclaration>)
              return detail::NodeAccess::make_ForwardDeclaration (
                symbols.at (detail::NodeAccess::symbol (value)));
            else if constexpr (std::is_same_v<T, DataDeclaration>)
              return detail::NodeAccess::make_DataDeclaration (
                symbols.at (detail::NodeAccess::symbol (value)));
            else
              return detail::NodeAccess::make_ExportDeclaration (
                symbols.at (detail::NodeAccess::symbol (value)));
          },
          item));
      for (auto &symbol : old_module.symbols ())
        symbols.at (&symbol)->forward_declared_ = symbol.forward_declared_;
      module.next_literal_ = old_module.next_literal_;
    }
    auto valid = result.verify ();
    if (!(valid).has_value ()) return valid.error ();
    return std::move (result);
  }
};
Result<Program> clone (const Program &source) {
  auto valid = source.verify ();
  if (!(valid).has_value ()) return valid.error ();
  std::vector<const Module *> modules;
  for (auto &module : source.modules ()) modules.push_back (&module);
  return IRCloner::run (modules);
}
Result<Program> clone (const Module &source) {
  const Module *modules[]{&source};
  return IRCloner::run (modules);
}
}  // namespace mirnext::ir
