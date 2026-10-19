/* Licensed under the MIT License. Two-pass typed remapping; no runtime objects. */
#include "mirnext/IR/IR.hpp"
#include <unordered_map>

namespace mirnext::ir {
class IRCloner {
 public:
  static Result<std::unique_ptr<Context>> run (std::span<const Module *const> input) {
    auto sources = input | std::views::transform ([] (const Module *module) -> const Module & {
                     return *module;
                   });
    for (auto &module : sources) {
      for (auto &function : module.functions ()) {
        auto phase = function.require_register_ir ();
        if (!phase) return phase.error ();
      }
      auto valid = module.verify ();
      if (!valid) return valid.error ();
    }
    auto result = std::make_unique<Context> ();
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
      auto created = result->create_module (old_module.name ());
      if (!created) return created.error ();
      auto &module = **created;
      modules.emplace (&old_module, &module);
      for (auto &symbol : old_module.symbols ()) {
        if (symbol.kind () == Symbol::Kind::Data && symbol.name ().empty ()) continue;
        Result<Symbol *> copied
          = symbol.kind () == Symbol::Kind::Prototype
              ? module.prototype (symbol.name (), *symbol.signature ())
            : symbol.kind () == Symbol::Kind::Import
              ? module.import (symbol.name (), symbol.signature ()
                                                 ? std::optional (*symbol.signature ())
                                                 : std::nullopt)
            : symbol.kind () == Symbol::Kind::Function
              ? module.declare_function (symbol.name (), *symbol.signature ())
              : module.forward (symbol.name ());
        if (!copied) return copied.error ();
        symbols.emplace (&symbol, *copied);
      }
      for (auto &old : old_module.functions ()) {
        std::vector<std::string_view> names;
        bool all_named = true;
        for (auto reg : old.arguments ()) {
          names.push_back (reg->name ());
          all_named &= !reg->name ().empty ();
        }
        auto copied = module.define_function (*symbols.at (&old.symbol ()),
                                              all_named ? std::span<const std::string_view> (names)
                                                        : std::span<const std::string_view>{});
        if (!copied) return copied.error ();
        auto &function = **copied;
        functions.emplace (&old, &function);
        for (std::size_t n = 0; n < old.arguments ().size (); ++n)
          registers.emplace (old.arguments ()[n], function.arguments ()[n]);
        for (auto &reg : old.registers ())
          if (!reg.is_argument ()) {
            auto mapped
              = function.create_register (reg.type (), reg.name (), reg.hardware_binding ());
            if (!mapped) return mapped.error ();
            registers.emplace (&reg, *mapped);
          }
        for (auto &reg : old.registers ())
          const_cast<Register *> (registers.at (&reg))->memory_backed_ = reg.is_memory_backed ();
        // Preserve aliases of an interned hardware register without declaring a second binding.
        for (auto &[name, reg] : old.register_names_)
          function.register_names_.insert_or_assign (module.context ().intern_string (name),
                                                     const_cast<Register *> (registers.at (reg)));
        for (auto &scope : old.stack_scopes ()) {
          auto copied_scope
            = function.create_stack_scope (scope.parent () ? scopes.at (scope.parent ()) : nullptr,
                                           scope.name ());
          if (!copied_scope) return copied_scope.error ();
          scopes.emplace (&scope, *copied_scope);
        }
        for (auto &block : old.blocks ()) {
          auto mapped = function.create_block (block.name (), block.stack_scope ()
                                                                ? scopes.at (block.stack_scope ())
                                                                : nullptr);
          if (!mapped) return mapped.error ();
          blocks.emplace (&block, *mapped);
        }
        for (auto &scope : old.stack_scopes ())
          scopes.at (&scope)->materialized_ = scope.materialized ();
        for (auto &label : old.labels ()) {
          auto mapped = function.create_label (*blocks.at (&label.block ()), label.name ());
          if (!mapped) return mapped.error ();
          labels.emplace (&label, *mapped);
        }
      }
    }
    auto operand = [&] (const Operand &old) -> Operand {
      if (auto reg = old.get<const Register *> ()) return Operand (*registers.at (*reg));
      if (auto label = old.get<const Label *> ()) return Operand (*labels.at (*label));
      if (auto symbol = old.get<SymbolReference> ())
        return Operand (SymbolReference{symbols.at (symbol->symbol), symbol->displacement});
      if (auto address = old.get<Memory> ()) {
        auto memory = *address;
        if (memory.base) memory.base = registers.at (memory.base);
        if (memory.index) memory.index = registers.at (memory.index);
        if (memory.literal_source_) memory.literal_source_ = data.at (memory.literal_source_);
        if (memory.location_) {
          // Clone only referenced analysis identities. Copying dead classes from
          // earlier optimization runs makes repeated optimize grow without bound.
          auto [at, fresh] = locations.try_emplace (memory.location_);
          if (fresh)
            at->second = &IRRewriter (*functions.at (&memory.location_->function ()))
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
            return SymbolReference{symbols.at (value.symbol), value.displacement};
          else if constexpr (std::is_same_v<T, LabelReference>)
            return LabelReference{labels.at (value.label),
                                  value.base ? labels.at (value.base) : nullptr,
                                  value.displacement};
          else if constexpr (std::is_same_v<T, ExpressionData>)
            return ExpressionData{functions.at (value.function)};
          else
            return value;
        },
        old.initializer ());
      auto mapped
        = modules.at (&old_module)
            ->create_data (old.symbol ().name (), std::move (initializer), old.writable ());
      if (!mapped) return mapped.error ();
      (*mapped)->literal_pool_ = old.literal_pool_;
      data.emplace (&old, *mapped);
      if (old.symbol ().name ().empty ())
        symbols.emplace (&old.symbol (), const_cast<Symbol *> (&(*mapped)->symbol ()));
      return {};
    };
    // Literal provenance is remapped when cloning bodies, before expression data is verified.
    for (auto &old_module : sources)
      for (auto &old : old_module.data ())
        if (!std::holds_alternative<ExpressionData> (old.initializer ())) {
          auto copied = copy_data (old_module, old);
          if (!copied) return copied.error ();
        }
    for (auto &old_module : sources) {
      auto &module = *modules.at (&old_module);
      for (auto &old_function : old_module.functions ())
        for (auto &old_block : old_function.blocks ()) {
          IRBuilder builder (*blocks.at (&old_block));
          for (auto &old : old_block.instructions ()) {
            std::vector<Operand> operands;
            operands.reserve (old.operands ().size ());
            for (auto &value : old.operands ()) operands.push_back (operand (value));
            auto copied = builder.emit (old.opcode (), operands);
            if (!copied) return copied.error ();
          }
        }
      for (auto &old : old_module.data ())
        if (std::holds_alternative<ExpressionData> (old.initializer ())) {
          auto copied = copy_data (old_module, old);
          if (!copied) return copied.error ();
        }
      for (auto &symbol : old_module.symbols ())
        if (symbol.exported ()) {
          auto exported = module.export_symbol (*symbols.at (&symbol));
          if (!exported) return exported.error ();
        }
      for (auto &symbol : old_module.symbols ())
        if (auto variable = symbol.variable ()) {
          auto bound = module.bind_variable (*symbols.at (&symbol), variable->type (),
                                             variable->writable ());
          if (!bound) return bound.error ();
        }
      module.items_.clear ();
      for (auto &item : old_module.items ())
        module.items_.push_back (std::visit (
          [&] (auto value) -> ModuleItem {
            using T = decltype (value);
            if constexpr (std::is_same_v<T, const Function *>)
              return static_cast<const Function *> (functions.at (value));
            else if constexpr (std::is_same_v<T, const GlobalData *>)
              return data.at (value);
            else if constexpr (std::is_same_v<T, const Symbol *>)
              return static_cast<const Symbol *> (symbols.at (value));
            else if constexpr (std::is_same_v<T, ForwardDeclaration>)
              return ForwardDeclaration{symbols.at (value.symbol)};
            else if constexpr (std::is_same_v<T, DataDeclaration>)
              return DataDeclaration{symbols.at (value.symbol)};
            else
              return ExportDeclaration{symbols.at (value.symbol)};
          },
          item));
      for (auto &symbol : old_module.symbols ())
        symbols.at (&symbol)->forward_declared_ = symbol.forward_declared_;
      module.next_literal_ = old_module.next_literal_;
    }
    auto valid = result->verify ();
    if (!valid) return valid.error ();
    return result;
  }
};
Result<std::unique_ptr<Context>> clone (const Context &source) {
  auto valid = source.verify ();
  if (!valid) return valid.error ();
  std::vector<const Module *> modules;
  for (auto &module : source.modules ()) modules.push_back (&module);
  return IRCloner::run (modules);
}
Result<std::unique_ptr<Context>> clone (const Module &source) {
  const Module *modules[]{&source};
  return IRCloner::run (modules);
}
}  // namespace mirnext::ir
