# MIRNext

MIRNext is an embeddable register IR, interpreter and lightweight JIT built with
modern C++20. It provides typed construction, checked transformations and owning
execution results. SSA is an internal optimization representation.

Hardware-bound registers have no memory address. Copy their value to an ordinary
local before calling `IRBuilder::address_of`; writes through that address update
the copy.

The MIR core is being migrated to one C++ object model throughout IR, analysis,
optimization, code generation and execution. C2MIR and surrounding tools are
outside this phase. The final target removes the old implementation and the
temporary C reference used for migration tests.

## Current implementation

The replacement libraries live in `include/mirnext` and `lib`:

| Layer | Responsibilities |
| --- | --- |
| IR | Context/module/function ownership, stable basic blocks and instructions, typed references and data-use relationships, IRBuilder/DSL, verification and stack-scope materialization |
| Analysis | CFG, traversal, dominance, loops, liveness and module/function caches with owner-scoped dependency invalidation |
| Transforms | Module/function pass managers, O0–O3 optimization, internal SSA, pass observation and independently owned optimized programs |
| Target | Native target facts, data layout, register information, addressing and ABI rules |
| CodeGen | Owning machine IR, lowering, allocation, spill rewriting, post-allocation optimization, frame formation and compiled artifacts |
| Serialization | Native MIR text and binary readers/writers, independent of the C reader |
| Execution | Loaded programs, symbols, data initialization, interpreter, code publication, typed calls and eager/lazy/BBV JIT |

AArch64 has a complete owning machine-code pipeline connected to the interpreter,
linker and execution session, including eager/lazy JIT and native ABI adapters.
Linux and macOS retain their separate ABI rules.

The x86_64 SysV/Windows port implements instruction selection,
ABI lowering, two-address legalization, allocation, spill rewriting and
post-allocation optimization using the new machine objects. Frame plans preserve
variadic areas and full vector saves, with explicit no-red-zone support and
corrective unaligned vector moves for JRET frames. Owning encoding and relocation
include branch layout, switch tables, constant pools and near-call patching.
The same owning session and IR-built host adapters now connect the x86 pipeline
to eager/lazy JIT and native interpreted callbacks. Immutable x86 lazy gates share
a resolver that preserves integer, XMM and x87 state across cold compilation.
Native x86 phase, byte, ABI and session comparisons require the corresponding CI
hosts and are pending acceptance; arm64 tests do not establish x86 parity.
Windows native calls retain the original single-result restriction; interpreted
calls support multiple results.

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
to whole-function compilation or the old engine. `ExecutionOptions::select_engine`
chooses interpretation, eager JIT, lazy JIT or BBV per definition in one loaded
program. Engines share symbols, global data and typed function handles.
The transitional C++ runtime, compatibility views and duplicate build targets
have been removed. The product uses only the owning layers; the original C
engine is an optional test oracle or a dependency of explicitly enabled tools.
The architecture dependency guard checks the owning layers and rejects
reintroducing the retired transitional sources.

See [the architecture and migration record](docs/cpp-architecture.md) for stage
contracts, behavioral comparisons, known C defects and validation details.
The [native platform acceptance gate](docs/native-acceptance.md) distinguishes
local execution, build-only checks and the CI evidence still needed to retire
the temporary C oracle.
The [IR reference](MIR.md), [target development guide](HOW-TO-PORT-MIR.md)
and [code-memory service guide](CUSTOM-ALLOCATORS.md) describe the C++ interfaces.

## Build

A C++20 compiler and CMake are required. The replacement libraries build without
a C compiler or C reference engine:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMIRNEXT_BUILD_C_REFERENCE=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 8
```

Use a separate build directory with `MIRNEXT_BUILD_C_REFERENCE=ON` to run the
temporary independent C comparisons. These require a C compiler.
The original MIR file corpus also executes through the C++ interpreter and JIT
when the C reference is off.

Useful options:

| Option | Purpose |
| --- | --- |
| `BUILD_TESTING=OFF` | Build without test executables |
| `MIRNEXT_BUILD_EXAMPLES=OFF` | Build without examples |
| `MIRNEXT_BUILD_BENCHMARKS=ON` | Build native architecture microbenchmarks |
| `MIRNEXT_BUILD_UTILITIES=OFF` | Omit the native format converters and binary runner; enabled by default |
| `MIRNEXT_BUILD_C_REFERENCE=ON` | Enable the temporary C test oracle; off by default, requires tests and a C compiler |
| `MIRNEXT_TEST_EXPECTED_TARGET` | Require a native test ABI instead of accepting the detected host target |
| `MIRNEXT_BUILD_TOOLS=ON` | Build the original C2MIR tool; off by default, outside the core migration |

The native `m2b`, `b2m`, `b2ctab` and `mir-bin-run` commands use the same owning
C++ format and execution layers. See [command usage](tools/README.md) for binary
execution, host library loading and embedding a program without the C engine.
The root GNU make entry points also build/test/install these native layers;
see [GNU make usage](INSTALL.md#gnu-make-entry-points). C2MIR remains an explicit
separate build.

## Build and execute IR

This example uses the owning interpreter and a checked function handle. The
handle retains its loaded program after the source Context and session are gone.

```cpp
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <iostream>

mirnext::Result<mirnext::execution::FunctionHandle> build() {
  using namespace mirnext;
  using namespace mirnext::ir;

  Context context;
  auto module = context.create_module("example");
  if (!module) return module.error();
  auto function = (*module)->create_function("add_one", {{Type::i64()}, {Type::i64()}});
  if (!function) return function.error();
  auto block = (*function)->create_block();
  if (!block) return block.error();

  IRBuilder body(**block);
  auto returned = body.ret(body.expression(*(*function)->arguments()[0]) + 1);
  if (!returned) return returned.error();

  auto session = execution::ExecutionSession::create(
      context, {.mode = execution::ExecutionMode::Interpreter});
  if (!session) return session.error();
  return session->find_function("example", "add_one");
}

int main() {
  auto function = build();
  if (!function) { std::cerr << function.error().message << '\n'; return 1; }
  std::array<std::int64_t, 1> arguments{41};
  auto value = function->call_i64(arguments);
  if (!value) { std::cerr << value.error().message << '\n'; return 1; }
  std::cout << *value << '\n';
}
```

For longer Result-returning functions, `MIRNEXT_RESULT_RET(expression)` propagates
the first error and `MIRNEXT_TRY_VALUE(name, expression)` declares an extracted
value. Both evaluate the expression once; extraction moves owning results such
as Context pointers and sessions. Use extraction inside a braced function/block.
The [lazy session example](examples/native-session.cpp) demonstrates these helpers.

On a supported JIT host, select `ExecutionMode::JIT` (the session default).
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
the loaded program rather than the source Context.

## Examples

- [Interpreter and typed C++ imports](examples/native-interpreter.cpp)
- [Eager/lazy execution and persistent module variables](examples/native-session.cpp)
- [Per-function interpretation and JIT in one program](examples/mixed-engines.cpp)
- [Block arguments and calls](examples/native-calls.cpp)
- [Module variables](examples/native-variables.cpp)
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
Contexts. `write_text` and `write_binary` traverse the unified IR and preserve
the existing MIR file protocols. Stack scopes are materialized on a copy when
needed; ordinary read-only traversal does not rewrite the source.

Diagnostic IR/machine dumps are separate from portable text serialization.
Internal SSA, machine operands and runtime compilation state are not written
into the public binary protocol.

## CMake integration

Install the replacement libraries:

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
target_link_libraries(my_jit PRIVATE MIRNext::Execution MIRNext::Transforms)
```

Configure the consumer with `CMAKE_PREFIX_PATH` pointing to the installation.
Available targets include `IR`, `Analysis`, `Transforms`, `Target`, `CodeGen`,
`Serialization` and `Execution` in the `MIRNext::` namespace.
Each layer has a corresponding `Shared` target. Link `MIRNext::ExecutionShared`
to use the interpreter, optimizer and JIT through shared libraries; its transitive
dependencies also use shared libraries. Static and shared targets use the same
compiled objects. CMake rejects mixing the two variants in one dependency graph
because that would duplicate IR and runtime state.

## License

MIRNext is licensed under the [MIT License](LICENSE). Project attribution and
third-party license information are recorded in [NOTICE](NOTICE).
