/* Licensed under the MIT License. Owned code bytes and typed relocations. */
#pragma once
#include "mirnext/CodeGen/Frame.hpp"
#include <functional>
namespace mirnext::codegen {
namespace detail {
struct ArtifactBuilder;
}
class CompiledLabel {
 public:
  std::size_t index () const { return index_; }
  friend bool operator== (CompiledLabel, CompiledLabel) = default;

 private:
  friend class CompiledFunction;
  friend struct detail::ArtifactBuilder;
  CompiledLabel (std::size_t n, std::shared_ptr<const char> owner)
    : index_ (n), owner_ (std::move (owner)) {}
  std::size_t index_;
  std::shared_ptr<const char> owner_;
};
struct CompiledLabelInfo {
  CompiledLabel label;
  std::string name;
  std::size_t offset;
  // Correspondence metadata, never an independently constructible label handle.
  // The ordinal belongs to the prepared source function's labels() order.
  std::optional<std::size_t> source_ordinal;
};
struct CodeRelocation {
  enum class Kind { AArch64WideMove, LocalAddress, AbsoluteSymbol };
  Kind kind;
  std::size_t offset;
  // AArch64 wide moves use a 16-bit part at this shift. LocalAddress writes
  // an eight-byte base-relative label address stored in the unrelocated bytes.
  // AbsoluteSymbol writes an eight-byte x86 immediate or constant-pool cell.
  unsigned shift = 0;
  std::optional<MachineSymbol> symbol;
  std::optional<Builtin> builtin;
  std::int64_t displacement = 0;
};
struct CodeSymbolRequest {
  const MachineSymbol *symbol = nullptr;
  std::optional<Builtin> builtin;
};
using CodeSymbolResolver = std::function<Result<std::uint64_t> (const CodeSymbolRequest &)>;
// Every original x86 pool reference owns a distinct aligned cell, even when
// values agree. Offsets and calls survive destruction of the machine IR.
struct ConstantPoolEntry {
  std::size_t offset, reference_offset, next_instruction;
  bool call = false;
};
struct LabelAddress {
  CompiledLabel label;
  std::optional<CompiledLabel> relative_to;
  std::int64_t displacement = 0;
};
class CompiledFunction {
 public:
  std::span<const std::uint8_t> bytes () const { return bytes_; }
  std::span<const CompiledLabelInfo> labels () const { return labels_; }
  std::span<const CodeRelocation> relocations () const { return relocations_; }
  std::span<const ConstantPoolEntry> constant_pool () const { return constants_; }
  const MachineFrameLayout &frame () const { return frame_; }
  std::string_view module_name () const { return module_; }
  std::string_view name () const { return name_; }
  target::Architecture architecture () const { return architecture_; }
  // Pure, repeatable relocation. Does not publish executable memory or mutate
  // this artifact; it may outlive every source/machine instruction and symbol.
  Result<std::vector<std::uint8_t>> relocate (std::uint64_t base,
                                              const CodeSymbolResolver & = {}) const;
  Result<std::vector<std::uint64_t>> label_addresses (std::uint64_t base,
                                                      std::span<const LabelAddress>) const;
  Result<CompiledLabel> find_label (std::string_view) const;
  Result<void> verify () const;

 private:
  friend struct detail::ArtifactBuilder;
  CompiledFunction (std::string module, std::string name, target::Architecture architecture,
                    MachineFrameLayout frame)
    : module_ (std::move (module)),
      name_ (std::move (name)),
      architecture_ (architecture),
      frame_ (std::move (frame)) {}
  std::string module_, name_;
  target::Architecture architecture_;
  MachineFrameLayout frame_;
  std::shared_ptr<const char> identity_ = std::make_shared<const char> (0);
  std::vector<std::uint8_t> bytes_;
  std::vector<CompiledLabelInfo> labels_;
  std::vector<CodeRelocation> relocations_;
  std::vector<ConstantPoolEntry> constants_;
};
// Optional known addresses retain the original shortest immediate selection.
// Without one, symbol moves reserve four chunks on AArch64 or eight immediate
// bytes on x86 and remain relocatable.
struct MachineEncodingOptions {
  CodeSymbolResolver known_addresses;
};
Result<CompiledFunction> encode_machine (const MachineFunction &, const MachineFrameResult &,
                                         MachineEncodingOptions = {});
}  // namespace mirnext::codegen
