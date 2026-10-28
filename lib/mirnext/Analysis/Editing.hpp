/* Licensed under the MIT License. Cache access within a manager-owned edit batch. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"
namespace mirnext::analysis::detail {
struct EditingAccess {
  template <class Unit, class Manager>
  static unsigned &counter (AnalysisCache<Unit, Manager> &manager) {
    return manager.state_->active_edits;
  }
};
class EditScope {
 public:
  template <class Manager>
  explicit EditScope (Manager &manager) : counter_ (EditingAccess::counter (manager)) {
    ++counter_;
  }
  ~EditScope () { --counter_; }
  EditScope (const EditScope &) = delete;

 private:
  unsigned &counter_;
};
}  // namespace mirnext::analysis::detail
