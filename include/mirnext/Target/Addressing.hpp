/* Licensed under the MIT License. Target-specific constraints for middle-end address combining. */
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::target {
enum class Architecture { Native, X86_64, AArch64 };
class AddressingRules {
public:
  static Result<AddressingRules> get(Architecture = Architecture::Native);
  Architecture architecture() const { return architecture_; }
  bool accepts(ir::Type, std::int64_t displacement, bool indexed, unsigned scale) const;
private:
  explicit AddressingRules(Architecture architecture) : architecture_(architecture) {}
  Architecture architecture_;
};
}
