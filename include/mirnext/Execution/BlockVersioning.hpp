/* Licensed under the MIT License. Observing owning block compilation. */
#pragma once
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/CodeGen/CompiledBlock.hpp"
namespace mirnext::execution {
struct BlockCompilationSnapshot {
  // Views live only during the observer. Rejecting a compilation leaves its
  // stable entry cold and retryable; no executable destination is published.
  const codegen::BBVVersion &version;
  const codegen::CompiledBlock &compiled;
};
struct BlockCompilationOptions {
  // Per-function executable arena budget, rounded to native pages. Exhaustion
  // returns a diagnostic; there is no unbounded mapping or engine fallback.
  std::size_t code_capacity = 1024*1024;
  std::function<Result<void>(const BlockCompilationSnapshot &)> observer;
};
struct BlockCompilationStatistics {
  std::size_t blocks = 0,compiled_blocks = 0,compilation_attempts = 0,code_batches = 0,code_bytes = 0;
};
}
