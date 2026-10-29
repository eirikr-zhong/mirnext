/* Licensed under the MIT License. Bounded basic-block SIMD128 vectorization. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Target/Addressing.hpp"
namespace mirnext::transforms {
struct VectorizationOptions {
  bool slp = false;
  bool combine = false;
  // Search limits bound JIT preparation work; reaching a limit retains scalar IR.
  std::size_t max_pack_nodes = 128;
  std::size_t max_block_packs = 64;
  std::size_t max_store_distance = 256;
};
class SLPVectorizePass final : public FunctionPass {
 public:
  explicit SLPVectorizePass (target::Architecture target = target::Architecture::Native,
                             VectorizationOptions options = {})
    : target_ (target), options_ (options) {}
  std::string_view name () const noexcept override { return "slp-vectorize"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;

 private:
  target::Architecture target_;
  VectorizationOptions options_;
};
class VectorCombinePass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "vector-combine"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
