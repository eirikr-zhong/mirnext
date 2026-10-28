/* Licensed under the MIT License. Checked mixed scalar/block native calls. */
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
  ir::Program context;
  auto module = ir::detail::access (context).create_module ("$native-call");
  if (!(module).has_value ()) return module.error ();
  auto callee = ir::detail::access (((module)).value ()).import ("callee", signature);
  if (!(callee).has_value ()) return callee.error ();
  auto wrapper = ir::detail::access (((module)).value ())
                   .create_function ("entry", {{}, {ir::Type::pointer (), ir::Type::pointer ()}});
  if (!(wrapper).has_value ()) return wrapper.error ();
  std::vector<const ir::Register *> hardware;
  for (auto [type, name] : hardware_) {
    auto r = ir::detail::access (((wrapper)).value ()).create_register (type, {}, name);
    if (!(r).has_value ()) return r.error ();
    hardware.push_back (&(r).value ());
  }
  auto block = ir::detail::access (((wrapper)).value ()).create_block ();
  if (!(block).has_value ()) return block.error ();
  ir::InstructionBuilder b ((block).value ());
  auto input = ir::detail::ReferenceAccess::pointers (wrapper.value ().arguments ())[0],
       output = ir::detail::ReferenceAccess::pointers (wrapper.value ().arguments ())[1];
  std::vector<ir::detail::CallArgumentState> operands;
  operands.reserve (args.size ());
  for (std::size_t n = 0; n < args.size (); ++n) {
    auto type = n < signature.parameters.size () ? signature.parameters[n] : args[n].type ();
    auto storage = type.is_block ()     ? ir::Type::pointer ()
                   : type.is_integer () ? ir::Type::i64 ()
                                        : type;
    auto value = b.load (ir::detail::NodeAccess::make_Memory (storage, input, nullptr,
                                                              static_cast<std::int64_t> (
                                                                n * ScalarValue::storage_size)));
    if (!(value).has_value ()) return value.error ();
    if (!type.is_block ())
      operands.emplace_back (ir::Operand ((value).value ()));
    else if (type.kind () == ir::Type::Kind::ReturnBlock)
      operands.emplace_back (
        ir::detail::BlockArgumentState::return_buffer ((value).value (), type.block_size ()));
    else
      operands.emplace_back (
        ir::detail::BlockArgumentState ((value).value (), type.block_size (), type.block_class ()));
  }
  if (transfer)
    for (std::size_t n = 0; n < hardware.size (); ++n) {
      auto value
        = b.load (ir::detail::NodeAccess::make_Memory (hardware_[n].first, input, nullptr,
                                                       std::int64_t ((args.size () + n)
                                                                     * ScalarValue::storage_size)));
      if (!(value).has_value ()) return value.error ();
      auto assigned = b.assign (*hardware[n], ir::Operand ((value).value ()));
      if (!(assigned).has_value ()) return assigned.error ();
    }
  auto called = b.call ((callee).value (), operands);
  if (!(called).has_value ()) return called.error ();
  for (std::size_t n = 0; n < signature.results.size (); ++n) {
    auto type = signature.results[n].is_integer () ? ir::Type::i64 () : signature.results[n];
    auto saved = b.store (ir::detail::NodeAccess::make_Memory (type, output, nullptr,
                                                               static_cast<std::int64_t> (
                                                                 n * ScalarValue::storage_size)),
                          ir::Operand (*ir::detail::ReferenceAccess::pointers (
                            called.value ().results ())[n]));
    if (!(saved).has_value ()) return saved.error ();
  }
  if (transfer)
    for (std::size_t n = 0; n < hardware.size (); ++n) {
      auto stored
        = b.store (ir::detail::NodeAccess::make_Memory (hardware_[n].first, output, nullptr,
                                                        std::int64_t (
                                                          (signature.results.size () + n)
                                                          * ScalarValue::storage_size)),
                   ir::Operand (*hardware[n]));
      if (!(stored).has_value ()) return stored.error ();
    }
  auto returned = b.ret ();
  if (!(returned).has_value ()) return returned.error ();
  auto optimized = transforms::optimize (context, {.level = transforms::OptimizationLevel::O0});
  if (!(optimized).has_value ()) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  auto compiled
    = codegen::compile_function (*optimized.value ().find_function ("$native-call", "entry").get (),
                                 (target).value (),
                                 {.optimization_level = 0,
                                  .preserve_host_registers = true,
                                  .observer = options_.compilation_observer});
  if (!(compiled).has_value ()) return compiled.error ();
  auto executable
    = CodeMemory (options_.memory_provider)
        .publish (std::move ((compiled).value ()), [&] (auto &request) -> Result<std::uint64_t> {
          auto id = request.builtin                  ? request.builtin
                    : request.symbol ().has_value () ? request.symbol ().value ().builtin
                                                     : std::nullopt;
          if (id) {
            auto builtin = builtin_address (*id);
            if (!builtin.has_value ()) return builtin.error ();
            return std::uint64_t (builtin.value ());
          }
          if (request.symbol ().has_value () && request.symbol ().value ().module == "$native-call"
              && request.symbol ().value ().name == "callee")
            return std::uint64_t (address);
          return invalid ("unexpected native-call relocation");
        });
  if (!(executable).has_value ()) return executable.error ();
  auto code = executable.value ().address ();
  entries_[address].push_back (
    {signature, std::move (variadics), transfer, std::move ((executable).value ())});
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
  ir::Program context;
  auto module = ir::detail::access (context).create_module ("$host");
  if (!(module).has_value ()) return module.error ();
  auto imported = ir::detail::access (((module)).value ()).import ("callee", signature);
  if (!(imported).has_value ()) return imported.error ();
  auto result_type
    = signature.results[0].is_float () ? ir::Type (ir::Type::Kind::F64) : ir::Type::i64 ();
  auto wrapper = ir::detail::access (((module)).value ())
                   .create_function ("entry", {{result_type}, {ir::Type::pointer ()}});
  if (!(wrapper).has_value ()) return wrapper.error ();
  // Reserve observable bindings even when this wrapper never reads them.
  // Otherwise loading arguments could clobber a callee's initial hard value.
  for (auto [type, name] : hardware_) {
    auto bound = ir::detail::access (((wrapper)).value ()).create_register (type, {}, name);
    if (!(bound).has_value ()) return bound.error ();
  }
  auto block = ir::detail::access (((wrapper)).value ()).create_block ();
  if (!(block).has_value ()) return block.error ();
  ir::InstructionBuilder builder ((block).value ());
  std::vector<ir::detail::CallArgumentState> arguments;
  arguments.reserve (signature.parameters.size ());
  for (std::size_t n = 0; n < signature.parameters.size (); ++n) {
    // Integer slots hold full bit patterns; the callee signature controls
    // narrow extension at the ordinary MIR call boundary.
    auto type
      = signature.parameters[n].is_float () ? ir::Type (ir::Type::Kind::F64) : ir::Type::i64 ();
    auto value = builder.load (
      ir::detail::NodeAccess::make_Memory (type,
                                           ir::detail::ReferenceAccess::pointers (
                                             wrapper.value ().arguments ())[0],
                                           nullptr,
                                           static_cast<std::int64_t> (n * sizeof (std::uint64_t))));
    if (!(value).has_value ()) return value.error ();
    arguments.emplace_back (ir::Operand ((value).value ()));
  }
  auto called = builder.call ((imported).value (), arguments);
  if (!(called).has_value ()) return called.error ();
  auto value = called.value ().value ();
  if (!(value).has_value ()) return value.error ();
  auto returned = builder.ret (ir::Operand ((value).value ()));
  if (!(returned).has_value ()) return returned.error ();
  auto optimized = transforms::optimize (context, {.level = transforms::OptimizationLevel::O0});
  if (!(optimized).has_value ()) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  auto compiled
    = codegen::compile_function (*optimized.value ().find_function ("$host", "entry").get (),
                                 (target).value (),
                                 {.optimization_level = 0,
                                  .preserve_host_registers = true,
                                  .observer = options_.compilation_observer});
  if (!(compiled).has_value ()) return compiled.error ();
  auto executable
    = CodeMemory (options_.memory_provider)
        .publish (std::move ((compiled).value ()),
                  [&] (const codegen::CodeSymbolRequest &request) -> Result<std::uint64_t> {
                    if (codegen::detail::NodeAccess::symbol (request)
                        && codegen::detail::NodeAccess::symbol (request)->module == "$host"
                        && codegen::detail::NodeAccess::symbol (request)->name == "callee")
                      return static_cast<std::uint64_t> (callee);
                    return invalid ("unexpected host-entry relocation");
                  });
  if (!(executable).has_value ()) return executable.error ();
  auto address = executable.value ().address ();
  scalars_[callee].push_back ({&signature, std::move ((executable).value ())});
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
  if (!(valid).has_value ()) return valid.error ();
  if (args.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size
      || signature.results.size () > std::size_t (INT64_MAX) / ScalarValue::storage_size)
    return invalid ("native call frame size overflow");
  auto count = check_argument_count (signature, args.size ());
  if (!(count).has_value ()) return count.error ();
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
    if (!(v).has_value ()) return v.error ();
    input[n] = v.value ().scalar ().get ()
                 ? v.value ().scalar ().get ()->storage ()
                 : ScalarValue::pointer (
                     reinterpret_cast<std::uintptr_t> (v.value ().block ().get ()->bytes.data ()))
                     .storage ();
  }
  for (std::size_t n = 0; n < hardware.size (); ++n) input[args.size () + n] = hardware[n].bytes;
  auto function = entry (address, signature, args, transfer);
  if (!(function).has_value ()) return function.error ();
  if (recovery) {
    auto status = recovery->invoke<void> ((function).value (),
                                          reinterpret_cast<std::uintptr_t> (input.data ()),
                                          reinterpret_cast<std::uintptr_t> (output.data ()));
    if (!(status).has_value ()) return status.error ();
  } else {
    ExecutionInvocation invocation;
    reinterpret_cast<void (*) (const ScalarValue::Storage *, ScalarValue::Storage *)> (
      (function).value ()) (input.data (), output.data ());
    auto status = invocation.status ();
    if (!(status).has_value ()) return status.error ();
  }
  for (std::size_t n = 0; n < hardware.size (); ++n)
    hardware[n].bytes = output[signature.results.size () + n];
  std::vector<ScalarValue> result;
  result.reserve (signature.results.size ());
  for (std::size_t n = 0; n < signature.results.size (); ++n) {
    auto value = ScalarValue::from_storage (signature.results[n], output[n]);
    if (!(value).has_value ()) return value.error ();
    auto normalized = normalize_scalar (signature.results[n], (value).value ());
    if (!(normalized).has_value ()) return normalized.error ();
    result.push_back ((normalized).value ());
  }
  return result;
}
}  // namespace mirnext::execution::detail
