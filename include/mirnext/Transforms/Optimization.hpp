/* Licensed under the MIT License. Owning optimization results on the canonical IR. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/IR/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Target/Addressing.hpp"
namespace mirnext::transforms {
enum class OptimizationLevel { O0, O1, O2, O3 };
enum class PassId {
  Materialize,
  Simplify,
  Inline,
  BuildCFG,
  CloneBlocks,
  BuildSSA,
  TransformAddresses,
  GVN,
  CopyPropagation,
  DeadStores,
  SSADeadCode,
  BuildLoops,
  LICM,
  PressureRelief,
  ConventionalSSA,
  SSACombine,
  DestroySSA,
  JumpOptimization
};
std::string_view pass_name (PassId) noexcept;
struct PassStatistics {
  PassId pass;
  std::string module, function;
  // Public instructions, excluding labels and private SSA PHIs.
  std::size_t instructions_before, instructions_after, transformations;
};
struct OptimizationSnapshot {
  // Borrowed views remain valid only during the callback.
  const PassStatistics &statistics;
  PassEvent::Phase phase;
  const ir::Function &function;
  const analysis::FunctionGraph *graph = nullptr;
  const analysis::LoopTree *loops = nullptr;
  const ir::SSAForm *ssa () const { return function.ssa_form (); }
};
struct OptimizationOptions {
  OptimizationLevel level = OptimizationLevel::O2;
  target::Architecture target = target::Architecture::Native;
  std::function<Result<void> (const OptimizationSnapshot &)> observer;
};
class OptimizedProgram {
 public:
  OptimizedProgram (OptimizedProgram &&) noexcept;
  OptimizedProgram &operator= (OptimizedProgram &&) noexcept;
  ~OptimizedProgram ();
  OptimizedProgram (const OptimizedProgram &) = delete;
  OptimizedProgram &operator= (const OptimizedProgram &) = delete;
  // These accessors require a non-moved-from program.
  const ir::Context &context () const noexcept;
  OptimizationLevel level () const noexcept;
  target::Architecture target () const noexcept;
  const ir::Function *find_function (std::string_view module, std::string_view name) const noexcept;
  std::span<const PassStatistics> statistics () const noexcept;
  Result<void> verify () const;
  std::string dump () const;
  Result<std::string> write_text () const;
  Result<std::vector<std::byte>> encode_binary (serialization::BinaryEncoding
                                                = serialization::BinaryEncoding::Compressed) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit OptimizedProgram (std::unique_ptr<Impl>);
  friend Result<OptimizedProgram> optimize (const ir::Context &, OptimizationOptions);
  friend Result<OptimizedProgram> optimize (const ir::Module &, OptimizationOptions);
};
Result<OptimizedProgram> optimize (const ir::Context &, OptimizationOptions = {});
Result<OptimizedProgram> optimize (const ir::Module &, OptimizationOptions = {});
}  // namespace mirnext::transforms
