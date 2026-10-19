/* Licensed under the MIT License. Owning BBV bytes and successor relocations. */
#pragma once
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/CodeGen/BlockVersionId.hpp"
namespace mirnext::codegen {
namespace detail { struct BlockArtifactBuilder; }
class BBVProgram;
class BBVVersion;
struct BlockRelocation {
  enum class Kind { AArch64Branch26, AArch64Branch19, AArch64Address21, X86Relative32, AbsoluteAddress };
  Kind kind;
  std::size_t offset;
  // x86 displacements are relative to the following instruction, not the field.
  std::size_t next_instruction;
  BlockVersionId target;
};
using BlockAddressResolver = std::function<Result<std::uint64_t>(const BlockVersionId &)>;
// No instruction, label, version or property pointer escapes into an artifact.
// Symbol fixups and pools use the same owning representation as whole functions.
class CompiledBlock {
public:
  BlockVersionId version() const { return version_; }
  std::span<const std::uint8_t> bytes() const { return code_.bytes(); }
  std::span<const CodeRelocation> relocations() const { return code_.relocations(); }
  std::span<const ConstantPoolEntry> constant_pool() const { return code_.constant_pool(); }
  std::span<const CompiledLabelInfo> labels() const { return code_.labels(); }
  const MachineFrameLayout &frame() const { return code_.frame(); }
  std::string_view module_name() const { return code_.module_name(); }
  std::string_view name() const { return code_.name(); }
  target::Architecture architecture() const { return code_.architecture(); }
  std::span<const BlockRelocation> successors() const { return successors_; }
  Result<void> verify() const;
  Result<std::vector<std::uint8_t>> relocate(std::uint64_t base,const BlockAddressResolver &,const CodeSymbolResolver & = {}) const;
private:
  friend struct detail::BlockArtifactBuilder;
  CompiledBlock(BlockVersionId id,CompiledFunction code,std::vector<BlockRelocation> successors)
    : version_(std::move(id)),code_(std::move(code)),successors_(std::move(successors)) {}
  BlockVersionId version_;
  CompiledFunction code_;
  std::vector<BlockRelocation> successors_;
};
Result<CompiledBlock> encode_bbv(BBVProgram &,const BBVVersion &,MachineEncodingOptions = {});
}
