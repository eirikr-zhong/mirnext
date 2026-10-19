/* Licensed under the MIT License. Owning native entry on a loaded C++ program. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <memory>
namespace mirnext::execution {
namespace detail { struct ProgramImage; }
// Retains the loaded program and executable storage after session destruction.
// Use only the declared native ABI signature. Raw calls use ExecutionInvocation
// to collect runtime diagnostics; checked session calls manage it automatically.
// Native MIR hardware bindings retain their documented physical side effects.
class NativeEntry {
public:
  std::uintptr_t address() const { return address_; }
  std::string_view module_name() const;
  std::string_view name() const;
  const ir::Signature &signature() const;
private:
  friend class LinkedProgram;
  friend class InterpretedProgram;
  friend class ExecutionSession;
  friend class FunctionHandle;
  NativeEntry(std::shared_ptr<detail::ProgramImage> owner,std::size_t index,std::uintptr_t address)
    : owner_(std::move(owner)),index_(index),address_(address) {}
  std::shared_ptr<detail::ProgramImage> owner_;
  std::size_t index_;
  std::uintptr_t address_;
};
}
