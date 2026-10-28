/* Licensed under the MIT License. Native compilation on the shared loaded program. */
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "mirnext/CodeGen/RuntimeEntry.hpp"
#include <algorithm>
namespace mirnext::execution::detail {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
}  // namespace
Result<std::size_t> LoadedProgram::find_native (std::string_view module,
                                                std::string_view name) const {
  auto found = ProgramImage::find (module, name);
  if ((found).has_value ()) return found;
  if (optimized)
    if (auto f = optimized->find_function (module, name).get ())
      return identities.at (&f->symbol ());
  return found;
}
Result<std::uint64_t> LoadedProgram::resolve_code (
  const codegen::CodeSymbolRequest &request) const {
  auto builtin = [] (codegen::Builtin id) -> Result<std::uint64_t> {
    auto address = builtin_address (id);
    if (!(address).has_value ()) return address.error ();
    return static_cast<std::uint64_t> ((address).value ());
  };
  if (request.builtin) return builtin (*request.builtin);
  if (!codegen::detail::NodeAccess::symbol (request)) return invalid ("missing typed code symbol");
  auto &symbol = *codegen::detail::NodeAccess::symbol (request);
  if (symbol.builtin) return builtin (*symbol.builtin);
  auto module = context.find_module (symbol.module).get ();
  const ir::Symbol *source = nullptr;
  if (module && !symbol.name.empty ())
    source = module->find_symbol (symbol.name).get ();
  else if (module && symbol.anonymous_ordinal) {
    std::size_t n = 0;
    for (auto &s : module->symbols ())
      if (n++ == *symbol.anonymous_ordinal) {
        source = &s;
        break;
      }
  }
  if (!source)
    return Error{ErrorCode::UnknownName,
                 "unknown code symbol " + symbol.module + '/' + symbol.name};
  auto &value = binding (identities.at (source));
  if (!value.address || value.kind == Kind::Prototype)
    return invalid ("symbol has no runtime address");
  return value.address;
}
namespace {
Result<void> labels_for (const ir::Function &f, const codegen::CompiledFunction &artifact) {
  const auto count = std::distance (f.labels ().begin (), f.labels ().end ());
  for (auto &label : artifact.labels ())
    if (label.source_ordinal && *label.source_ordinal >= std::size_t (count))
      return invalid ("invalid source label correspondence");
  return {};
}
void install_labels (LoadedProgram &program, std::size_t index) {
  auto &record = *program.functions[index];
  std::vector<const ir::Label *> labels;
  for (auto &label : record.source->labels ()) labels.push_back (&label);
  for (auto &label : record.artifact->labels ())
    if (label.source_ordinal)
      program.label_offsets.emplace (labels[*label.source_ordinal], std::pair{index, label.offset});
}
std::uintptr_t resolve_lazy (LoadedProgram::LazyDescriptor *descriptor) {
  auto result = descriptor->state.compile_native (descriptor->function);
  if (!(result).has_value ()) {
    report_execution_error (result.error ());
    return 0;
  }
  return descriptor->state.functions[descriptor->function]->destination;
}
}  // namespace
Result<void> LoadedProgram::compile_native (std::size_t index) {
  if (functions[index]->engine == ExecutionEngine::Interpreter)
    return invalid ("explicit native compilation requires JIT mode");
  if (auto &blocks = functions[index]->blocks)
    return blocks->compile (blocks->entry_version ().id ());
  auto &record = *functions[index];
  if (record.state == CodeState::Compiled) return {};
  auto fail = [&] (Error e) -> Result<void> {
    return error (&record.source->symbol (), "lazy-compile", std::move (e));
  };
  if (record.state == CodeState::Compiling)
    return fail (invalid ("recursive compilation of the same function"));
  record.state = CodeState::Compiling;
  struct Reset {
    Code &record;
    ~Reset () {
      if (record.state == CodeState::Compiling) record.state = CodeState::Uncompiled;
    }
  } reset{record};
  auto compiled
    = codegen::compile_function (*record.source, target,
                                 {.optimization_level = static_cast<unsigned> (optimized->level ()),
                                  .observer = options.link.compilation_observer});
  if (!(compiled).has_value ()) return fail (compiled.error ());
  auto labels = labels_for (*record.source, (compiled).value ());
  if (!(labels).has_value ()) return fail (labels.error ());
  if (compiled.value ().bytes ().size () > SIZE_MAX - statistics.code_bytes)
    return fail (invalid ("total code size overflow"));
  const std::array sizes{compiled.value ().bytes ().size ()};
  auto reservation = CodeMemory (options.link.memory_provider).reserve (sizes);
  if (!(reservation).has_value ()) return fail (reservation.error ());
  auto address = reservation.value ().address (0);
  if (!(address).has_value ()) return fail (address.error ());
  auto bytes = compiled.value ().relocate ((address).value (),
                                           [&] (auto &request) { return resolve_code (request); });
  if (!(bytes).has_value ()) return fail (bytes.error ());
  const std::array<std::span<const std::uint8_t>, 1> views{(bytes).value ()};
  auto executable = reservation.value ().publish (views);
  if (!(executable).has_value ()) return fail (executable.error ());
  // Publish the destination only after relocation and publication both succeed.
  statistics.code_bytes += compiled.value ().bytes ().size ();
  ++statistics.code_batches;
  ++statistics.compiled_functions;
  record.artifact = std::move ((compiled).value ());
  record.code = std::move (((executable).value ())[0]);
  install_labels (*this, index);
  record.destination = (address).value ();
  record.state = CodeState::Compiled;
  return {};
}
Result<void> LoadedProgram::publish_lazy_entries () {
  std::vector<std::vector<std::uint8_t>> buffers;
  std::vector<std::size_t> sizes;
  auto count = std::count_if (functions.begin (), functions.end (),
                              [] (auto &f) { return f->engine == ExecutionEngine::LazyJIT; });
  if (!count) return {};
  buffers.reserve (statistics.functions + 1);
  sizes.reserve (statistics.functions + 1);
  lazy_descriptors.reserve (statistics.functions);
  auto shared
    = codegen::make_lazy_resolver (target, reinterpret_cast<std::uintptr_t> (&resolve_lazy));
  if (!(shared).has_value ()) return error (nullptr, "lazy-entry", shared.error ());
  sizes.push_back (shared.value ().size ());
  buffers.push_back (std::move ((shared).value ()));
  auto x87_results = [&] (const Code &record) {
    unsigned count = 0;
    if (target.architecture () == target::Architecture::X86_64)
      for (auto type : record.source->signature ().results)
        if (target.scalar_type (type).kind () == ir::Type::Kind::LongDouble) ++count;
    return count;
  };
  for (std::size_t n = 0; n < functions.size (); ++n)
    if (functions[n]->engine == ExecutionEngine::LazyJIT) {
      auto descriptor = std::make_unique<LazyDescriptor> (LazyDescriptor{*this, n});
      sizes.push_back (codegen::lazy_entry_bytes);
      buffers.emplace_back ();
      lazy_descriptors.push_back (std::move (descriptor));
    }
  auto total = statistics.code_bytes;
  for (auto size : sizes) {
    if (size > SIZE_MAX - total) return invalid ("total code size overflow");
    total += size;
  }
  auto reserved = CodeMemory (options.link.memory_provider).reserve (sizes);
  if (!(reserved).has_value ()) return error (nullptr, "lazy-entry", reserved.error ());
  auto resolver = reserved.value ().address (0);
  if (!(resolver).has_value ()) return resolver.error ();
  for (std::size_t n = 0; n < lazy_descriptors.size (); ++n) {
    auto &descriptor = lazy_descriptors[n];
    auto &record = functions[descriptor->function];
    auto bytes
      = codegen::make_lazy_entry (target, {reinterpret_cast<std::uintptr_t> (&record->destination),
                                           reinterpret_cast<std::uintptr_t> (descriptor.get ()),
                                           (resolver).value (), x87_results (*record)});
    if (!(bytes).has_value ())
      return error (&record->source->symbol (), "lazy-entry", bytes.error ());
    buffers[n + 1] = std::move ((bytes).value ());
  }
  std::vector<std::span<const std::uint8_t>> views;
  for (auto &b : buffers) views.emplace_back (b);
  auto published = reserved.value ().publish (views);
  if (!(published).has_value ()) return error (nullptr, "lazy-entry", published.error ());
  for (std::size_t n = 0; n < lazy_descriptors.size (); ++n) {
    auto &f = *functions[lazy_descriptors[n]->function];
    bindings[identities.at (&f.source->symbol ())].address
      = ((published).value ())[n + 1].address ();
  }
  statistics.code_bytes = total;
  lazy_entries = std::move ((published).value ());
  ++statistics.code_batches;
  return {};
}
Result<void> LoadedProgram::prepare_lazy_roots () {
  // A raw label address must designate actual body code before relocation.
  for (auto &module : context.modules ())
    for (auto &data : module.data ())
      if (auto ref = std::get_if<ir::LabelReference> (&data.initializer ())) {
        for (auto label :
             {ir::detail::NodeAccess::label (*(ref)), ir::detail::NodeAccess::base (*(ref))})
          if (label) {
            auto index = *binding (identities.at (&label->function ().symbol ())).function;
            if (functions[index]->engine == ExecutionEngine::LazyJIT) {
              auto compiled = compile_native (index);
              if (!(compiled).has_value ()) return compiled;
            }
          }
      }
  // JCALL/JRET do not offer the ordinary ABI return path for a failed resolver.
  // Materialize these nonstandard entries before any user code can reach them.
  for (auto &entry : bindings)
    if (!entry.alias && entry.function && entry.jump
        && functions[*entry.function]->engine == ExecutionEngine::LazyJIT) {
      auto compiled = compile_native (*entry.function);
      if (!(compiled).has_value ()) return compiled;
    }
  return {};
}
Result<std::uint64_t> LoadedProgram::native_label_address (const ir::Label &source) const {
  auto &entry = binding (identities.at (&source.function ().symbol ()));
  if (!entry.function) return invalid ("label owner is not compiled");
  if (auto &blocks = functions[*entry.function]->blocks) {
    if (auto found = block_labels.find (&source); found != block_labels.end ()) {
      auto address = blocks->label_address (*found->second);
      if (!(address).has_value ()) return address.error ();
      return static_cast<std::uint64_t> ((address).value ());
    }
    return invalid ("source label has no block entry correspondence");
  }
  auto found = label_offsets.find (&source);
  if (found != label_offsets.end () && found->second.first == *entry.function)
    return functions[*entry.function]->destination + found->second.second;
  return invalid ("source label has no compiled address");
}
Result<std::optional<CodeReservation>> LoadedProgram::prepare_eager () {
  std::vector<std::size_t> indices, sizes;
  for (std::size_t n = 0; n < functions.size (); ++n)
    if (functions[n]->engine == ExecutionEngine::EagerJIT) {
      auto &record = functions[n];
      indices.push_back (n);
      record->state = CodeState::Compiling;
      auto compiled = codegen::compile_function (*record->source, target,
                                                 {.optimization_level
                                                  = static_cast<unsigned> (optimized->level ()),
                                                  .observer = options.link.compilation_observer});
      if (!(compiled).has_value ())
        return error (&record->source->symbol (), "eager-compile", compiled.error ());
      auto labels = labels_for (*record->source, (compiled).value ());
      if (!(labels).has_value ()) return labels.error ();
      sizes.push_back (compiled.value ().bytes ().size ());
      record->artifact = std::move ((compiled).value ());
      install_labels (*this, n);
    }
  if (indices.empty ()) return std::optional<CodeReservation>{};
  auto reservation = CodeMemory (options.link.memory_provider).reserve (sizes);
  if (!(reservation).has_value ()) return error (nullptr, "reserve", reservation.error ());
  for (std::size_t n = 0; n < indices.size (); ++n) {
    auto address = reservation.value ().address (n);
    if (!(address).has_value ()) return address.error ();
    auto &record = *functions[indices[n]];
    record.destination = (address).value ();
    bindings[identities.at (&record.source->symbol ())].address = (address).value ();
  }
  return std::optional<CodeReservation> (std::move ((reservation).value ()));
}
Result<void> LoadedProgram::publish_eager (CodeReservation &reservation) {
  std::vector<std::vector<std::uint8_t>> buffers;
  std::vector<std::size_t> indices;
  for (std::size_t n = 0; n < functions.size (); ++n)
    if (functions[n]->engine == ExecutionEngine::EagerJIT) {
      auto &record = *functions[n];
      indices.push_back (n);
      auto bytes = record.artifact->relocate (record.destination, [&] (auto &request) {
        return resolve_code (request);
      });
      if (!(bytes).has_value ())
        return error (&record.source->symbol (), "relocate", bytes.error ());
      buffers.push_back (std::move ((bytes).value ()));
    }
  std::vector<std::span<const std::uint8_t>> views;
  for (auto &buffer : buffers) views.emplace_back (buffer);
  auto published = reservation.publish (views);
  if (!(published).has_value ()) return error (nullptr, "publish", published.error ());
  for (std::size_t n = 0; n < indices.size (); ++n) {
    auto &record = *functions[indices[n]];
    record.code = std::move (((published).value ())[n]);
    record.state = CodeState::Compiled;
    statistics.code_bytes += record.artifact->bytes ().size ();
    ++statistics.compiled_functions;
  }
  ++statistics.code_batches;
  return {};
}
}  // namespace mirnext::execution::detail
