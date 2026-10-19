/* Licensed under the MIT License. Immutable executable allocations and ownership. */
#pragma once
#include "mirnext/CodeGen/CompiledFunction.hpp"
namespace mirnext::execution {
struct MappedCodeMemory { std::uint8_t *data = nullptr; std::size_t size = 0; };
// OS service boundary. Implementations must support concurrent independent
// mappings. Protection/flush operations may receive page-aligned subranges of
// an allocation; release always receives the entire original mapping.
// No source IR, generator state or legacy callback table participates.
class CodeMemoryProvider {
public:
  virtual ~CodeMemoryProvider() = default;
  virtual std::size_t page_size() const = 0;
  virtual Result<MappedCodeMemory> allocate(std::size_t) = 0;
  virtual Result<void> release(MappedCodeMemory) = 0;
  virtual Result<void> make_writable(MappedCodeMemory) = 0;
  virtual Result<void> make_executable(MappedCodeMemory) = 0;
  virtual Result<void> flush(MappedCodeMemory) = 0;
};
std::shared_ptr<CodeMemoryProvider> native_code_memory_provider();
namespace detail { struct CodeRegion; }
class ExecutableCode {
public:
  // Moved-from handles have address zero and an empty byte view.
  std::uintptr_t address() const;
  std::span<const std::uint8_t> bytes() const;
private:
  friend class CodeReservation;
  ExecutableCode(std::shared_ptr<const detail::CodeRegion>,std::size_t,std::size_t);
  std::shared_ptr<const detail::CodeRegion> region_;
  std::size_t offset_,size_;
};
class CodeReservation {
public:
  CodeReservation(CodeReservation &&) noexcept;
  CodeReservation &operator=(CodeReservation &&) noexcept;
  ~CodeReservation();
  CodeReservation(const CodeReservation &) = delete;
  CodeReservation &operator=(const CodeReservation &) = delete;
  std::size_t size() const;
  Result<std::uintptr_t> address(std::size_t) const;
  // All buffers must match the reserved lengths. Publication is all-or-nothing;
  // failures return no executable handles. Successful publication consumes it.
  Result<std::vector<ExecutableCode>> publish(std::span<const std::span<const std::uint8_t>>);
private:
  friend class CodeMemory;
  friend class CodeArena;
  struct Slot { std::size_t offset,size; };
  CodeReservation(std::shared_ptr<detail::CodeRegion>,std::vector<Slot>,MappedCodeMemory);
  std::shared_ptr<detail::CodeRegion> region_;
  std::vector<Slot> slots_;
  MappedCodeMemory publication_;
};
// A bounded, monotonically reserved code address range. Each reservation owns
// complete pages, so later publication never requests an earlier batch's pages
// or rewrites its bytes. Slots
// inside a batch remain densely packed and 16-byte aligned. Failed publication
// can retry its reservation; abandoned reservations do not reclaim address space.
// Published code retains the mapping independently of the arena's lifetime.
// Reservation/publication/execution require external synchronization.
class CodeArena {
public:
  static Result<CodeArena> create(std::size_t byte_capacity,
      std::shared_ptr<CodeMemoryProvider> = native_code_memory_provider());
  CodeArena(CodeArena &&) noexcept = default;
  CodeArena &operator=(CodeArena &&) noexcept = default;
  CodeArena(const CodeArena &) = delete;
  CodeArena &operator=(const CodeArena &) = delete;
  std::size_t capacity() const;
  std::size_t remaining() const;
  std::uintptr_t address() const;
  Result<CodeReservation> reserve(std::span<const std::size_t>);
private:
  CodeArena(std::shared_ptr<detail::CodeRegion> region,std::size_t page)
    : region_(std::move(region)),page_(page) {}
  std::shared_ptr<detail::CodeRegion> region_;
  std::size_t page_,used_ = 0;
};
class ExecutableFunction {
public:
  const codegen::CompiledFunction &artifact() const { return artifact_; }
  const ExecutableCode &code() const { return code_; }
  std::uintptr_t address() const { return code_.address(); }
  Result<std::vector<std::uint64_t>> label_addresses(std::span<const codegen::LabelAddress> refs) const {
    return artifact_.label_addresses(address(),refs);
  }
private:
  friend class CodeMemory;
  ExecutableFunction(codegen::CompiledFunction artifact,ExecutableCode code) : artifact_(std::move(artifact)),code_(std::move(code)) {}
  codegen::CompiledFunction artifact_;
  ExecutableCode code_;
};
class CodeMemory {
public:
  explicit CodeMemory(std::shared_ptr<CodeMemoryProvider> provider = native_code_memory_provider()) : provider_(std::move(provider)) {}
  // One mapping per immutable batch; functions share pages without reopening
  // already published code. Offsets are 16-byte aligned, page rounding is exact.
  Result<CodeReservation> reserve(std::span<const std::size_t>) const;
  Result<ExecutableFunction> publish(codegen::CompiledFunction,const codegen::CodeSymbolResolver & = {}) const;
private:
  std::shared_ptr<CodeMemoryProvider> provider_;
};
}
