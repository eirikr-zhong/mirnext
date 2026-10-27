/* Licensed under the MIT License. Typed, revision-aware analysis caching without RTTI. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <unordered_set>

namespace mirnext::analysis {
enum class Dependency : unsigned {
  Instructions = 1,
  ControlFlow = 2,
  Registers = 4,
  ModuleData = 8,
  Symbols = 16,
  All = 31
};
constexpr Dependency operator| (Dependency a, Dependency b) {
  return static_cast<Dependency> (static_cast<unsigned> (a) | static_cast<unsigned> (b));
}
namespace detail {
template <class Analysis>
inline unsigned char analysis_key = 0;
template <class Analysis>
const unsigned char *key () noexcept {
  // Analyses shared across DLLs provide one out-of-line id() in their owning
  // library. Inline template variables have a separate address in each DLL.
  // Local extension analyses may use their own token without registration.
  if constexpr (requires { Analysis::id (); })
    return Analysis::id ();
  else
    return &analysis_key<Analysis>;
}
inline bool current (ir::Revisions before, ir::Revisions after, Dependency dependencies) {
  auto mask = static_cast<unsigned> (dependencies);
  return (!(mask & 1) || before.instructions == after.instructions)
         && (!(mask & 2) || before.control_flow == after.control_flow)
         && (!(mask & 4) || before.registers == after.registers)
         && (!(mask & 8) || before.module_data == after.module_data)
         && (!(mask & 16) || before.symbols == after.symbols);
}
}  // namespace detail
class PreservedAnalyses {
 public:
  static PreservedAnalyses all () {
    PreservedAnalyses result;
    result.all_ = true;
    return result;
  }
  static PreservedAnalyses none () { return {}; }
  template <class Analysis>
  void preserve () {
    preserved_.insert (detail::key<Analysis> ());
  }
  bool preserves (const unsigned char *key) const { return all_ || preserved_.contains (key); }

 private:
  bool all_ = false;
  std::unordered_set<const unsigned char *> preserved_;
};

namespace detail {
// One cache implementation for both ownership levels. Revision checks are O(1)
// and do not walk the module's functions on every analysis lookup.
template <class Unit, class Manager>
class AnalysisCache {
 public:
  explicit AnalysisCache (Unit &unit) : unit_ (unit) {}
  template <class Analysis>
  const typename Analysis::Result *get_cached () const {
    auto found = entries_.find (detail::key<Analysis> ());
    if (found == entries_.end ()
        || !detail::current (found->second->revisions, revisions (), found->second->dependencies))
      return nullptr;
    return &static_cast<const Entry<Analysis> &> (*found->second).value;
  }
  template <class Analysis>
  Result<const typename Analysis::Result *> get () {
    const auto key = detail::key<Analysis> ();
    auto revisions = this->revisions ();
    if (auto found = entries_.find (key); found != entries_.end ()) {
      if (detail::current (found->second->revisions, revisions, found->second->dependencies))
        return &static_cast<Entry<Analysis> &> (*found->second).value;
      entries_.erase (found);
    }
    if (!running_.insert (key).second)
      return Error{ErrorCode::InvalidArgument, diagnostic_name () + ": analysis dependency cycle"};
    struct Guard {
      std::unordered_set<const unsigned char *> &running;
      const unsigned char *key;
      ~Guard () { running.erase (key); }
    } guard{running_, key};
    auto computed = [&] {
      if constexpr (requires { Analysis::run (unit_, static_cast<Manager &> (*this)); })
        return Analysis::run (unit_, static_cast<Manager &> (*this));
      else
        return Analysis::run (unit_);
    }();
    if (!computed) return computed.error ();
    auto entry = std::make_unique<Entry<Analysis>> (std::move (*computed), revisions);
    auto result = &entry->value;
    entries_.emplace (key, std::move (entry));
    return result;
  }
  template <class Analysis>
  Result<void> set (typename Analysis::Result value) {
    const auto key = detail::key<Analysis> ();
    if (running_.contains (key))
      return Error{ErrorCode::InvalidArgument, "cannot replace a running analysis"};
    entries_.insert_or_assign (key,
                               std::make_unique<Entry<Analysis>> (std::move (value), revisions ()));
    return {};
  }
  template <class Analysis>
  Result<std::optional<typename Analysis::Result>> take_cached () {
    const auto key = detail::key<Analysis> ();
    if (running_.contains (key))
      return Error{ErrorCode::InvalidArgument, "cannot take a running analysis"};
    auto found = entries_.find (key);
    if (found == entries_.end ()) return std::optional<typename Analysis::Result>{};
    if (!detail::current (found->second->revisions, revisions (), found->second->dependencies)) {
      entries_.erase (found);
      return std::optional<typename Analysis::Result>{};
    }
    std::optional<typename Analysis::Result> result (
      std::move (static_cast<Entry<Analysis> &> (*found->second).value));
    entries_.erase (found);
    return result;
  }
  void invalidate (const PreservedAnalyses &preserved) {
    auto revisions = this->revisions ();
    std::erase_if (entries_, [&] (const auto &entry) {
      return !preserved.preserves (entry.first)
             || !detail::current (entry.second->revisions, revisions, entry.second->dependencies);
    });
  }
  void clear () { entries_.clear (); }
  std::size_t cached () const { return entries_.size (); }

 protected:
  Unit &unit_;

 private:
  ir::Revisions revisions () const {
    auto result = unit_.revisions ();
    // Symbol lookup is an analysis dependency, not a mutation of a function's
    // body. Keep Function::revisions() stable when another function is declared.
    if constexpr (std::is_same_v<Unit, ir::Function>)
      result.symbols = unit_.module ().revisions ().symbols;
    return result;
  }
  std::string diagnostic_name () const {
    if constexpr (std::is_same_v<Unit, ir::Function>)
      return std::string (unit_.symbol ().name ());
    else
      return std::string (unit_.name ());
  }
  struct EntryBase {
    ir::Revisions revisions;
    Dependency dependencies;
    EntryBase (ir::Revisions revisions, Dependency dependencies)
      : revisions (revisions), dependencies (dependencies) {}
    virtual ~EntryBase () = default;
  };
  template <class Analysis>
  struct Entry final : EntryBase {
    typename Analysis::Result value;
    Entry (typename Analysis::Result value, ir::Revisions revisions)
      : EntryBase (revisions, Analysis::dependencies), value (std::move (value)) {}
  };
  std::unordered_map<const unsigned char *, std::unique_ptr<EntryBase>> entries_;
  std::unordered_set<const unsigned char *> running_;
};
}  // namespace detail

// Managers and their results must not outlive their IR owner. A result remains
// valid until its cache entry is invalidated or that analysis is recomputed.
class FunctionAnalysisManager
  : public detail::AnalysisCache<ir::Function, FunctionAnalysisManager> {
 public:
  using AnalysisCache::AnalysisCache;
  const ir::Function &function () const { return unit_; }
};

class ModuleAnalysisManager : public detail::AnalysisCache<ir::Module, ModuleAnalysisManager> {
 public:
  using AnalysisCache::AnalysisCache;
  const ir::Module &module () const { return unit_; }
  Result<FunctionAnalysisManager *> function_analyses (const ir::Function &function) {
    if (&function.module () != &unit_)
      return Error{ErrorCode::InvalidArgument, "module analysis: function owner mismatch"};
    auto &manager = functions_[&function];
    // The module itself is mutable; const analysis views may populate caches.
    if (!manager)
      manager = std::make_unique<FunctionAnalysisManager> (const_cast<ir::Function &> (function));
    return manager.get ();
  }
  void invalidate (const PreservedAnalyses &preserved) {
    AnalysisCache::invalidate (preserved);
    for (auto &[function, manager] : functions_) manager->invalidate (preserved);
  }
  void clear () {
    AnalysisCache::clear ();
    for (auto &[function, manager] : functions_) manager->clear ();
  }

 private:
  std::unordered_map<const ir::Function *, std::unique_ptr<FunctionAnalysisManager>> functions_;
};
}  // namespace mirnext::analysis
