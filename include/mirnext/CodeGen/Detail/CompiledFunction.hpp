/* Licensed under the MIT License. Checked publication of owning code artifacts. */
#pragma once
#include "mirnext/CodeGen/CompiledFunction.hpp"
namespace mirnext::codegen::detail {
// Mutable encoding state is private to code generation. Publishing transfers
// ownership, creates label identities and validates every fixup in one place.
struct EncodedLabel {
  std::string name;
  std::size_t offset;
  std::optional<std::size_t> source_ordinal;
};
struct CompiledFunctionData {
  std::string module_,name_;
  target::Architecture architecture_;
  MachineFrameLayout frame_;
  std::vector<std::uint8_t> bytes_;
  std::vector<EncodedLabel> labels_;
  std::vector<CodeRelocation> relocations_;
  std::vector<ConstantPoolEntry> constants_;
  CompiledFunctionData(std::string module,std::string name,target::Architecture architecture,MachineFrameLayout frame)
    : module_(std::move(module)),name_(std::move(name)),architecture_(architecture),frame_(std::move(frame)) {}
};
struct ArtifactBuilder {
  static Result<CompiledFunction> finish(CompiledFunctionData);
};
}
