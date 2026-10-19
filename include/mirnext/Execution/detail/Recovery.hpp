/* Licensed under the MIT License. Native invocation recovery ownership. */
#pragma once
#include "mirnext/CodeGen/InvocationEntry.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include <type_traits>
namespace mirnext::execution::detail {
class RecoveryActivation {
public:
  explicit RecoveryActivation(codegen::NativeInvocationState &) noexcept;
  ~RecoveryActivation();
  RecoveryActivation(const RecoveryActivation &) = delete;
  RecoveryActivation &operator=(const RecoveryActivation &) = delete;
private:
  codegen::NativeInvocationState *previous_;
};
// Immutable generated entries may be shared across threads. Each invocation
// uses its own stack checkpoint and nested thread-local activation, with no
// allocation on the success path. The called adapter and MIR runtime remain
// owned by the caller. Every host-to-MIR reentry must use a checked boundary.
class NativeRecovery {
public:
  static Result<NativeRecovery> create(std::shared_ptr<CodeMemoryProvider> = native_code_memory_provider());
  NativeRecovery(NativeRecovery &&) noexcept = default;
  NativeRecovery &operator=(NativeRecovery &&) noexcept = default;
  NativeRecovery(const NativeRecovery &) = delete;
  NativeRecovery &operator=(const NativeRecovery &) = delete;
  std::uintptr_t recovery_address() const { return recover_.address(); }
  std::size_t code_bytes() const { return invoke_.bytes().size()+recover_.bytes().size(); }
  template<class T> Result<T> invoke(std::uintptr_t adapter,std::uintptr_t arg0 = 0,std::uintptr_t arg1 = 0) const {
    static_assert(std::is_same_v<T,void> || std::is_same_v<T,std::int64_t> || std::is_same_v<T,std::uint64_t> || std::is_same_v<T,double>);
    if (!adapter || !invoke_.address()) return Error{ErrorCode::InvalidArgument,"null native invocation adapter or recovery service"};
    codegen::NativeInvocationState state;
    RecoveryActivation activation(state);
    ExecutionInvocation invocation;
    auto entry = reinterpret_cast<T (*)(std::uintptr_t,std::uintptr_t,std::uintptr_t,codegen::NativeInvocationState *)>(invoke_.address());
    if constexpr (std::is_void_v<T>) {
      entry(adapter,arg0,arg1,&state);
      return invocation.status();
    } else {
      auto result = entry(adapter,arg0,arg1,&state);
      auto status = invocation.status(); if (!status) return status.error(); return result;
    }
  }
private:
  NativeRecovery(ExecutableCode invoke,ExecutableCode recover) : invoke_(std::move(invoke)),recover_(std::move(recover)) {}
  ExecutableCode invoke_,recover_;
};
}
