# Native execution acceptance

The product's owning C++ IR, optimizer, backends and runtime run without the
original C engine. Removing the temporary C test oracle still requires native
execution evidence on each maintained platform. A successful cross-build alone
does not establish ABI or execution parity.

## Repeatable platform check

`mirnext-native-host` checks the actual host CPU, operating system and process
translation against the compiled target. Negative cases exercise admission using
synthetic host descriptions; they never execute code for another architecture.

`mirnext-native-acceptance-static` and `mirnext-native-acceptance-shared` repeat
that admission before executing a program built entirely with the public DSL.
The program exercises cross-module imports, an indirect call, a typed global,
a loop and an external host callback. Each linkage runs 21 scenes:

- Original IR through the interpreter.
- At each of O0–O3: optimized interpreter, eager JIT, lazy JIT, BBV and mixed
  lazy-JIT/interpreter execution.

Every scene makes five checked calls and one native ABI call, checks global
memory after each call, verifies engine selection and observes compilation.
The source Context and ExecutionSession are destroyed before the calls; owning
function, data and native-entry handles keep the loaded program alive.
All scenes must start from the same binary fingerprint. Interpreter ABI adapters
are counted separately from compiled user functions. BBV must encode reached
blocks. This smoke test complements the full semantic and differential suites;
it does not replace their ABI, pass or instruction coverage.

For a native Apple arm64 build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DMIRNEXT_BUILD_C_REFERENCE=OFF \
  -DMIRNEXT_TEST_EXPECTED_TARGET=aarch64-macos
cmake --build build --parallel 2
ctest --test-dir build -R 'mirnext-native-(host|acceptance)' \
  --output-on-failure --parallel 8
```

Other expected-target values are `aarch64-linux`, `x86_64-sysv` and
`x86_64-windows`. An empty value detects the native target. The SysV receipt also
records whether the actual host is Linux or macOS. A mismatched target returns a
diagnostic before creating an execution session. No Rosetta execution is used.

The executables write `native-acceptance-{static,shared}-<Config>.json` in the
build directory. Schema 2 receipts include the host, ABI, translation status, completed
scenes, call counts, machine-stage counts and input fingerprints. They are reset
to `running` before execution and become `passed` only after every scene succeeds.
The CTest wrapper also tries an incorrect target and verifies that failure
overwrites a previous successful receipt with zero completed scenes. A crash,
partial report or missing report cannot satisfy the gate.

Each receipt also identifies its compiled configuration, static/shared linkage,
whether the C oracle was enabled and a SHA-256 source fingerprint. The fingerprint
covers the native headers/implementations, commands, examples, test fixtures and
scripts, CMake/make rules, workflows and temporary C oracle. It includes untracked
source files and normalizes CRLF to LF across checkouts; it does not depend on a
Git commit, timestamps or local paths. Documentation outside the command guide
and out-of-scope C2MIR sources do not affect this identity. Source changes trigger
CMake regeneration; the CTest wrapper independently recomputes the fingerprint
and refuses to execute a stale acceptance binary. Linkage identity is compiled
into each executable, rather than supplied by a command-line argument.

## Auditing saved evidence

For a complete local run, keep the JUnit output in the build directory and audit
it after CTest finishes:

```sh
ctest --test-dir build --output-on-failure --parallel 8 \
  --output-junit "$PWD/build/native-tests.xml"
cmake --build build --config Debug --target mirnext_check_native_evidence --parallel 2
```

Select the actual build configuration for a multi-configuration generator.
The audit uses the source tree at audit time, the two acceptance receipts and
`native-test-manifest-<Config>.json`, which CMake emits after all tests are
registered. JUnit must contain exactly that inventory with no failed, skipped,
disabled or missing tests. Acceptance test output must match the receipt's source
and build identity. The audit rechecks every scene, call count, stage completion
and native host/ABI admission. A successful local audit records its explicit scope
in `native-evidence-<Config>.json`; it does not accept other platforms.

To check downloaded CI artifacts against the current source tree, extract each
artifact into its own directory, retaining its `build` and `build-core` paths:

```sh
python3 -B tests/Platform/AuditAcceptance.py --source . downloaded-artifacts
```

Without scope arguments, this requires all five maintained hosts, Debug and
Release, both reference-enabled and standalone builds, and both linkage variants:
40 passing receipts and their full-suite JUnit results. Repeated `--platform` and
`--configuration` options, plus `--reference on|off|both`, restrict the requested
scope for local checks. Missing platforms produce a diagnostic. The tool never
compiles, runs or translates foreign code. Python 3.8+ and CMake/CTest 3.21+ are
test-tool requirements only; installed libraries have neither dependency.
Synthetic negative artifact tests run as `mirnext-native-evidence-checks` and are
not themselves evidence that another host executed MIRNext.

## CI evidence and retirement gate

The native workflows request these hosts in Debug and Release, both with and
without the C oracle:

| Host | Expected target | GitHub-hosted runner |
| --- | --- | --- |
| Linux x86_64 | `x86_64-sysv` | `ubuntu-latest` |
| macOS Intel | `x86_64-sysv` | `macos-15-intel` |
| Windows x86_64 | `x86_64-windows` | `windows-latest` |
| Linux aarch64 | `aarch64-linux` | `ubuntu-24.04-arm` |
| macOS arm64 | `aarch64-macos` | `macos-15` |

Hosted runner selection follows the
[GitHub runner architecture table](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The Linux ARM job uses a native hosted VM, replacing the former `gcc117`
self-hosted requirement. Repository runner inventory on 2026-10-16 reports no
registered self-hosted runners. This change removes that scheduling dependency;
successful execution evidence still requires a workflow run of the current source.
Each workflow audits its native build and saves the receipts, configured test
manifests, audit summaries, complete CTest JUnit results and LastTest logs,
including on failure. These artifacts belong to that workflow run and revision;
configuring a workflow is not evidence that it has executed successfully.

Compilation uses two jobs. Local ordinary CTest uses eight, ASan uses four with
`ASAN_OPTIONS=detect_leaks=0`, and CI CTest uses four. Test parallelism is independent
of compilation parallelism.

Local native acceptance currently covers arm64 macOS. The x86_64 build-only
libraries and consumers establish compilation/linking only. Native full-suite
results from the other hosts remain required before removing the temporary C
oracle. The out-of-scope C2MIR tool has its own explicitly enabled legacy build.

The initial gate validation passed Debug **76/76** (82.31 s), Release **76/76**
(13.61 s) and C-reference-disabled ASan **62/62** (43.15 s). Both linkage receipts
in every configuration contain all 21 scenes and 126 successful calls. The
no-tests CXX-only product rebuild and external installed consumer **3/3** also
pass. All three new acceptance executables compile and link for x86_64; their
architecture is inspected without running them. These local results do not
establish the outstanding platform CI results.
