/* Licensed under the MIT License. Shared loaded symbols/data; no engine state. */
#pragma once
#include "mirnext/Execution/Options.hpp"
namespace mirnext::execution::detail {
// Own the immutable IR before all borrowing symbols and callbacks, so it is
// destroyed last. Execution/preparation state never belongs to the source IR.
struct ProgramImage {
  enum class Kind { Prototype, Function, Data };
  struct Binding {
    const ir::Symbol *source;
    Kind kind = Kind::Prototype;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    bool writable = false, jump = false;
    std::optional<ir::Signature> signature;
    std::optional<std::size_t> alias, function;
    InterpreterHostFunction interpret;
  };
  struct Section {
    std::unique_ptr<std::byte[]> data;
    std::size_t size;
  };
  std::shared_ptr<const ir::Program> source;
  const ir::Program &context;
  target::TargetMachine target;
  std::vector<Binding> bindings;
  std::unordered_map<const ir::Symbol *, std::size_t> identities;
  ir::StringMap<std::size_t> exports;
  std::vector<Section> sections;
  LinkStatistics statistics;
  ProgramImage (std::shared_ptr<const ir::Program> c, target::TargetMachine t)
    : source (std::move (c)), context (*source), target (std::move (t)) {}
  std::size_t resolved (std::size_t n) const { return bindings[n].alias.value_or (n); }
  const Binding &binding (std::size_t n) const { return bindings[resolved (n)]; }
  Binding &binding (std::size_t n) { return bindings[resolved (n)]; }
  Error error (const ir::Symbol *, std::string_view phase, Error) const;
  Result<std::size_t> find (std::string_view module, std::string_view name) const;
  Result<void> check_import (std::size_t) const;
  Result<void> symbols (const LinkOptions &, bool native = true);
  Result<void> data ();
  Result<void> references (const std::function<Result<std::uint64_t> (const ir::Label &)> &);
};
}  // namespace mirnext::execution::detail
