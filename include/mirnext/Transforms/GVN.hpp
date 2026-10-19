/* Licensed under the MIT License. Value numbering over function-owned SSA. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/IR/SSA.hpp"
namespace mirnext::transforms {
struct GVNResult {
  struct Number {
    bool constant = false;
    std::uint64_t value = 0;
    unsigned alloca_flags = 0;
  };
  struct MemoryLocation {
    ir::Type type;
    std::string alias, nonalias;
    unsigned alloca_flags;
    const ir::SSAValue *base = nullptr;
    std::uint64_t displacement = 0;
  };
  std::unordered_map<const ir::SSAValue *, Number> numbers;
  std::unordered_map<const ir::Instruction *, std::size_t> locations;
  std::vector<MemoryLocation> memory;
  bool full_escape = false;
};
struct GVNAnalysis {
  using Result = GVNResult;
  static constexpr analysis::Dependency dependencies = analysis::Dependency::All;
  static mirnext::Result<Result> run (const ir::Function &);
};
class GVNPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "gvn"; }
  Result<PassResult> run (ir::Function &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
