/* Licensed under the MIT License. Value numbering over function-owned SSA. */
#pragma once
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/IR/SSA.hpp"
namespace mirnext::transforms {
namespace detail {
class AnalysisAccess;
}
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
    OptionalRef<const ir::SSAValue> base;
    std::uint64_t displacement = 0;
  };
  OptionalRef<const Number> number (const ir::SSAValue &value) const {
    auto at = numbers.find (&value);
    return at == numbers.end () ? OptionalRef<const Number>{}
                                : OptionalRef<const Number> (at->second);
  }
  std::optional<std::size_t> location (const ir::Instruction &value) const {
    auto at = locations.find (&value);
    return at == locations.end () ? std::nullopt : std::optional (at->second);
  }

 private:
  friend class detail::AnalysisAccess;
  std::unordered_map<const ir::SSAValue *, Number> numbers;

  std::unordered_map<const ir::Instruction *, std::size_t> locations;

 public:
  std::vector<MemoryLocation> memory;
  bool full_escape = false;
};
struct GVNAnalysis {
  static const unsigned char *id () noexcept;
  using Result = GVNResult;
  static constexpr analysis::Dependency dependencies = analysis::Dependency::All;
  static mirnext::Result<Result> run (const ir::Function &);
};
class GVNPass final : public FunctionPass {
 public:
  std::string_view name () const noexcept override { return "gvn"; }
  Result<PassResult> run (ir::MIRBuilder &, analysis::FunctionAnalysisManager &) override;
};
}  // namespace mirnext::transforms
