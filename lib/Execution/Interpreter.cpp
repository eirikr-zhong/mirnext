#include "mirnext/IR/Vector.hpp"
/* Licensed under the MIT License. Owning interpreter preparation and dispatch. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include <algorithm>
namespace mirnext::execution {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
}  // namespace
namespace detail {
Result<void> LoadedProgram::prepare () {
  for (auto &record : functions)
    if (record->engine == ExecutionEngine::Interpreter) {
      auto &fn = *record->source;
      auto body = std::make_unique<InterpreterBody> ();
      body->source = &fn;
      std::unordered_map<const ir::Register *, std::size_t> regs;
      for (auto &r : fn.registers ()) {
        regs.emplace (&r, body->registers.size ());
        body->registers.push_back (&r);
        auto h = no_slot;
        if (!r.hardware_binding ().empty ()) {
          auto reg = target.find_register (r.hardware_binding ());
          if (!(reg).has_value ()) return error (&fn.symbol (), "prepare", reg.error ());
          if (!target.accepts ((reg).value (), r.type ()))
            return error (&fn.symbol (), "prepare", invalid ("invalid hardware binding type"));
          h = hardware_indices.at (reg.value ().encoding ());
        }
        body->hardware.push_back (h);
      }
      for (auto a : ir::detail::ReferenceAccess::pointers (fn.arguments ()))
        body->arguments.push_back (regs.at (a));
      for (auto &b : fn.blocks ()) {
        for (auto l : ir::detail::ReferenceAccess::pointers (b.labels ()))
          body->labels.emplace (l, body->operations.size ());
        for (auto &i : b.instructions ()) {
          auto descriptor = ir::instruction_descriptor (i.opcode ()).get ();
          PreparedOperation operation{&i, descriptor, {}};
          using O = ir::Opcode;
          auto op = i.opcode ();
          bool scalar = descriptor->count == ir::OperandCount::Fixed && i.operands ().size () >= 2
                        && !descriptor->roles.empty ()
                        && (descriptor->roles[0] == 'I' || descriptor->roles[0] == 'F'
                            || descriptor->roles[0] == 'D' || descriptor->roles[0] == 'E'
                            || descriptor->roles[0] == 'l');
          scalar &= op != O::Addr && op != O::Addr8 && op != O::Addr16 && op != O::Addr32
                    && op != O::LAddr && op != O::VaArg && op != O::Alloca && op != O::PrBeq
                    && op != O::PrBne;
          if (scalar)
            operation.scalar_inputs = static_cast<std::uint8_t> (i.operands ().size () - 1);
          if (ir::detail::vector_opcode (op))
            operation.vector_inputs = static_cast<std::uint8_t> (i.operands ().size () - 1);
          operation.operands.reserve (i.operands ().size ());
          for (auto &o : i.operands ()) {
            PreparedOperand p{&o};
            if (auto r = ir::detail::operand_access (o).get<const ir::Register *> ())
              p.reg = regs.at (*r);
            if (auto mem = ir::detail::operand_access (o).get<ir::Memory> ()) {
              if (ir::detail::NodeAccess::base (*(mem)))
                p.base = regs.at (ir::detail::NodeAccess::base (*(mem)));
              if (ir::detail::NodeAccess::index (*(mem)))
                p.index = regs.at (ir::detail::NodeAccess::index (*(mem)));
            }
            if (auto ref = ir::detail::operand_access (o).get<ir::SymbolReference> ())
              p.binding = identities.at (ir::detail::NodeAccess::symbol (*(ref)));
            operation.operands.push_back (p);
          }
          body->operations.push_back (std::move (operation));
          ++prepared.instructions;
        }
      }
      for (auto &i : body->operations)
        for (auto &o : i.operands)
          if (auto l = ir::detail::operand_access (*(o.source)).get<const ir::Label *> ())
            o.pc = body->labels.at (*l);
      ++prepared.functions;
      if (!options.native_entries || bindings[identities.at (&fn.symbol ())].jump) {
        auto address = reinterpret_cast<std::uintptr_t> (body.get ());
        bindings[identities.at (&fn.symbol ())].address = address;
        if (native_calls || options.native_calls) virtual_functions.insert (address);
      }
      record->interpreter = std::move (body);
    }
  for (auto &record : functions)
    if (auto &body = record->interpreter)
      for (auto &[label, pc] : body->labels) {
        auto address = reinterpret_cast<std::uintptr_t> (body->operations.data () + pc);
        locations.try_emplace (address, Location{body.get (), pc});
      }
  return {};
}
Result<std::uintptr_t> LoadedProgram::label_address (const ir::Label &label) const {
  auto index = binding (identities.at (&label.function ().symbol ())).function;
  if (!index || !functions[*index]->interpreter) return invalid ("unknown interpreted label owner");
  auto &body = *functions[*index]->interpreter;
  auto pc = body.labels.find (&label);
  if (pc == body.labels.end ()) return invalid ("unknown prepared label");
  return reinterpret_cast<std::uintptr_t> (body.operations.data () + pc->second);
}
Result<void> LoadedProgram::writable (std::uintptr_t address, std::size_t size) const {
  if (!size) return {};
  if (!address || size > UINTPTR_MAX - address) return invalid ("invalid memory write range");
  auto at = std::lower_bound (readonly.begin (), readonly.end (), address + size,
                              [] (auto range, std::uintptr_t end) { return range.first < end; });
  if (at != readonly.begin () && std::prev (at)->second > address)
    return invalid ("write to read-only loaded data");
  return {};
}
Result<InterpreterOutcome> InterpreterInvocation::invoke (
  std::size_t index, std::span<const ExecutionArgument> arguments, bool jump,
  std::span<const std::byte> native_va) {
  auto &entry = state.binding (index);
  auto fn = entry.source->function ().get ();
  auto fail = [&] (Error e) -> Result<InterpreterOutcome> {
    return state.error (entry.source, "interpret", std::move (e));
  };
  if (entry.kind != ProgramImage::Kind::Function || !entry.signature || entry.jump != jump)
    return fail (invalid ("call target kind or calling convention mismatch"));
  auto &sig = *entry.signature;
  auto checked = normalize_arguments (sig, arguments, !fn && bool (entry.interpret) && !jump);
  if (!(checked).has_value ()) return fail (checked.error ());
  auto &normalized = checked.value ().values;
  if (state.options.call_depth_limit && depth >= state.options.call_depth_limit)
    return fail (invalid ("interpreter call depth limit exceeded"));
  struct Depth {
    std::size_t &value;
    Depth (std::size_t &v) : value (v) { ++value; }
    ~Depth () { --value; }
  } guard (depth);
  InvocationActivation activation (*this);
  Result<InterpreterOutcome> outcome
    = invalid ("native foreign calls are disabled or require a typed interpreter implementation");
  const bool native_definition = fn && state.engine (index) != ExecutionEngine::Interpreter;
  if (fn && !native_definition) {
    if (!entry.function || !state.functions[*entry.function]->interpreter)
      return fail (invalid ("missing prepared function"));
    InterpreterFrame frame (*this, *state.functions[*entry.function]->interpreter);
    auto initialized = frame.initialize (normalized, native_va);
    if (!(initialized).has_value ()) return fail (initialized.error ());
    outcome = frame.run ();
  } else if (jump) {
    if (native_definition)
      return fail (invalid ("JCALL/JRET cannot cross interpreted and native execution domains"));
    return fail (
      invalid ("external jcall requires a nonstandard ABI entry; no native adapter is available"));
  } else if (entry.interpret) {
    auto values = entry.interpret (normalized);
    if (!(values).has_value ()) return fail (values.error ());
    outcome = InterpreterOutcome{std::move ((values).value ()), {}};
  } else if (state.native_calls && (native_definition || state.options.native_calls)) {
    auto values = call_native (entry.address, sig, normalized, native_definition);
    if (!(values).has_value ()) return fail (values.error ());
    outcome = InterpreterOutcome{std::move ((values).value ()), {}};
  }
  if (!(outcome).has_value ()) return fail (outcome.error ());
  if (jump) {
    if (!outcome.value ().jump) return fail (invalid ("jcall target did not return through jret"));
    return outcome;
  }
  if (outcome.value ().jump || outcome.value ().values.size () != sig.results.size ())
    return fail (invalid ("call result count or convention mismatch"));
  for (std::size_t n = 0; n < sig.results.size (); ++n) {
    auto normalized = normalize_scalar (sig.results[n], outcome.value ().values[n]);
    if (!(normalized).has_value ()) return fail (normalized.error ());
    outcome.value ().values[n] = std::move ((normalized).value ());
  }
  return outcome;
}
Result<std::vector<ScalarValue>> InterpreterInvocation::call_native (
  std::uintptr_t address, const ir::Signature &signature,
  std::span<const ExecutionArgument> arguments, bool native_definition) {
  if (!state.options.native_calls && !native_definition)
    return invalid (
      "native foreign calls are disabled or require a typed interpreter implementation");
  for (auto &argument : arguments)
    if (auto scalar = argument.scalar ().get (); scalar && scalar->type ().is_integer ()) {
      auto value = scalar->as_u64 ();
      if ((value).has_value () && state.virtual_functions.contains ((value).value ()))
        return invalid (state.options.native_entries
                          ? "nonstandard interpreted function has no ordinary native ABI entry"
                          : "passing an interpreted function address to native code requires "
                            "native_entries at load time");
    }
  // A function pointer supplied by the caller need not be an imported symbol.
  // Keep native ABI support cold until such a call actually occurs.
  if (!state.native_calls)
    state.native_calls
      = std::make_unique<NativeCalls> (state.options.link, state.hardware_bindings);
  return state.native_calls->call (address, signature, arguments,
                                   state.native_boundary ? &state.native_boundary->recovery ()
                                                         : nullptr,
                                   state.hardware);
}
Result<InterpreterOutcome> InterpreterInvocation::invoke_foreign (
  std::uintptr_t address, const ir::Signature &signature,
  std::span<const ExecutionArgument> arguments) {
  if (!address) return invalid ("null native call target");
  if (state.options.call_depth_limit && depth >= state.options.call_depth_limit)
    return invalid ("interpreter call depth limit exceeded");
  struct Depth {
    std::size_t &value;
    Depth (std::size_t &v) : value (v) { ++value; }
    ~Depth () { --value; }
  } guard (depth);
  InvocationActivation activation (*this);
  auto values = call_native (address, signature, arguments);
  if (!(values).has_value ()) return values.error ();
  return InterpreterOutcome{std::move ((values).value ()), {}};
}
}  // namespace detail
}  // namespace mirnext::execution
