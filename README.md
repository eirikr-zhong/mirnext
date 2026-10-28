# MIRNext

MIRNext is an embeddable register IR, interpreter and lightweight JIT built with
modern C++20. It provides typed construction, checked transformations and owning
execution results. SSA is an internal optimization representation.

Hardware-bound registers have no memory address. Copy their value to an ordinary
local before calling `MIRBuilder::create_address_of`; writes through that address update
the copy.

IR construction, analysis, optimization, machine code generation and execution
share one owning C++ object model. Supported native targets are Linux and macOS
on x86_64 and aarch64. Windows x86_64 is validated with MSVC in CI.

## Architecture

The library is organized under `include/mirnext` and `lib`:

| Layer | Responsibilities |
| --- | --- |
| IR | Program/module/function ownership, stable basic blocks and instructions, typed references and data-use relationships, explicit MIRBuilder batches, verification and stack-scope materialization |
| Analysis | CFG, traversal, dominance, loops, liveness and module/function caches with owner-scoped dependency invalidation |
| Transforms | Module/function pass managers, O0–O3 optimization, internal SSA, pass observation and independently owned optimized programs |
| Target | Native target facts, data layout, register information, addressing and ABI rules |
| CodeGen | Owning machine IR, lowering, allocation, spill rewriting, post-allocation optimization, frame formation and compiled artifacts |
| Serialization | MIR text and binary readers/writers |
| Execution | Loaded programs, symbols, data initialization, interpreter, code publication, typed calls and eager/lazy/BBV JIT |

AArch64 has a complete owning machine-code pipeline connected to the interpreter,
linker and execution session, including eager/lazy JIT and native ABI adapters.
Linux and macOS retain their separate ABI rules.

The x86_64 backend implements instruction selection,
ABI lowering, two-address legalization, allocation, spill rewriting and
post-allocation optimization using the new machine objects. Frame plans preserve
variadic areas and full vector saves, with explicit no-red-zone support and
corrective unaligned vector moves for JRET frames. Owning encoding and relocation
include branch layout, switch tables, constant pools and near-call patching.
The same owning session and IR-built host adapters now connect the x86 pipeline
to eager/lazy JIT and native interpreted callbacks. Immutable x86 lazy gates share
a resolver that preserves integer, XMM and x87 state across cold compilation.

BBV now has owning machine preparation, stable block/version identities,
property specialization and independently owned block bytes with typed successor
relocations, including GVN memory identities and alias rules. Immutable cold/warm
block entries preserve live registers, flags, floating state and dynamic stacks.
An owning block runtime now selects, compiles and publishes reached versions in
a bounded code arena; later batches use separate pages and retain stable gates.
`ExecutionSession` supports `JITCompilation::BlockVersioning`: ordinary function
entries install native recovery checkpoints, label/data references retain stable
block gates, and expression initializers use the same typed calls and symbol
image. Failed cold compilation reports diagnostics and leaves entries retryable,
including raw variadic entries and nested host callbacks. Block observers and
per-function statistics expose demand-driven compilation; there is no fallback
to whole-function compilation. `ExecutionOptions::select_engine`
chooses interpretation, eager JIT, lazy JIT or BBV per definition in one loaded
program. Engines share symbols, global data and typed function handles.
Public examples cover
[custom code memory](examples/custom-code-memory.cpp),
[machine IR](examples/native-machine-ir.cpp) and
[compilation stages](examples/native-compile.cpp).

## Build

A C++20 compiler and CMake 3.14 or newer are required. Script integration checks
are enabled with tests by default and require Python 3.8 or newer. Optional
platform acceptance auditing also requires CTest 3.21 or newer for JUnit output:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 8
```

Compilation uses two jobs; test parallelism is independent. MIR language fixtures
in `tests/Fixtures` run through the C++ interpreter and JIT.

Useful options:

| Option | Purpose |
| --- | --- |
| `BUILD_SHARED_LIBS=ON` | Build a shared library; defaults to a static library |
| `BUILD_TESTING=OFF` | Build without test executables |
| `MIRNEXT_BUILD_SCRIPT_TESTS=OFF` | Omit Python integration checks; C++ tests remain available without Python |
| `MIRNEXT_BUILD_EXAMPLES=OFF` | Build without examples |
| `MIRNEXT_BUILD_BENCHMARKS=ON` | Build native architecture microbenchmarks |
| `MIRNEXT_BUILD_UTILITIES=OFF` | Omit the native format converters and binary runner; enabled by default |
| `MIRNEXT_ENABLE_ASAN=ON` | Compile and link with AddressSanitizer; disabled by default |
| `MIRNEXT_ENABLE_NATIVE_EVIDENCE=ON` | Build source-stamped acceptance tests and register audit checks; disabled by default, enabled in CI |
| `MIRNEXT_TEST_EXPECTED_TARGET` | Require a native test ABI instead of accepting the detected host target |

The native `m2b`, `b2m`, `b2cpp` and `mir-bin-run` commands use the same owning
C++ format and execution layers. See [command usage](tools/README.md) for binary
execution, host library loading and embedding a program.

For a library/commands build without tests or examples, configure
with `-DBUILD_TESTING=OFF -DMIRNEXT_BUILD_EXAMPLES=OFF`.

Select build features through `-D` options without editing build files. For example,
to build tests with AddressSanitizer:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DMIRNEXT_BUILD_EXAMPLES=OFF -DMIRNEXT_ENABLE_ASAN=ON
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure --parallel 4
```

`CMAKE_BUILD_TYPE` and the standard CMake compiler/toolchain and installation
variables select the configuration, compiler, toolchain and install prefix.
Optimization and execution behavior are selected through the C++ API.
CMake declares build targets, dependencies, compiler options, installation and
test commands. Source auditing, report validation and command behavior checks
are implemented in standalone Python scripts. Native evidence requires script
tests to be enabled. After building with `MIRNEXT_ENABLE_NATIVE_EVIDENCE=ON`, run
its audit independently (use the actual build configuration):

```sh
ctest --test-dir build --output-on-failure --parallel 8 --output-junit build/native-tests.xml
python3 tests/Platform/AuditAcceptance.py --source . --platform native \
  --configuration Release --linkage static --output build/native-evidence-Release.json build
```

Use `--linkage shared` for a shared build. To audit both linkages together, omit
`--linkage` and pass both build directories.

## Build and execute IR

`Program` owns modules, functions, blocks and instructions. `MIRBuilder` selects an
insertion point and creates instructions through explicit methods, in the style
of LLVM's IRBuilder. Values have no arithmetic or comparison operators. Results
use named `has_value()`, `value()` and `error()` accessors.

```cpp
#include <mirnext/Execution/Session.hpp>
#include <iostream>

int main() {
  using namespace mirnext;
  using namespace mirnext::ir;

  Program program;
  MIRBuilder builder(program);
  auto* module = builder.create_module("example");
  builder.create_function<std::int64_t(std::int64_t)>(module, "add_one");
  auto sum = builder.create_add(builder.argument(0), builder.constant_i64(1));
  builder.create_return(sum);

  auto built = builder.finish();
  if (!built.has_value()) {
    std::cerr << built.error().message << '\n';
    return 1;
  }
  auto session = execution::ExecutionSession::create(std::move(program));
  if (!session.has_value()) {
    std::cerr << session.error().message << '\n';
    return 1;
  }
  auto result = session.value().call_i64("example", "add_one", {41});
  if (!result.has_value()) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  std::cout << result.value() << '\n';
}
```

`create_function<Return(Arguments...)>(module, name)` maps C++ scalar types to MIR
types and selects an entry block automatically. Explicit `Signature` values cover
block arguments, multiple results, variadic and special calling conventions.
`create_block()` creates a layout block; `set_insert_point(block)` selects it.
Creating another function changes the selected function; `set_function(function)`
selects an existing function for editing.

| Operation | Explicit construction API |
| --- | --- |
| Parameters, constants and locals | `argument(index)`, `constant_i64(value)`, `create_register(type)` |
| Arithmetic, comparison and conversion | `create_add(a, b)`, `create_lt(a, b)`, `create_convert(value, type)` |
| Direct and indirect calls | `create_call(callee, {args...})`, `create_indirect_call(signature, address, {args...})` |
| Call results | `call.value()` requires exactly one result; `call.result(index)` selects a result |
| Variables and memory | `create_load(variable)`, `create_store(variable, value)`, `memory(type, address, offset)` |
| Control flow | `create_block(name)`, `set_insert_point(block)`, `create_cond_branch(condition, target)`, `create_jump(target)` |
| Block ABI arguments | `block_argument(address, Type::block(size, classification))`, `return_buffer(address, size)` |
| Precise instructions | `create_instruction(opcode, {operands...})` uses the same validator |

Methods above are members of `MIRBuilder`. Creation returns ordinary borrowed node
pointers or typed `Value` objects. Required owner relationships return references;
collections yield objects directly. There is one canonical instruction body.
Moving a `Program` preserves node identity. Nodes and values must not outlive
their program; deleting a node invalidates references to it. A builder can move
without invalidating its insertion state or values. Replacing a call invalidates
its `CallResult` view.

The builder records the first error and stops subsequent changes. `finish()` checks
the batch and closes it before analysis, optimization, serialization or execution.
Successful changes before a failure remain applied; unfinished or failed batches
prevent later stages. An invalid result is safely propagated by subsequent builder
calls. Inspect `status()` or `error()` for an early diagnostic.

`create_import`, `declare_function` and `create_prototype` create symbols without
a body; `define_function` defines a declaration with the same symbol identity.
`export_symbol` connects modules. Module data accepts typed `DataInitializer`
values: `InitializedData`, `ZeroData`, `SymbolReference`, `LabelReference` and
`ExpressionData`. `create_anonymous_data` preserves contiguous layout; declarations
and later definitions share symbol identity. See [block calls](examples/native-calls.cpp),
[cross-module calls](examples/native-cross-module.cpp) and
[shared variables](examples/native-variables.cpp).

Parsing, cloning and materialization return owning `Result<Program>` values.
Fallible borrowed results use `Result<T&>`; optional queries use `OptionalRef<T>`.
Both use `has_value()` and `value()` and never extend the referent's lifetime.
`OptionalRef::get()` exposes a nullable borrowed pointer when needed. Neither
wrapper provides dereference, arrow or implicit Boolean conversion operators.
Structural edits invalidate collection views; obtain a fresh view after editing.

On a supported JIT host, `ExecutionMode::JIT` is the session default.
Select `ExecutionMode::Interpreter` to interpret instead.
Use `LinkOptions::compilation = JITCompilation::Lazy` for first-call compilation.
`FunctionHandle::compile()` prepares a lazy body explicitly.
Select `JITCompilation::BlockVersioning` for demand-driven block compilation;
in this mode `compile()` prepares only the call-entry block. Set
`LinkOptions::block_versioning` for the per-function code budget and block
observer, and use `FunctionHandle::block_statistics()` to inspect publication.

For mixed execution, supply a fallible `select_engine(const ir::Function &)`
callback returning `ExecutionEngine::{Interpreter,EagerJIT,LazyJIT,BlockVersioning}`.
It runs once per owned optimized definition, before linking and code publication;
aliases share the definition's choice. Optimization precedes engine selection, so
inlined code uses its caller's engine. Mixed sessions automatically publish native
ABI entries for interpreted definitions. Ordinary calls can cross engines with
scalar, block and variadic arguments; `JCALL/JRET` continuations must stay within
the interpreted domain or the native domain. Cross-domain jump calls return a
diagnostic. `FunctionHandle::engine()` reports a definition's selected engine.
See [the mixed execution example](examples/mixed-engines.cpp).

Construction and execution report diagnostics through `Result`, including in
no-exceptions builds. Optimization returns an independent `OptimizedProgram`;
source IR is retained unchanged. Runtime data and compilation caches belong to
the loaded program rather than the source Program.


## Editing, analyses and compilation stages

Read-only traversal yields objects directly:

```cpp
for (const auto &module : program.modules())
  for (const auto &function : module.functions())
    for (const auto &block : function.blocks())
      for (const auto &instruction : block.instructions()) {
        // Inspect instruction.opcode() and instruction.operands().
      }
```

Required owner relationships, such as `instruction.block()`, return references.
Optional relationships and tagged operand queries return `OptionalRef`, for example
`operand.get<Register>()`. A missing optional value is distinct from a failed
`Result`. Both wrappers borrow the object and require a presence check before
access through `has_value()` and `value()`.

An `MIRBuilder` can borrow an existing `Program`, `Function` or `BasicBlock` to
edit it using the same explicit creation methods. `replace_instruction`,
`replace_operand`, `erase_instruction`, `split_before` and `merge_fallthrough`
maintain the existing validation and invalidation rules. `ssa()` supplies a checked
`SSARewriter` for internal SSA extensions. `MachineEditor` provides the machine
mutation batch. Checked SSA/machine handles detect deletion while their editing
state remains alive; they do not extend its lifetime. Analysis references become
invalid when the cache is invalidated, and SSA references on SSA destruction.

`FunctionAnalysisManager` and `ModuleAnalysisManager` are value objects sharing
their cache state when copied. `get<Analysis>()` returns
`Result<const Analysis::Result&>`; `get_cached<Analysis>()` returns `OptionalRef`.
Custom function passes receive an `MIRBuilder`; module passes receive the module
and a context containing the builder. Pass managers check
each batch before advancing, apply `PreservedAnalyses`, and propagate observer
failures. The optimization pipeline and its O0–O3 decisions are unchanged.

`MachineCompilation::create(function, target, options)` returns an owning value
for both incremental and complete compilation. `step()` and `run_until(stage)`
advance that same state, `graph()` and other analysis queries expose optional
read-only views, and `finish()` returns an owning `CompiledFunction`. Use `edit()`
before lowering to obtain a machine editing batch. The compiler retains no source
IR; a prepared or compiled result can outlive the source program. The
[stage example](examples/native-machine-ir.cpp) demonstrates this lifetime and
[the compilation example](examples/native-compile.cpp) demonstrates observation
and code publication. Stage observers borrow views only for the callback duration.

`ExecutionSession` is the loading and execution entry point. Moving a `Program`
or an `OptimizedProgram` into it transfers ownership; passing a borrowed program
preserves it. Function and data handles retain the loaded program independently
of the session. Native entries, executable memory and host callbacks keep explicit
ABI address types. All other object access follows the same reference rules.

## Examples

- [Interpreter and typed C++ imports](examples/native-interpreter.cpp)
- [Eager/lazy execution and persistent module variables](examples/native-session.cpp)
- [Per-function interpretation and JIT in one program](examples/mixed-engines.cpp)
- [Block arguments and calls](examples/native-calls.cpp)
- [Module variables](examples/native-variables.cpp)
- [Cross-module function calls](examples/native-cross-module.cpp)
- [Stack scopes](examples/native-stack-scope.cpp)
- [Block and multiple-result execution](examples/execution-values.cpp)
- [Interpreted native callbacks](examples/interpreted-callback.cpp)
- [Machine stages and dumps](examples/native-machine-ir.cpp)
- [Compilation, observation and code publication](examples/native-compile.cpp)
- [Custom code-memory service and mapping lifetime](examples/custom-code-memory.cpp)
- [BBV preparation, property specialization and block encoding diagnostics](examples/native-bbv.cpp)
- [Demand-driven BBV execution and block observation](examples/bbv-session.cpp)

The target-specific examples explicitly report unsupported hosts.

## Formats and diagnostics

`serialization::parse_text` and `read_binary` return independently owned
Programs. `write_text` and `write_binary` traverse the unified IR and preserve
the existing MIR file protocols. Stack scopes are materialized on a copy when
needed; ordinary read-only traversal does not rewrite the source.

Diagnostic IR/machine dumps are separate from portable text serialization.
Internal SSA, machine operands and runtime compilation state are not written
into the public binary protocol.

## CMake integration

Install the library:

```sh
cmake --install build --prefix /your/install/prefix
```

A consumer uses CXX-only CMake and the installed C++ headers:

```cmake
cmake_minimum_required(VERSION 3.14)
project(MyJIT LANGUAGES CXX)
find_package(MIRNext CONFIG REQUIRED)
add_executable(my_jit main.cpp)
target_compile_features(my_jit PRIVATE cxx_std_20)
target_link_libraries(my_jit PRIVATE MIRNext::MIRNext)
```

Configure the consumer with `CMAKE_PREFIX_PATH` pointing to the installation.
The single `MIRNext::MIRNext` target provides IR construction, optimization,
serialization, interpretation and JIT execution. Configure MIRNext with
`-DBUILD_SHARED_LIBS=ON` for a shared library, or `OFF` (the default) for a static
library. To build and test both, use separate build and installation directories.

## License

MIRNext is licensed under the [MIT License](LICENSE), retaining the original MIR
copyright and adding attribution for MIRNext contributors. Project attribution
is recorded in [NOTICE](NOTICE). Both files are included in installations under
`share/licenses/MIRNext` with the default CMake data directory.
