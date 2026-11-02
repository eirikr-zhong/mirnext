# MIRNext

MIRNext is an embeddable register IR, interpreter and lightweight JIT written in
C++20. Build typed IR with an explicit `MIRBuilder`, optimize it, and execute it
through one `ExecutionSession`. SSA is internal to the optimizer.

Native targets: **x86_64 on Linux, macOS and Windows; AArch64 on Linux and macOS.**
The library supports builds without exceptions or RTTI.

## Quick start

```cpp
#include <mirnext/Execution/Session.hpp>
#include <iostream>

int main() {
  using namespace mirnext;
  using namespace mirnext::ir;

  Program program;
  MIRBuilder builder(program);
  auto module = builder.create_module("example");
  builder.create_function<std::int64_t(std::int64_t)>(module, "add_one");
  builder.create_return(
      builder.create_add(builder.argument(0), builder.constant_i64(1)));

  auto built = builder.finish();
  if (!built) {
    std::cerr << built.error().message << '\n';
    return 1;
  }
  auto session = execution::ExecutionSession::create(std::move(program));
  if (!session) {
    std::cerr << session.error().message << '\n';
    return 1;
  }
  auto result = session->call_i64("example", "add_one", {41});
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
  std::cout << *result << '\n'; // 42
}
```

`Program` owns the IR; moving it preserves node identity. Builder values and node
references borrow that storage. The builder records the first error and stops
further changes; call `finish()` before using the program. Stage boundaries and
execution return `Result`: `if (result)` checks success, `*result` accesses the value,
`result->method()` accesses its members, and `result.error()` retrieves a failure.

## Capabilities

- **IR:** typed registers, basic blocks, calls, block ABI arguments, module data,
  symbols and cross-module references; MIR text and binary serialization.
- **Optimization:** O0–O3, inlining, SSA, GVN, copy propagation, dead code/store
  elimination, loop-invariant code motion and target-aware combines. Custom
  passes, analysis caching and stage observers are available.
- **Execution:** interpreter, eager/lazy JIT, basic-block versioning (BBV), and
  per-function engine selection in one session. JIT defaults to O2.
- **Code generation:** native C++ lowering, register allocation, spill handling,
  encoding, relocation and executable-memory management.
- **SIMD128:** signed/unsigned integer lanes plus f32x4/f64x2, covering the standard
  SIMD128 operations from WebAssembly Core 2.0 through NEON/SSE2 instructions or
  register sequences. Includes arithmetic, comparisons, saturation, conversions,
  shuffle/swizzle and narrow memory operations. No Wasm loader or 256-bit types.

SLP vectorization is opt-in at O2/O3:

```cpp
mirnext::transforms::OptimizationOptions options;
options.vectorization.slp = true;
auto optimized = mirnext::transforms::optimize(program, options);
```

SLP packs independent scalar operations within a basic block and checks memory
dependencies and target costs. It preserves floating-point operation order;
loop-iteration widening and reductions are not implemented. Vector cleanup can
also be enabled separately with `options.vectorization.combine = true`.

## Build

Requires a C++20 compiler and CMake 3.14+. Default script tests also require
Python 3.8+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 4
```

For multi-configuration generators such as Visual Studio, add `--config Release`
to the build command and `-C Release` to CTest.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared library |
| `BUILD_TESTING` | `ON` | Build and register tests |
| `MIRNEXT_BUILD_EXAMPLES` | `ON` | Build API examples |
| `MIRNEXT_BUILD_UTILITIES` | `ON` | Build format converters and binary runner |
| `MIRNEXT_BUILD_SCRIPT_TESTS` | `ON` | Run Python integration checks |
| `MIRNEXT_BUILD_BENCHMARKS` | `OFF` | Build native microbenchmarks |
| `MIRNEXT_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer |
| `MIRNEXT_ENABLE_NATIVE_EVIDENCE` | `OFF` | Enable platform acceptance auditing |

## Use in your project

Install with `cmake --install build --prefix <prefix>`, then configure your
project with `CMAKE_PREFIX_PATH` pointing to that prefix:

```cmake
cmake_minimum_required(VERSION 3.14)
project(MyJIT LANGUAGES CXX)
find_package(MIRNext CONFIG REQUIRED)
add_executable(my_jit main.cpp)
target_link_libraries(my_jit PRIVATE MIRNext::MIRNext)
```

The single `MIRNext::MIRNext` target supplies the library and its C++20 requirement.

## Examples and reference

| Topic | Example |
| --- | --- |
| Interpreter and host imports | [native-interpreter.cpp](examples/native-interpreter.cpp) |
| Eager/lazy JIT and module variables | [native-session.cpp](examples/native-session.cpp) |
| Mixed execution engines | [mixed-engines.cpp](examples/mixed-engines.cpp) |
| Block arguments and multiple results | [native-calls.cpp](examples/native-calls.cpp), [execution-values.cpp](examples/execution-values.cpp) |
| Cross-module calls | [native-cross-module.cpp](examples/native-cross-module.cpp) |
| SIMD and automatic packing | [native-simd.cpp](examples/native-simd.cpp), [native-vectorization.cpp](examples/native-vectorization.cpp) |
| Custom passes and analyses | [native-simplify.cpp](examples/native-simplify.cpp), [native-analysis.cpp](examples/native-analysis.cpp) |
| Machine stages and code publication | [native-machine-ir.cpp](examples/native-machine-ir.cpp), [native-compile.cpp](examples/native-compile.cpp) |
| BBV and code memory | [bbv-session.cpp](examples/bbv-session.cpp), [custom-code-memory.cpp](examples/custom-code-memory.cpp) |

Public headers are under [include/mirnext](include/mirnext); native tools have
[command usage](tools/README.md). Benchmarks live in [benchmarks](benchmarks),
including SIMD lowering and scalar-to-vector comparisons. Timing results depend
on the target and workload. SIMD fixture provenance is in [tests/SIMD/SOURCE](tests/SIMD/SOURCE).

## License

[MIT](LICENSE). Original MIR and MIRNext attribution is retained in [NOTICE](NOTICE).
