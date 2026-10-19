/* Licensed under the MIT License. Checked mixed scalar/block native calls. */
#include "mirnext/Execution/detail/NativeCalls.hpp"
#include "mirnext/Execution/detail/Recovery.hpp"
#include "mirnext/Execution/detail/Values.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include <algorithm>
namespace mirnext::execution::detail {
namespace {
Error invalid (std::string text) { return {ErrorCode::InvalidArgument, std::move (text)}; }
}  // namespace
Result<std::uintptr_t> NativeCalls::entry (std::uintptr_t address, const ir::Signature &signature,
                                           std::span<const ExecutionArgument> args, bool transfer) {
  const auto unnamed = args.subspan (signature.parameters.size ());
  if (auto found = entries_.find (address); found != entries_.end ())
    for (auto &e : found->second) {
      if (e.signature != signature || e.variadics.size () != unnamed.size ()
          || e.transfer != transfer)
        continue;
      if (std::equal (e.variadics.begin (), e.variadics.end (), unnamed.begin (),
                      [] (ir::Type type, const auto &arg) { return type == arg.type (); }))
        return e.function.address ();
    }
  // Only cache misses need an owning type sequence.
  std::vector<ir::Type> variadics;
  variadics.reserve (unnamed.size ());
  for (auto &arg : unnamed) variadics.push_back (arg.type ());
  ir::Context context;
  auto module = context.create_module ("$native-call");
  if (!module) return module.error ();
  auto callee = (*module)->import ("callee", signature);
  if (!callee) return callee.error ();
  auto wrapper
    = (*module)->create_function ("entry", {{}, {ir::Type::pointer (), ir::Type::pointer ()}});
  if (!wrapper) return wrapper.error ();
  std::vector<const ir::Register *> hardware;
  for (auto [type, name] : hardware_) {
    auto r = (*wrapper)->create_register (type, {}, name);
    if (!r) return r.error ();
    hardware.push_back (*r);
  }
  auto block = (*wrapper)->create_block ();
  if (!block) return block.error ();
  ir::IRBuilder b (**block);
  auto input = (*wrapper)->arguments ()[0], output = (*wrapper)->arguments ()[1];
  std::vector<ir::CallArgument> operands;
  operands.reserve (args.size ());
  for (std::size_t n = 0; n < args.size (); ++n) {
    auto type = n < signature.parameters.size () ? signature.parameters[n] : args[n].type ();
    auto storage = type.is_block ()     ? ir::Type::pointer ()
                   : type.is_integer () ? ir::Type::i64 ()
                                        : type;
    auto value = b.load (ir::Memory{storage, input, nullptr,
                                    static_cast<std::int64_t> (n * ScalarValue::storage_size)});
    if (!value) return value.error ();
    if (!type.is_block ())
      operands.emplace_back (ir::Operand (**value));
    else if (type.kind () == ir::Type::Kind::ReturnBlock)
      operands.emplace_back (ir::BlockArgument::return_buffer (**value, type.block_size ()));
    else
      operands.emplace_back (ir::BlockArgument (**value, type.block_size (), type.block_class ()));
  }
  if (transfer)
    for (std::size_t n = 0; n < hardware.size (); ++n) {
      auto value
        = b.load (ir::Memory{hardware_[n].first, input, nullptr,
                             std::int64_t ((args.size () + n) * ScalarValue::storage_size)});
      if (!value) return value.error ();
      auto assigned = b.assign (*hardware[n], ir::Operand (**value));
      if (!assigned) return assigned.error ();
    }
  auto called = b.call (**callee, operands);
  if (!called) return called.error ();
  for (std::size_t n = 0; n < signature.results.size (); ++n) {
    auto type = signature.results[n].is_integer () ? ir::Type::i64 () : signature.results[n];
    auto saved = b.store (ir::Memory{type, output, nullptr,
                                     static_cast<std::int64_t> (n * ScalarValue::storage_size)},
                          ir::Operand (*called->results ()[n]));
    if (!saved) return saved.error ();
  }
  if (transfer)
    for (std::size_t n = 0; n < hardware.size (); ++n) {
      auto stored = b.store (ir::Memory{hardware_[n].first, output, nullptr,
                                        std::int64_t ((signature.results.size () + n)
                                                      * ScalarValue::storage_size)},
                             ir::Operand (*hardware[n]));
      if (!stored) return stored.error ();
    }
  auto returned = b.ret ();
  if (!returned) return returned.error ();
  auto optimized = transforms::optimize (context, {.level = transforms::OptimizationLevel::O0});
  if (!optimized) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target) return target.error ();
  auto compiled
    = codegen::compile_function (*optimized->find_function ("$native-call", "entry"), *target,
                                 {.optimization_level = 0,
                                  .preserve_host_registers = true,
                                  .observer = options_.compilation_observer});
  if (!compiled) return compiled.error ();
  auto executable
    = CodeMemory (options_.memory_provider)
        .publish (std::move (*compiled), [&] (auto &request) -> Result<std::uint64_t> {
          auto id = request.builtin  ? request.builtin
                    : request.symbol ? request.symbol->builtin
                                     : std::nullopt;
          if (id) {
            auto builtin = builtin_address (*id);
            if (!builtin) return builtin.error ();
            return std::uint64_t (*builtin);
          }
          if (request.symbol && request.symbol->module == "$native-call"
              && request.symbol->name == "callee")
            return std::uint64_t (address);
          return invalid ("unexpected native-call relocation");
        });
  if (!executable) return executable.error ();
  auto code = executable->address ();
  entries_[address].push_back (
    {signature, std::move (variadics), transfer, std::move (*executable)});
  ++count_;
  return code;
}
Result<std::uintptr_t> NativeCalls::scalar_entry (std::uintptr_t callee,
                                                  const ir::Signature &signature) {
  // A host address can have several typed entry contracts. The usual handle
  // has the identical signature object; aliases can share only an equal one.
  if (auto at = scalars_.find (callee); at != scalars_.end ())
    for (auto &entry : at->second)
      if (entry.signature == &signature || *entry.signature == signature)
        return entry.function.address ();
  // One array pointer crosses the host boundary. Native ABI lowering handles
  // all real callee arguments, including exhausted register banks and stacks.
  ir::Context context;
  auto module = context.create_module ("$host");
  if (!module) return module.error ();
  auto imported = (*module)->import ("callee", signature);
  if (!imported) return imported.error ();
  auto result_type
    = signature.results[0].is_float () ? ir::Type (ir::Type::Kind::F64) : ir::Type::i64 ();
  auto wrapper = (*module)->create_function ("entry", {{result_type}, {ir::Type::pointer ()}});
  if (!wrapper) return wrapper.error ();
  // Reserve observable bindings even when this wrapper never reads them.
  // Otherwise loading arguments could clobber a callee's initial hard value.
  for (auto [type, name] : hardware_) {
    auto bound = (*wrapper)->create_register (type, {}, name);
    if (!bound) return bound.error ();
  }
  auto block = (*wrapper)->create_block ();
  if (!block) return block.error ();
  ir::IRBuilder builder (**block);
  std::vector<ir::CallArgument> arguments;
  arguments.reserve (signature.parameters.size ());
  for (std::size_t n = 0; n < signature.parameters.size (); ++n) {
    // Integer slots hold full bit patterns; the callee signature controls
    // narrow extension at the ordinary MIR call boundary.
    auto type
      = signature.parameters[n].is_float () ? ir::Type (ir::Type::Kind::F64) : ir::Type::i64 ();
    auto value = builder.load (ir::Memory{type, (*wrapper)->arguments ()[0], nullptr,
                                          static_cast<std::int64_t> (n * sizeof (std::uint64_t))});
    if (!value) return value.error ();
    arguments.emplace_back (ir::Operand (**value));
  }
  auto called = builder.call (**imported, arguments);
  if (!called) return called.error ();
  auto value = called->value ();
  if (!value) return value.error ();
  auto returned = builder.ret (ir::Operand (**value));
  if (!returned) return returned.error ();
  auto optimized = transforms::optimize (context, {.level = transforms::OptimizationLevel::O0});
  if (!optimized) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target) return target.error ();
  auto compiled = codegen::compile_function (*optimized->find_function ("$host", "entry"), *target,
                                             {.optimization_level = 0,
                                              .preserve_host_registers = true,
                                              .observer = options_.compilation_observer});
  if (!compiled) return compiled.error ();
  auto executable
    = CodeMemory (options_.memory_provider)
        .publish (std::move (*compiled),
                  [&] (const codegen::CodeSymbolRequest &request) -> Result<std::uint64_t> {
                    if (request.symbol && request.symbol->module == "$host"
                        && request.symbol->name == "callee")
                      return static_cast<std::uint64_t> (callee);
                    return invalid ("unexpected host-entry relocation");
                  });
  if (!executable) return executable.error ();
  auto address = executable->address ();
  scalars_[callee].push_back ({&signature, std::move (*executable)});
  ++count_;
  return address;
}
Result<std::vector<ScalarValue>> NativeCalls::call (std::uintptr_t address,
                                                    const ir::Signature &signature,
                                                    std::span<const ExecutionArgument> args,
                                                    const NativeRecovery *recovery,
                                                    std::span<HardwareSlot> hardware) {
  if (!address) return invalid ("null native call target");
  auto valid = signature.verify ();
  if (!valid) return valid.error ();
  if (args.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size
      || signature.results.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size)
    return invalid ("native call frame size overflow");
  auto count = check_argument_count (signature, args.size ());
  if (!count) return count.error ();
  if (!hardware.empty () && hardware.size () != hardware_.size ())
    return invalid ("hardware transfer bank size mismatch");
  const bool transfer = !hardware.empty ();
  // Validate directly into the ABI frame: a second vector of normalized
  // ExecutionArguments would allocate and copy every scalar on each call.
  const auto limit = std::size_t (INT64_MAX) / ScalarValue::storage_size;
  if (hardware.size () > limit || args.size () > limit - hardware.size ()
      || signature.results.size () > limit - hardware.size ())
    return invalid ("native call frame size overflow");
  const auto inputs = args.size () + hardware.size (),
             outputs = signature.results.size () + hardware.size ();
  if (inputs > SIZE_MAX - outputs) return invalid ("native call frame size overflow");
  // Most ABI calls fit in a small per-invocation frame. Keep it local so a
  // callback can reenter without overwriting the caller's inputs or results.
  alignas (long double) std::array<ScalarValue::Storage, 16> local;
  std::vector<ScalarValue::Storage> overflow;
  std::span<ScalarValue::Storage> slots (local);
  const auto size = inputs + outputs;
  if (size > slots.size ()) {
    overflow.resize (size);
    slots = overflow;
  }
  auto input = slots.first (inputs);
  auto output = slots.subspan (inputs, outputs);
  std::fill (output.begin (), output.end (), ScalarValue::Storage{});
  for (std::size_t n = 0; n < args.size (); ++n) {
    auto type = n < signature.parameters.size () ? signature.parameters[n] : args[n].type ();
    auto v = normalize_argument (type, args[n], n >= signature.parameters.size ());
    if (!v) return v.error ();
    input[n]
      = v->scalar ()
          ? v->scalar ()->storage ()
          : ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (v->block ()->bytes.data ()))
              .storage ();
  }
  for (std::size_t n = 0; n < hardware.size (); ++n) input[args.size () + n] = hardware[n].bytes;
  auto function = entry (address, signature, args, transfer);
  if (!function) return function.error ();
  if (recovery) {
    auto status
      = recovery->invoke<void> (*function, reinterpret_cast<std::uintptr_t> (input.data ()),
                                reinterpret_cast<std::uintptr_t> (output.data ()));
    if (!status) return status.error ();
  } else {
    ExecutionInvocation invocation;
    reinterpret_cast<void (*) (const ScalarValue::Storage *, ScalarValue::Storage *)> (
      *function) (input.data (), output.data ());
    auto status = invocation.status ();
    if (!status) return status.error ();
  }
  for (std::size_t n = 0; n < hardware.size (); ++n)
    hardware[n].bytes = output[signature.results.size () + n];
  std::vector<ScalarValue> result;
  result.reserve (signature.results.size ());
  for (std::size_t n = 0; n < signature.results.size (); ++n) {
    auto value = ScalarValue::from_storage (signature.results[n], output[n]);
    if (!value) return value.error ();
    auto normalized = normalize_scalar (signature.results[n], *value);
    if (!normalized) return normalized.error ();
    result.push_back (*normalized);
  }
  return result;
}
}  // namespace mirnext::execution::detail
