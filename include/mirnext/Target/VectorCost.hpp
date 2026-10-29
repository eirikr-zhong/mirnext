/* Licensed under the MIT License. Conservative SIMD128 instruction-sequence costs. */
#pragma once
#include "mirnext/Target/Addressing.hpp"
namespace mirnext::target {
// Relative instruction costs, not predicted cycles. Unsupported operations have
// no estimate and cannot make an SLP candidate profitable.
class VectorCostModel {
 public:
  static Result<VectorCostModel> get (Architecture = Architecture::Native);
  std::optional<unsigned> operation (ir::Opcode, ir::Type vector) const;
  unsigned constant () const { return 4; }
  unsigned splat (ir::Type) const;
  unsigned scalar (ir::Opcode) const;

 private:
  explicit VectorCostModel (Architecture architecture) : architecture_ (architecture) {}
  Architecture architecture_;
};
}  // namespace mirnext::target
