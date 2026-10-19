# Building MIRNext

Use CMake 3.14 or newer and a C++20 compiler. The installed libraries expose the
unified C++ API described in [README.md](README.md).

## Build and test

Tests require Python 3.8 or newer for test-only artifact validation; product and
installed consumer builds do not depend on Python. Native evidence auditing
uses CTest 3.21 or newer to write JUnit (`--output-junit`).

Build the replacement libraries and native tests without a C compiler:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DMIRNEXT_BUILD_C_REFERENCE=OFF
cmake --build build-debug --parallel 2
ctest --test-dir build-debug --output-on-failure --parallel 8
```

For optimized builds, use a separate directory and `CMAKE_BUILD_TYPE=Release`.
Compilation remains limited to two jobs. CTest runs independent tests in parallel;
adjust its job count for the host CPU and memory. CI uses four test jobs, and the
native eight-core validation host uses eight (four for AddressSanitizer).
With Visual Studio or another multi-configuration generator, add `--config Debug`
to the build and `-C Debug` to CTest, or select Release in both commands.

The temporary independent C comparisons require a C compiler. Enable them in a
separate build with `MIRNEXT_BUILD_C_REFERENCE=ON`. This option requires
`BUILD_TESTING=ON`. The transitional runtime and its build targets have been
removed; applications consume the installed `MIRNext::` targets.

Native format converters and the binary runner build and install by default
without enabling C. Set `MIRNEXT_BUILD_UTILITIES=OFF` for libraries only.
See [native command usage](tools/README.md) for stdin/stdout conversion,
library resolution, engine selection and the `MIRNEXT_EMBEDDED_HEADER` option.

The owning session provides eager/lazy JIT and native ABI adapters on AArch64.
The x86_64 port now connects encoding, relocation, the same IR-built adapters,
native interpreted callbacks and immutable lazy entries to the owning session.
Native x86 ABI and execution acceptance requires the corresponding CI hosts and
has not been established by local arm64 validation. Windows native calls retain
the original single-result restriction. The owning session also provides BBV
and per-definition engine selection through `ExecutionOptions::select_engine`.
Static/shared consumer tests exercise mixed execution as well as uniform sessions.
See the [architecture record](docs/cpp-architecture.md)
for platform-specific validation and remaining work.

## Build without tests or examples

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DMIRNEXT_BUILD_C_REFERENCE=OFF -DBUILD_TESTING=OFF -DMIRNEXT_BUILD_EXAMPLES=OFF
cmake --build build-release --config Release --parallel 2
```

## AddressSanitizer

For supported Clang configurations:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMIRNEXT_BUILD_C_REFERENCE=OFF -DMIRNEXT_BUILD_EXAMPLES=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure --parallel 4
```

The macOS validation uses `detect_leaks=0`; memory-access checks remain enabled.
Build and run natively on Apple arm64. Rosetta is not part of validation.

## Install and consume

```sh
cmake --install build-release --config Release --prefix /your/install/prefix
```

Configure the consumer with `CMAKE_PREFIX_PATH=/your/install/prefix` and use:

```cmake
cmake_minimum_required(VERSION 3.14)
project(MyJIT LANGUAGES CXX)
find_package(MIRNext CONFIG REQUIRED)
add_executable(my_jit main.cpp)
target_compile_features(my_jit PRIVATE cxx_std_20)
target_link_libraries(my_jit PRIVATE MIRNext::Execution MIRNext::Transforms)
```

The package exports `IR`, `Analysis`, `Transforms`, `Target`, `CodeGen`,
`Serialization` and `Execution`, and a `Shared` variant of each, in the
`MIRNext::` namespace. Headers are installed under `include/mirnext`.
For shared execution, link only `MIRNext::ExecutionShared`; CMake supplies the
corresponding shared dependencies. Choose one linkage variant for the entire
dependency graph. Mixing static and shared MIRNext layers fails during CMake
generation instead of duplicating object identities or thread-local runtime state.
Installed shared libraries locate adjacent dependencies relative to themselves
on macOS and ELF systems. Windows applications must deploy the installed DLLs
beside the executable or add the installation's `bin` directory to `PATH`.
The external consumer checks in [tests/Consumer](tests/Consumer) exercise static
and shared execution with a CXX-only project, disabled exceptions and disabled RTTI.
Both run the optimizer, complete machine pipeline, interpreter, eager/lazy JIT,
BBV and native callbacks. A separate configuration check rejects mixed linkage.
The [subproject consumer](cmake/consumer-smoke) uses the same native layers via
`add_subdirectory`.

## Examples and benchmarks

Runnable new-model examples are linked from [README.md](README.md).
`MIRNEXT_BUILD_EXAMPLES` controls their build.
`MIRNEXT_BUILD_BENCHMARKS=ON` enables native architecture microbenchmarks.
`mirnext_format_io_benchmark --functions 32` compares owning API construction,
text parsing and binary reading for identical sieve IR, validates canonical
roundtrips, and separately measures private temporary-stream I/O. Without an
argument it uses the original 100,000-function workload. Independent invocations
share no temporary path.
`mirnext_classic_benchmark` runs the original loop/sieve workloads entirely
through C++ sessions. Use `--loop` or `--sieve`, `--interpreter` for interpreted
bodies, and `--native-entry` for a real host ABI entry instead of a checked C++
call. It reports construction, load/compilation, native-entry preparation and
first-call latency separately at O0–O3 and checks the result. The first checked
call includes its cold host-adapter preparation. `--quick` reduces workload sizes; `--functions 1000
--compile-only` measures repeated compilation without executing the workload.
Numeric `--iterations`, `--passes` and `--flags` override the original defaults.
For binary serialization, build and run `mirnext_serialization_benchmark`; it
checks roundtrip bytes and reports writing time for repeated/distinct short and
long identifiers. Timings are informational rather than test thresholds.
C2MIR and other surrounding tools are outside the current core migration.

## GNU make entry points

The root GNUmakefile delegates to the same native CMake targets and installation
package. It does not probe a C compiler or install the old C headers/library.
Builds use two compiler jobs even with an outer `make -j`; CTest has independent
parallelism. The default build directories live under `build-make`.

```sh
make                         # Release libraries and native commands, no tests
make debug                   # Separate Debug product build
make test TEST_JOBS=8         # Native tests/examples, C oracle off
make reference-test          # Explicit temporary C oracle, requires C
make asan                    # Clang ASan tests, four CTest jobs
make bench                   # Quick native loop, sieve and format benchmarks
make install PREFIX=/your/install/prefix
make uninstall               # Uses that build's last successful install receipt
```

`BUILD_DIR` changes the common build root. `NATIVE_BINARY_DIR`,
`DEBUG_BINARY_DIR`, `TEST_BINARY_DIR`, `REFERENCE_BINARY_DIR`,
`ASAN_BINARY_DIR` and `BENCHMARK_BINARY_DIR` can select existing separate CMake
builds. `CONFIG` chooses Release by default; `debug` and `asan` use Debug.
`make clean` cleans the configured product build, and is harmless before it has
been configured. CMake remains available directly for arbitrary targets/options.

On Unix, `make install PREFIX=/opt/mirnext DESTDIR=/temporary/stage` supports
staged installation. The installation receipt records the actual staged prefix
and files. Uninstall reads that receipt without configuring/building or guessing
from the current `PREFIX`/`DESTDIR`; it preflights all entries, removes only listed
files, and preserves directories and unrelated files. Repeated uninstall is
harmless. Reinstallation in the same prefix merges owned files, including multiple
configurations. Uninstall the previous destination before selecting a different
one with the same build directory; the installer diagnoses remaining owned files.
Direct `cmake --install` remains supported; use GNU make's `install` entry to
create its corresponding uninstall receipt.

`make legacy-c2mir` explicitly builds the out-of-scope C2MIR command in a separate
directory. Historical C2MIR/ADT recipes are isolated in `c2mir/Legacy.mk`; invoke
them explicitly from the repository root with `make -f c2mir/Legacy.mk <target>`.
They are not evaluated by default product, installation, test or benchmark targets.
