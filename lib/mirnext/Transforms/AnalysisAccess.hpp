/* Licensed under the MIT License. Private value-numbering cache mutation. */
#pragma once
#include "mirnext/Transforms/GVN.hpp"
namespace mirnext::transforms::detail {
class AnalysisAccess {
 public:
  static auto &numbers (GVNResult &value) { return value.numbers; }
  static const auto &numbers (const GVNResult &value) { return value.numbers; }
  static auto &locations (GVNResult &value) { return value.locations; }
  static const auto &locations (const GVNResult &value) { return value.locations; }
};
}  // namespace mirnext::transforms::detail
