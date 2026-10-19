/* Licensed under the MIT License. Diagnostics across the native ABI boundary. */
#pragma once
#include "mirnext/Support/Result.hpp"
namespace mirnext::execution {
// A native ABI cannot return Result alongside arbitrary MIR results. This scope
// collects the first runtime error, including interpreted callbacks. Nested
// scopes restore their parent. Checked session calls create one automatically.
class ExecutionInvocation {
 public:
  ExecutionInvocation ();
  ~ExecutionInvocation ();
  ExecutionInvocation (const ExecutionInvocation &) = delete;
  ExecutionInvocation &operator= (const ExecutionInvocation &) = delete;
  Result<void> status () const;

 private:
  std::optional<Error> error_;
  std::optional<Error> *previous_;
};
namespace detail {
void report_execution_error (Error);
}
}  // namespace mirnext::execution
