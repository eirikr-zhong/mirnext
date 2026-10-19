/* Licensed under the MIT License. Derived interpreter cache and invocation frames. */
#pragma once
#include "mirnext/Execution/Interpreter.hpp"
#include "mirnext/Execution/detail/ProgramImage.hpp"
#include "mirnext/Execution/detail/Values.hpp"
#include "mirnext/Execution/detail/NativeCalls.hpp"
#include "mirnext/Execution/detail/Hardware.hpp"
#include <array>
#include <cstring>
#include <unordered_set>
namespace mirnext::execution::detail {
constexpr std::size_t no_slot = SIZE_MAX;
using InterpreterSlot = HardwareSlot;
template <class T>
T read_slot (const InterpreterSlot &slot) {
  T value;
  std::memcpy (&value, slot.bytes.data (), sizeof value);
  return value;
}
template <class T>
InterpreterSlot slot (T value) {
  InterpreterSlot s;
  std::memcpy (s.bytes.data (), &value, sizeof value);
  return s;
}
struct OverflowFlags {
  bool signed_value = false, unsigned_value = false;
};
Result<std::optional<ScalarValue>> evaluate_scalar (ir::Opcode, std::span<const ScalarValue>,
                                                    OverflowFlags &);
Result<ScalarValue> load_scalar (ir::Type, std::uintptr_t);
Result<void> store_scalar (ir::Type, std::uintptr_t, const ScalarValue &);
struct PreparedOperand {
  const ir::Operand *source;
  std::size_t reg = no_slot, base = no_slot, index = no_slot, binding = no_slot, pc = no_slot;
};
struct PreparedOperation {
  const ir::Instruction *source;
  const ir::InstructionDescriptor *descriptor;
  std::vector<PreparedOperand> operands;
  std::uint8_t scalar_inputs = 0;
};
struct InterpreterBody {
  const ir::Function *source;
  std::vector<PreparedOperation> operations;
  std::vector<const ir::Register *> registers;
  std::vector<std::size_t> hardware, arguments;
  std::unordered_map<const ir::Label *, std::size_t> labels;
};
// Segmented bump stack. Saved marks are real aligned cursor addresses; no host
// stack pointer, longjmp, or compiler-specific dynamic alloca is involved.
struct InterpreterStack {
  struct Segment {
    std::unique_ptr<std::byte[]> data;
    std::size_t size, cursor;
  };
  struct Mark {
    std::size_t segments, cursor;
  };
  std::vector<Segment> segments;
  std::unordered_map<std::uintptr_t, Mark> marks;
  Result<std::uintptr_t> allocate (std::size_t);
  Result<std::uintptr_t> save ();
  Result<void> restore (std::uintptr_t);
};
struct InterpreterOutcome {
  std::vector<ScalarValue> values;
  std::optional<std::uintptr_t> jump;
};
struct LoadedProgram;
struct InterpreterInvocation {
  LoadedProgram &state;
  std::size_t steps = 0, depth = 0;
  std::vector<std::span<const std::byte>> variadic_areas;
  std::vector<std::span<const std::byte>> native_variadics;
  bool native_variadic_cursor (std::span<const std::byte>) const;
  Result<InterpreterOutcome> invoke (std::size_t, std::span<const ExecutionArgument>,
                                     bool jump = false, std::span<const std::byte> native_va = {});
  Result<InterpreterOutcome> invoke_foreign (std::uintptr_t, const ir::Signature &,
                                             std::span<const ExecutionArgument>);
  Result<std::vector<ScalarValue>> call_native (std::uintptr_t, const ir::Signature &,
                                                std::span<const ExecutionArgument>,
                                                bool native_definition = false);
};
// Reentry retains the invocation's budgets and hardware state. No call state
// is stored in the source IR or in a context-wide generator field.
class InvocationActivation {
 public:
  explicit InvocationActivation (InterpreterInvocation &);
  ~InvocationActivation ();
  static InterpreterInvocation *find (LoadedProgram &);

 private:
  InterpreterInvocation &invocation_;
  InvocationActivation *previous_;
  static thread_local InvocationActivation *current_;
};
struct InterpreterFrame {
  InterpreterInvocation &invocation;
  InterpreterBody &body;
  std::vector<InterpreterSlot> locals;
  std::vector<InterpreterSlot *> registers;
  InterpreterStack stack;
  std::vector<ExecutionArgument> arguments;
  std::vector<std::byte> variadics;
  std::size_t region_depth, native_depth;
  std::span<const std::byte> native_va;
  OverflowFlags overflow;
  InterpreterFrame (InterpreterInvocation &, InterpreterBody &);
  ~InterpreterFrame ();
  Result<void> initialize (std::span<const ExecutionArgument>, std::span<const std::byte> = {});
  Result<ScalarValue> read (const PreparedOperand &, ir::Type);
  Result<void> write (const PreparedOperand &, const ScalarValue &);
  Result<std::uintptr_t> address (const PreparedOperand &) const;
  Result<InterpreterOutcome> run ();
  Result<void> start_variadics (std::uintptr_t);
};
}  // namespace mirnext::execution::detail
