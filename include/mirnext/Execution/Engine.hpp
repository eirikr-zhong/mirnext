/* Licensed under the MIT License. Per-definition execution policy. */
#pragma once
#include "mirnext/Support/Result.hpp"
#include <functional>
namespace mirnext::ir { class Function; }
namespace mirnext::execution {
enum class ExecutionMode { JIT, Interpreter, Mixed };
enum class ExecutionEngine { Interpreter, EagerJIT, LazyJIT, BlockVersioning };
// Invoked once per owned, optimized definition, before code publication.
// The reference is borrowed for the callback; aliases share their definition's
// choice. An error aborts loading rather than selecting a fallback engine.
using EngineSelector = std::function<Result<ExecutionEngine>(const ir::Function &)>;
}
