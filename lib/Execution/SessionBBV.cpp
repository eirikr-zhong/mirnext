/* Licensed under the MIT License. Block compilation on the shared loaded image. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution::detail {
Result<void> LoadedProgram::prepare_block_versioning() {
  // All functions and stable block gates exist before resolving any code/data
  // reference. Preparation owns its machine body and does not compile blocks.
  for (std::size_t n = 0; n < functions.size(); ++n) {
    auto &record = *functions[n];
    if (record.engine != ExecutionEngine::BlockVersioning) continue;
    auto fail = [&](Error e) { return error(&record.source->symbol(),"bbv-prepare",std::move(e)); };
    auto prepared = codegen::BBVPreparation::prepare(*record.source,target,
      {.optimization_level=static_cast<unsigned>(optimized->level()),.observer=options.link.compilation_observer});
    if (!prepared) return fail(prepared.error());
    auto program = codegen::BBVProgram::create(std::move(*prepared)); if (!program) return fail(program.error());
    auto runtime = BBVRuntime::create(std::move(*program),{
      .code_capacity=options.link.block_versioning.code_capacity,
      .failure_continuation=native_boundary->recovery().recovery_address(),
      .memory_provider=options.link.memory_provider,
      .resolve=[this](auto &request) { return resolve_code(request); },
      .observe=[this](auto &version,auto &compiled) -> Result<void> {
        if (options.link.block_versioning.observer) return options.link.block_versioning.observer({version,compiled});
        return {};
      }});
    if (!runtime) return fail(runtime.error());
    record.blocks = std::move(*runtime); record.destination = record.blocks->entry_address();
    std::vector<const ir::Label *> labels; for (auto &label : record.source->labels()) labels.push_back(&label);
    for (auto &label : record.blocks->program().preparation().function().labels()) if (auto ordinal = label.source_ordinal()) {
      if (*ordinal >= labels.size()) return fail({ErrorCode::InvalidOperand,"invalid source label correspondence"});
      block_labels.emplace(labels[*ordinal],&label);
    }
    auto &entry = bindings[identities.at(&record.source->symbol())]; entry.address = record.destination;
  }
  return {};
}
const LinkStatistics &LoadedProgram::link_statistics() {
  total_statistics = statistics;
  if (native_boundary) {
    total_statistics.code_bytes += native_boundary->code_bytes(); total_statistics.code_batches += native_boundary->code_batches();
  }
  for (auto &record : functions) if (record->blocks) {
    auto &runtime = *record->blocks; auto &s = runtime.statistics();
    total_statistics.code_bytes += s.code_bytes; total_statistics.code_batches += s.code_batches;
    total_statistics.compiled_blocks += s.compiled_blocks; total_statistics.block_compilation_attempts += s.compilation_attempts;
    if (runtime.artifact(runtime.entry_version().id())) ++total_statistics.compiled_functions;
  }
  return total_statistics;
}
}
