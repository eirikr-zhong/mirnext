/* Licensed under the MIT License. Private block artifact construction. */
#pragma once
#include "mirnext/CodeGen/CompiledBlock.hpp"
#include "mirnext/CodeGen/BlockVersioning.hpp"
namespace mirnext::codegen::detail {
struct BlockArtifactBuilder {
  static Result<CompiledBlock> finish (BlockVersionId id, CompiledFunction code,
                                       std::vector<BlockRelocation> successors) {
    CompiledBlock result (std::move (id), std::move (code), std::move (successors));
    auto valid = result.verify ();
    if (!valid) return valid.error ();
    return result;
  }
};
Result<CompiledBlock> encode_aarch64_block (const BBVPreparation &, const BBVVersion &,
                                            MachineEncodingOptions);
Result<CompiledBlock> encode_x86_block (const BBVPreparation &, const BBVVersion &,
                                        MachineEncodingOptions);
// All native fixups emitted for one operand (e.g. floating unordered branches)
// retain the same typed successor, without relying on fixup/edge count equality.
class BlockTargets {
  struct Key {
    const MachineInstruction *instruction;
    std::size_t operand;
    friend bool operator== (Key, Key) = default;
  };
  struct Hash {
    std::size_t operator() (Key key) const {
      return std::hash<const MachineInstruction *>{}(key.instruction) ^ (key.operand + 0x9e3779b9);
    }
  };
  std::unordered_map<Key, BlockVersionId, Hash> targets_;

 public:
  explicit BlockTargets (const BBVSpecialization &plan) {
    targets_.reserve (plan.successors ().size ());
    for (auto &edge : plan.successors ())
      targets_.emplace (Key{edge.instruction, edge.operand}, edge.version->id ());
  }
  Result<BlockVersionId> find (const MachineInstruction *i, std::size_t operand) const {
    auto found = targets_.find ({i, operand});
    if (found != targets_.end ()) return found->second;
    return Error{ErrorCode::InvalidOperand, "bbv-encode: missing typed successor"};
  }
};
}  // namespace mirnext::codegen::detail
