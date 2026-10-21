/* Licensed under the MIT License. Native runtime coverage and ABI limitations. */
#pragma once
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
namespace mirnext::test {
inline bool native_execution_available() { return bool(target::TargetMachine::native()); }
inline std::string native_hardware_text(std::string_view text) {
  std::string result(text);
  if (target::TargetMachine::native()->architecture() == target::Architecture::X86_64)
    for (auto p = result.find(":r19"); p != std::string::npos; p = result.find(":r19",p+4)) result.replace(p,4,":r12");
  return result;
}
// The original Windows ABI supports only one result. Keep constructing and
// verifying these fixtures and require an explicit native diagnostic; the
// interpreter still executes them. This is not an architecture-wide skip.
inline bool check_native_result_limit(const ir::Context &context,execution::ExecutionOptions options = {}) {
  if (options.mode != execution::ExecutionMode::JIT && !options.native_entries) return false;
  if (target::TargetMachine::native()->abi() != target::ABI::X86Windows) return false;
  bool multiple = false;
  for (auto &module : context.modules()) for (auto &function : module.functions()) if (function.signature().results.size() > 1) multiple = true;
  if (!multiple) return false;
  REQUIRE(context.verify()); const auto compilation = options.link.compilation;
  auto rejected = execution::ExecutionSession::create(context,std::move(options));
  if (rejected) {
    // Lazy sessions defer body validation. Compile the unsupported signature
    // directly so fixtures with arbitrary arguments need not execute to fail.
    REQUIRE(compilation == execution::JITCompilation::Lazy);
    for (auto &module : rejected->context().modules()) for (auto &function : module.functions()) if (function.signature().results.size() > 1) {
      auto compiled = codegen::compile_function(function,*target::TargetMachine::native()); REQUIRE_FALSE(compiled);
      CHECK(compiled.error().message.find("Windows does not support multiple return values") != std::string::npos);
    }
  } else CHECK(rejected.error().message.find("Windows does not support multiple return values") != std::string::npos);
  return true;
}
}
