/* Licensed under the MIT License. Typed native ABI adapters on the owning IR. */
#pragma once
#include "mirnext/Execution/Linker.hpp"
#include "mirnext/Execution/detail/Hardware.hpp"
namespace mirnext::execution::detail {
class NativeRecovery;
// Signature-dependent adapters own their generated code, never the called code.
// The owner must keep callees and borrowed hardware names alive. Calls require
// external serialization.
class NativeCalls {
 public:
  explicit NativeCalls (LinkOptions options,
                        std::vector<std::pair<ir::Type, std::string_view>> hardware = {})
    : options_ (std::move (options)), hardware_ (std::move (hardware)) {
    options_.resolve = {};
  }
  Result<std::vector<ScalarValue>> call (std::uintptr_t, const ir::Signature &,
                                         std::span<const ExecutionArgument>,
                                         const NativeRecovery * = nullptr,
                                         std::span<HardwareSlot> = {});
  // The signature is immutable and belongs to the owning loaded program.
  // The checked scalar interface validates the homogeneous calling contract.
  Result<std::uintptr_t> scalar_entry (std::uintptr_t, const ir::Signature &);
  std::size_t size () const { return count_; }

 private:
  struct Entry {
    ir::Signature signature;
    std::vector<ir::Type> variadics;
    bool transfer;
    ExecutableFunction function;
  };
  LinkOptions options_;
  std::vector<std::pair<ir::Type, std::string_view>> hardware_;
  std::unordered_map<std::uintptr_t, std::vector<Entry>> entries_;
  struct ScalarEntry {
    const ir::Signature *signature;
    ExecutableFunction function;
  };
  std::unordered_map<std::uintptr_t, std::vector<ScalarEntry>> scalars_;
  std::size_t count_ = 0;
  Result<std::uintptr_t> entry (std::uintptr_t, const ir::Signature &,
                                std::span<const ExecutionArgument>, bool);
};
}  // namespace mirnext::execution::detail
