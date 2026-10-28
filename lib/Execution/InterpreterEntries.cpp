/* Licensed under the MIT License. Native ABI entries dispatch into owned C++ IR. */
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include <algorithm>
namespace mirnext::execution {
std::string_view NativeEntry::module_name () const {
  return owner_->bindings[index_].source->module ().name ();
}
std::string_view NativeEntry::name () const { return owner_->bindings[index_].source->name (); }
const ir::Signature &NativeEntry::signature () const { return *owner_->binding (index_).signature; }
namespace detail {
thread_local InvocationActivation *InvocationActivation::current_ = nullptr;
InvocationActivation::InvocationActivation (InterpreterInvocation &i)
  : invocation_ (i), previous_ (current_) {
  current_ = this;
}
InvocationActivation::~InvocationActivation () { current_ = previous_; }
InterpreterInvocation *InvocationActivation::find (LoadedProgram &s) {
  for (auto at = current_; at; at = at->previous_)
    if (&at->invocation_.state == &s) return &at->invocation_;
  return nullptr;
}
namespace {
Error invalid (std::string text) { return {ErrorCode::InvalidArgument, std::move (text)}; }
template <class T>
T load (std::span<const std::byte> bytes) {
  T value;
  std::memcpy (&value, bytes.data (), sizeof value);
  return value;
}
// This is the actual native ABI boundary. The descriptor is a typed, stable C++
// object owned by the loaded program. Its program and the native frame remain
// alive for this invocation.
void dispatch (LoadedProgram::EntryDescriptor *descriptor, const ScalarValue::Storage *input,
               ScalarValue::Storage *output, const std::byte *native_va, HardwareSlot *physical) {
  auto &state = descriptor->state;
  auto &signature = *state.binding (descriptor->binding).signature;
  for (std::size_t n = 0; n < state.hardware.size (); ++n) state.hardware[n] = physical[n];
  for (std::size_t n = 0; n < signature.results.size (); ++n) output[n] = {};
  auto execute = [&] () -> Result<void> {
    std::vector<ExecutionArgument> arguments;
    arguments.reserve (signature.parameters.size ());
    for (std::size_t n = 0; n < signature.parameters.size (); ++n) {
      auto type = signature.parameters[n];
      if (!type.is_block ()) {
        auto value = ScalarValue::from_storage (type, input[n]);
        if (!(value).has_value ()) return value.error ();
        arguments.emplace_back ((value).value ());
      } else {
        auto address = load<std::uintptr_t> (input[n]);
        if (!address) return invalid ("null native block parameter");
        if (type.kind () == ir::Type::Kind::ReturnBlock)
          arguments.push_back (ExecutionArgument::return_buffer (
            {reinterpret_cast<std::byte *> (address), type.block_size ()}));
        else
          arguments.push_back (
            ExecutionArgument::block ({reinterpret_cast<const std::byte *> (address),
                                       type.block_size ()},
                                      type.block_class ()));
      }
    }
    std::span<const std::byte> variadics;
    if (signature.variadic) {
      if (!native_va) return invalid ("missing native variadic state");
      variadics = {native_va, variadic_state_size (state.target.abi ())};
    }
    InterpreterInvocation invocation{state};
    auto active = InvocationActivation::find (state);
    auto result
      = (active ? *active : invocation).invoke (descriptor->binding, arguments, false, variadics);
    if (!(result).has_value ()) return result.error ();
    for (std::size_t n = 0; n < result.value ().values.size (); ++n)
      output[n] = result.value ().values[n].storage ();
    return {};
  };
  auto result = execute ();
  if (!(result).has_value ()) report_execution_error (result.error ());
  for (std::size_t n = 0; n < state.hardware.size (); ++n) physical[n] = state.hardware[n];
}
Result<void> build_entry (ir::Module &module, const ir::Symbol &dispatcher,
                          LoadedProgram::EntryDescriptor &descriptor, std::size_t va_size,
                          std::string name) {
  auto &signature = *descriptor.state.binding (descriptor.binding).signature;
  if (signature.parameters.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size
      || signature.results.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size)
    return invalid ("native entry frame size overflow");
  auto function = ir::detail::access (module).create_function (name, signature);
  if (!(function).has_value ()) return function.error ();
  std::vector<const ir::Register *> hardware;
  for (auto [type, name] : descriptor.state.hardware_bindings) {
    auto reg = ir::detail::access (((function)).value ()).create_register (type, {}, name);
    if (!(reg).has_value ()) return reg.error ();
    hardware.push_back (&(reg).value ());
  }
  auto block = ir::detail::access (((function)).value ()).create_block ();
  if (!(block).has_value ()) return block.error ();
  ir::InstructionBuilder b ((block).value ());
  auto inputs = b.allocate_stack (std::uint64_t (
    std::max (std::size_t{8}, signature.parameters.size () * ScalarValue::storage_size)));
  if (!(inputs).has_value ()) return inputs.error ();
  auto outputs = b.allocate_stack (std::uint64_t (
    std::max (std::size_t{8}, signature.results.size () * ScalarValue::storage_size)));
  if (!(outputs).has_value ()) return outputs.error ();
  for (std::size_t n = 0; n < signature.parameters.size (); ++n) {
    auto type = signature.parameters[n];
    auto storage = type.is_block ()     ? ir::Type::pointer ()
                   : type.is_integer () ? ir::Type::i64 ()
                                        : type;
    auto width = descriptor.state.target.layout (storage);
    if (!(width).has_value ()) return width.error ();
    // Define unused bytes too: the dispatcher copies complete scalar slots.
    for (std::size_t offset = width.value ().size < 8 ? 0 : width.value ().size;
         offset < ScalarValue::storage_size; offset += 8) {
      auto zero
        = b.store (ir::detail::NodeAccess::make_Memory (ir::Type::i64 (), &(inputs).value (),
                                                        nullptr,
                                                        std::int64_t (n * ScalarValue::storage_size
                                                                      + offset)),
                   0);
      if (!(zero).has_value ()) return zero.error ();
    }
    auto stored
      = b.store (ir::detail::NodeAccess::make_Memory (storage, &(inputs).value (), nullptr,
                                                      std::int64_t (n * ScalarValue::storage_size)),
                 ir::Operand (
                   *ir::detail::ReferenceAccess::pointers (function.value ().arguments ())[n]));
    if (!(stored).has_value ()) return stored.error ();
  }
  ir::Operand va = std::uint64_t{0};
  if (signature.variadic) {
    auto list = b.allocate_stack (std::uint64_t (va_size));
    if (!(list).has_value ()) return list.error ();
    auto started = b.start_varargs (ir::Operand ((list).value ()));
    if (!(started).has_value ()) return started.error ();
    va = ir::Operand ((list).value ());
  }
  ir::Operand bank = std::uint64_t{0};
  const ir::Register *physical = nullptr;
  if (!hardware.empty ()) {
    auto buffer = b.allocate_stack (std::uint64_t (hardware.size () * ScalarValue::storage_size));
    if (!(buffer).has_value ()) return buffer.error ();
    physical = &(buffer).value ();
    bank = ir::Operand (*physical);
    for (std::size_t n = 0; n < hardware.size (); ++n) {
      for (std::size_t offset = 0; offset < ScalarValue::storage_size; offset += 8) {
        auto zero
          = b.store (ir::detail::NodeAccess::make_Memory (ir::Type::i64 (), physical, nullptr,
                                                          std::int64_t (
                                                            n * ScalarValue::storage_size
                                                            + offset)),
                     0);
        if (!(zero).has_value ()) return zero.error ();
      }
      auto saved
        = b.store (ir::detail::NodeAccess::make_Memory (hardware[n]->type (), physical, nullptr,
                                                        std::int64_t (n
                                                                      * ScalarValue::storage_size)),
                   ir::Operand (*hardware[n]));
      if (!(saved).has_value ()) return saved.error ();
    }
  }
  auto called
    = b.call (dispatcher,
              {ir::Operand (std::uint64_t (reinterpret_cast<std::uintptr_t> (&descriptor))),
               ir::Operand ((inputs).value ()), ir::Operand ((outputs).value ()), va, bank});
  if (!(called).has_value ()) return called.error ();
  for (std::size_t n = 0; n < hardware.size (); ++n) {
    auto value
      = b.load (ir::detail::NodeAccess::make_Memory (hardware[n]->type (), physical, nullptr,
                                                     std::int64_t (n * ScalarValue::storage_size)));
    if (!(value).has_value ()) return value.error ();
    auto assigned = b.assign (*hardware[n], ir::Operand ((value).value ()));
    if (!(assigned).has_value ()) return assigned.error ();
  }
  std::vector<ir::Operand> returned;
  for (std::size_t n = 0; n < signature.results.size (); ++n) {
    auto type = signature.results[n].is_integer () ? ir::Type::i64 () : signature.results[n];
    auto value
      = b.load (ir::detail::NodeAccess::make_Memory (type, &(outputs).value (), nullptr,
                                                     std::int64_t (n * ScalarValue::storage_size)));
    if (!(value).has_value ()) return value.error ();
    returned.emplace_back ((value).value ());
  }
  auto end = b.ret (returned);
  if (!(end).has_value ()) return end.error ();
  return {};
}
}  // namespace
bool InterpreterInvocation::native_variadic_cursor (std::span<const std::byte> cursor) const {
  if (native_variadics.empty ()) return false;
  auto abi = state.target.abi ();
  if (cursor.size () != variadic_state_size (abi)) return false;
  auto stack = load<std::uintptr_t> (cursor.subspan (abi == target::ABI::X86SysV ? 8 : 0));
  for (auto area : variadic_areas) {
    auto base = reinterpret_cast<std::uintptr_t> (area.data ());
    if (stack >= base && stack - base <= area.size ()) return false;
  }
  for (auto source : native_variadics) {
    if (source.size () != cursor.size ()) continue;
    if (abi == target::ABI::AArch64Linux) {
      struct List {
        std::uintptr_t stack, gp, fp;
        std::int32_t go, fo;
      };
      auto a = load<List> (source), b = load<List> (cursor);
      if (b.stack >= a.stack && b.gp == a.gp && b.fp == a.fp && b.go >= a.go && b.fo >= a.fo)
        return true;
    } else if (abi == target::ABI::X86SysV) {
      struct List {
        std::uint32_t gp, fp;
        std::uintptr_t stack, registers;
      };
      auto a = load<List> (source), b = load<List> (cursor);
      if (b.stack >= a.stack && b.registers == a.registers && b.gp >= a.gp && b.fp >= a.fp)
        return true;
    } else if (load<std::uintptr_t> (cursor) >= load<std::uintptr_t> (source))
      return true;
  }
  return false;
}
Result<void> LoadedProgram::publish_entries () {
  auto failure = [&] (const ir::Symbol *symbol, Error e) -> Result<void> {
    e.message = std::string (target.name ()) + ": " + e.message;
    return error (symbol, "native-entry", std::move (e));
  };
  for (auto &entry : bindings)
    if (!entry.alias && entry.kind == Kind::Function) {
      if (!entry.address && !entry.source->function ().get () && entry.jump)
        return failure (entry.source, invalid ("a callback-only jump-return import needs a native "
                                               "ABI implementation for native exposure"));
      if (!entry.address && !entry.source->function ().get () && entry.signature->variadic)
        return failure (entry.source, invalid ("a variadic callback-only import needs a native ABI "
                                               "implementation for native exposure"));
    }
  ir::Program wrappers;
  auto module = ir::detail::access (wrappers).create_module ("$interpreter-entry");
  if (!(module).has_value ()) return module.error ();
  auto dispatcher
    = ir::detail::access (((module)).value ())
        .import ("dispatch",
                 ir::Signature{{},
                               {ir::Type::pointer (), ir::Type::pointer (), ir::Type::pointer (),
                                ir::Type::pointer (), ir::Type::pointer ()}});
  if (!(dispatcher).has_value ()) return dispatcher.error ();
  ir::StringMap<std::size_t> callbacks;
  for (std::size_t n = 0; n < bindings.size (); ++n) {
    auto &entry = bindings[n];
    if (entry.source->function ().get () && engine (n) != ExecutionEngine::Interpreter) continue;
    if (has_native && !entry.source->function ().get () && entry.address) continue;
    if (entry.alias || entry.kind != Kind::Function || entry.jump
        || (entry.address && (!entry.interpret || entry.signature->variadic)))
      continue;
    if (!entry.source->function ().get ()) {
      auto [at, inserted] = callbacks.try_emplace (entry.source->name (), n);
      if (!inserted) {
        entry.alias = at->second;
        continue;
      }
    }
    auto descriptor = std::make_unique<EntryDescriptor> (EntryDescriptor{*this, n});
    auto built = build_entry ((module).value (), (dispatcher).value (), *descriptor,
                              variadic_state_size (target.abi ()), "entry." + std::to_string (n));
    if (!(built).has_value ()) return failure (entry.source, built.error ());
    entry_descriptors.push_back (std::move (descriptor));
  }
  if (entry_descriptors.empty ()) return {};
  auto program = transforms::optimize (wrappers, {.level = transforms::OptimizationLevel::O0});
  if (!(program).has_value ()) return failure (nullptr, program.error ());
  std::vector<codegen::CompiledFunction> artifacts;
  std::vector<std::size_t> sizes;
  for (auto &descriptor : entry_descriptors) {
    auto fn
      = program.value ()
          .find_function ("$interpreter-entry", "entry." + std::to_string (descriptor->binding))
          .get ();
    auto compiled = codegen::compile_function (*fn, target,
                                               {.optimization_level = 0,
                                                .preserve_host_registers = !has_native,
                                                .observer = options.link.compilation_observer});
    if (!(compiled).has_value ())
      return failure (bindings[descriptor->binding].source, compiled.error ());
    sizes.push_back (compiled.value ().bytes ().size ());
    statistics.code_bytes += compiled.value ().bytes ().size ();
    artifacts.push_back (std::move ((compiled).value ()));
  }
  auto reservation = CodeMemory (options.link.memory_provider).reserve (sizes);
  if (!(reservation).has_value ()) return failure (nullptr, reservation.error ());
  std::vector<std::vector<std::uint8_t>> buffers;
  for (std::size_t n = 0; n < artifacts.size (); ++n) {
    auto address = reservation.value ().address (n);
    if (!(address).has_value ()) return failure (nullptr, address.error ());
    auto bytes
      = artifacts[n]
          .relocate ((address).value (),
                     [&] (const codegen::CodeSymbolRequest &request) -> Result<std::uint64_t> {
                       auto id = request.builtin ? request.builtin
                                 : codegen::detail::NodeAccess::symbol (request)
                                   ? codegen::detail::NodeAccess::symbol (request)->builtin
                                   : std::nullopt;
                       if (id) {
                         auto value = builtin_address (*id);
                         if (!(value).has_value ()) return value.error ();
                         return std::uint64_t ((value).value ());
                       }
                       if (codegen::detail::NodeAccess::symbol (request)
                           && codegen::detail::NodeAccess::symbol (request)->module
                                == "$interpreter-entry"
                           && codegen::detail::NodeAccess::symbol (request)->name == "dispatch")
                         return std::uint64_t (reinterpret_cast<std::uintptr_t> (&dispatch));
                       return invalid ("unexpected interpreter entry relocation");
                     });
    if (!(bytes).has_value ())
      return failure (bindings[entry_descriptors[n]->binding].source, bytes.error ());
    buffers.push_back (std::move ((bytes).value ()));
  }
  std::vector<std::span<const std::uint8_t>> views;
  for (auto &buffer : buffers) views.emplace_back (buffer);
  auto published = reservation.value ().publish (views);
  if (!(published).has_value ()) return failure (nullptr, published.error ());
  for (std::size_t n = 0; n < artifacts.size (); ++n) {
    auto &code = ((published).value ())[n];
    bindings[entry_descriptors[n]->binding].address = code.address ();
    entries.push_back ({std::move (artifacts[n]), std::move (code)});
  }
  ++statistics.code_batches;
  return {};
}
}  // namespace detail
}  // namespace mirnext::execution
