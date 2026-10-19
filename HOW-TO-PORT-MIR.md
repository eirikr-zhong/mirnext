# Developing a MIRNext target

Targets operate on owning C++ IR and machine objects. Start with the
[architecture overview](docs/cpp-architecture.md),
[IR reference](MIR.md) and the
[stage-by-stage executable](examples/native-machine-ir.cpp).
MIRNext currently selects the native x86_64 or aarch64 target; cross-target
compilation is not a product entry point. Adding a target requires explicit
support throughout lowering, encoding and runtime boundaries. There is no
placeholder backend that silently accepts instructions.

## Target facts and ABI

`target::TargetMachine` supplies architecture and ABI identity, physical
registers, register banks, reserved registers, call clobbers, scalar layouts and
opcode normalization. `PhysicalRegister` carries its architecture; virtual
registers and stack slots have separate C++ types. Do not distinguish them by
number ranges or use source IR objects as runtime state.

Begin with [TargetMachine.hpp](include/mirnext/Target/TargetMachine.hpp) and
[its implementation](lib/Target/TargetMachine.cpp). Update native target
selection and `AddressingRules` deliberately; a new enum value alone does not
make compilation available. Preserve ABI-specific behavior rather than selecting
rules only by CPU name. Existing differences include x86_64 SysV/Windows and
aarch64 Linux/macOS block passing, variadic areas and long-double representation.

## Owning machine preparation and lowering

`prepare_machine_ir` creates a `MachineFunction` that owns its blocks,
instructions, virtual registers, stack slots, labels, symbols and memory
identities. It retains no source Context nodes. Use `MachineRewriter` for checked
mutation. Its revision changes invalidate analyses that borrow those nodes.
Keep leaf status, dynamic allocation, frame requirements and ABI save areas in
`MachineFunctionState`; state belongs to this compilation, not a Context cache.

`machinize` performs ABI and target-specific lowering; `legalize_operands`
handles ordered constraints, reloads, two-address operands and memory forms.
Implement all public operations or return a contextual `Result` diagnostic.
Instruction matching must use the target's own ordered patterns; middle-end
addressing checks do not establish machine encoding legality.

| Concern | Current C++ implementation |
| --- | --- |
| Preparation and source correspondence | [Prepare.cpp](lib/CodeGen/Prepare.cpp) |
| ABI lowering | [AArch64Lowering.cpp](lib/CodeGen/AArch64Lowering.cpp), [X86Lowering.cpp](lib/CodeGen/X86Lowering.cpp) |
| x86 ABI layout | [X86ABI.hpp](include/mirnext/Target/X86ABI.hpp), [X86ABI.cpp](lib/Target/X86ABI.cpp) |
| Ordered target patterns | [AArch64Patterns.def](include/mirnext/Target/AArch64Patterns.def), [X86Patterns.def](include/mirnext/Target/X86Patterns.def) |
| Pattern interpretation | [AArch64Patterns.cpp](lib/CodeGen/AArch64Patterns.cpp), [X86Patterns.cpp](lib/CodeGen/X86Patterns.cpp), [InstructionPatterns.cpp](lib/CodeGen/InstructionPatterns.cpp) |
| Operand legalization | [LegalizeOperands.cpp](lib/CodeGen/LegalizeOperands.cpp) |

Lowering must cover parameters/results, direct and indirect calls, jump calls,
all block classes, implicit return buffers, varargs, stack operations, fixed
register constraints, integer division/shifts, floating comparisons and builtins.
Builtin references remain typed until runtime resolution. Public file opcodes
remain independent of private machine instructions and operand representations.

## Allocation, frames and compiled results

The canonical [compilation pipeline](lib/CodeGen/Compilation.cpp) owns all mutable
state of one invocation. It runs preparation, machinize, legalization, coalescing,
liveness, live ranges, assignment, rewriting, edge spills, post-allocation
optimization, frame formation and encoding. Preserve the optimization-level
conditions and stage order when adding target behavior.

Target-specific register effects and early clobbers participate in
[Liveness.cpp](lib/CodeGen/Liveness.cpp). Stack addressing and reload rules live in
[SpillEmitter.cpp](lib/CodeGen/SpillEmitter.cpp) and
[RegisterRewrite.cpp](lib/CodeGen/RegisterRewrite.cpp). Frame generation is in
[Frame.cpp](lib/CodeGen/Frame.cpp) and [X86Frame.cpp](lib/CodeGen/X86Frame.cpp).
Review generic stages for architecture dispatch as well as adding lowering code.

Encoding produces an owning `CompiledFunction`: code bytes, constant pools,
relocations and label offsets. `CompiledBlock` owns BBV bytes and typed successor
relocations. Neither artifact may retain instruction pointers from a destroyed
machine function. Symbol addresses remain unresolved until relocation; preserve
physical register identity, constant bits, stack offsets and instruction order.
See [AArch64Encoding.cpp](lib/CodeGen/AArch64Encoding.cpp),
[X86Encoding.cpp](lib/CodeGen/X86Encoding.cpp) and the corresponding encoding
recipe/dispatch files.

`compile_function` consumes verified, materialized non-SSA public IR, normally
from `transforms::optimize`. It neither optimizes the input again nor publishes
executable memory. `prepare_machine_function` stops after frame formation and
can preserve property instructions for BBV specialization.

## Execution boundaries

The interpreter, eager/lazy JIT and BBV share an `ExecutionSession` symbol image.
Runtime addresses, global data and compilation caches belong to loaded programs.
Implement native ABI entry/call adapters, lazy resolution, BBV cold/warm gates,
invocation recovery and host-register preservation as part of target support.
These are not optional substitutes for whole-function code generation.

The target entry declarations are under `include/mirnext/CodeGen`, including
[RuntimeEntry.hpp](include/mirnext/CodeGen/RuntimeEntry.hpp),
[BlockEntry.hpp](include/mirnext/CodeGen/BlockEntry.hpp),
[InvocationEntry.hpp](include/mirnext/CodeGen/InvocationEntry.hpp) and
[NativeBoundary.hpp](include/mirnext/CodeGen/NativeBoundary.hpp).
Variadic runtime operations and builtin resolution use the C++ services in
[Builtins.hpp](include/mirnext/Execution/Builtins.hpp).

Executable mapping, protection and cache maintenance belong to
`execution::CodeMemoryProvider`. Implement OS behavior there; do not put mapping
ownership into target instruction nodes. The
[code-memory guide](CUSTOM-ALLOCATORS.md) describes its lifetime and publication
contract. Source IR remains usable after execution or compilation.

## Validation and observation

`CompilationOptions::observer` returns `Result<void>` and sees a
`CompilationSnapshot` containing stage, read-only machine view, instruction count
and dump; the encode stage also exposes the artifact. Views are valid only during
the callback. Callback failure must propagate with module, function, target and
stage information. See [native compilation](examples/native-compile.cpp).

Exercise every lowering/pattern branch with positive, forbidden and boundary
cases. Compare machine IR, graph edges, call registers, function state, frames,
bytes and relocations against the temporary independent reference where available.
Run the same input through original/optimized interpretation and eager/lazy/BBV
execution at O0–O3, including ABI blocks, varargs, hardware binding, stack restore,
label data, external callbacks, failure recovery and independent lifetimes.

The [native acceptance guide](docs/native-acceptance.md) records host/ABI admission
and the platform matrix. A compile/link check cannot establish native execution
parity. Compilation uses two jobs, normal local CTest eight and ASan four; no
Rosetta execution is used. CI must retain architecture, ABI and complete test
results before a platform is accepted. C2MIR is outside the MIR core migration.
