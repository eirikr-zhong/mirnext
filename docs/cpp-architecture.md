# Unified C++ architecture migration

The final architecture uses C++ interfaces throughout IR construction, analysis,
optimization, machine code generation and execution. The old C object model is
not a compatibility requirement. LLVM informs the layering; LLVM is not a dependency.
Public IR retains MIR register semantics, with SSA as an internal optimization stage.
Existing MIR text and binary formats remain an independent serialization concern.
The final migration target also removes all C implementation and C reference code.
The optional C oracle is a temporary validation aid, not a permanent dependency of
the finished repository; it remains only while replacement parity is being verified.

## Current implementation

The replacement layers live under `include/mirnext` and `lib`. They are independently
compiled as `MIRNext::IR`, `MIRNext::Analysis`, `MIRNext::Transforms` and
`MIRNext::Serialization`, `MIRNext::Target` and `MIRNext::CodeGen`. These layers
have no include path to the pre-rewrite runtime and do not call its adapters.
Each layer has a corresponding `Shared` target, including `MIRNext::ExecutionShared`.

Implemented:

- Context-owned identifier/type/signature interning; module-owned stable symbols,
  ordered module items, functions, data, imports and declarations.
- Function-owned basic blocks, registers and label anchors; block-owned stable
  instructions. Multiple labels can identify the same block.
- Checked `IRBuilder` and `IRRewriter`, hardware-binding identity, typed operands,
  public instruction role/type validation, diagnostic dumps and independent cloning.
- Structural rewriting inserts before/after stable instructions, splits a block at
  an instruction and merges an unlabeled fallthrough successor on the same stack
  scope. Transferred instructions retain their addresses, iterators and operand
  storage. Labels stay at the original split entry; only consumed blocks and erased
  labels invalidate their own references. Batch label cleanup retains all operand
  references and both anchors of label data. All structural edits invalidate CFG
  and instruction analyses; value-only insertion preserves the CFG cache.
- Early expression-initializer side-effect diagnostics, duplicate-export checks,
  import signature checks and direct/cross-module jump-call convention checks.
  Call convention collection scans each function once, not once per caller.
- Derived CFG with explicit edge kinds and duplicate switch edges, iterative traversal,
  register occurrence indexing, revision-aware module/function analysis caching and
  module/function pass managers with verification and fallible observation.
- Native CFG preparation merges fallthrough layout, removes empty unlabeled blocks
  and optionally prunes unreachable code. Co-located labels split into empty prefix
  blocks to preserve MIR's graph and PHI boundaries; their identities, layout
  positions and normal instructions remain unchanged. Batch erasure validates all surviving
  instruction and label-data references before changing ownership. Unconditional
  final jumps, switches and indirect jumps are valid, including infinite loops.
- Shared optimization graph with typed entry/exit/prologue boundaries, MIR's edge
  order and iterative DFS numbering; linear-storage dominator trees, sparse register
  liveness and ordered natural loop trees. Analysis dependencies reuse the same
  cached CFG; dependency cycles return diagnostics and unwind cleanly.
- Native maximal SSA construction, iterative PHI minimization, layout-order register
  renaming, conventional SSA and exit. `Function` owns the internal `SSAForm`;
  ordinary definitions and uses borrow canonical instructions and operand positions.
  PHIs retain their original incoming-edge order and positions between instructions,
  including copies inserted into self loops. Hardware assignments share one bound
  register and distinct definition identities; typed return-boundary uses retain effects.
  No duplicated opcode/operand body or fake public PHI/USE/NOP is involved.
- Native address-family conversion on the same instruction identities and SSA edges,
  including signed/unsigned narrow extensions, copy chains, PHI escapes, call
  escapes, multiple outputs and hardware-bound storage. Its normal result exits
  unrenamed SSA before a fresh renamed SSA build.
- Native block cloning, GVN, copy propagation, extension elimination, integer
  strength reduction and dead-store elimination over the same IR and SSA. Passes
  retain typed GVN memory metadata and ordered graphs through owned cache transfers;
  independent C stages compare both instructions and SSA edges.
- Standalone C reference probe, linked only to `mir.c` and `mir-gen.c`, which records
  O0–O3 snapshots and checks interpreter/JIT execution without C++ runtime adapters.
- Native `parse_text`, `write_text`, `read_binary` and `write_binary` construct and traverse the replacement IR
  directly, with owned parse results, contextual errors and resource limits.
  Symbol reservation and delayed data definitions preserve original module item slots.
  Coverage includes every public opcode, all 16 MIR regression files, hardware
  register aliases, block classes, varargs, label data and expression initializers.
- Independent MIR v1 binary records and compatible compression; raw and compressed
  input use the same native decoder. Binary parsing retains module record positions
  while resolving symbols and label anchors in two passes, with sparse label maps.
  It never delegates to the C reader or converts through text.
- Function verification checks overflow producers, intervening moves/stores,
  multiplication signedness and mixed ordinary/jump returns against the C rules.
- Function-owned stack scopes annotate canonical basic blocks. Allocation, explicit
  stack save/restore, loads/stores and local/symbol/label addresses have checked
  Builder helpers. Scope materialization runs directly on stable nodes, independently
  of the old runtime, and is available as `MaterializeStackScopesPass`.
  Both format writers materialize a clone only when auxiliary scope metadata exists.
- Native call DSL accepts functions, typed imports and signature symbols for indirect
  targets. `BlockArgument` carries address, offset, size and ABI classification;
  `CallResult` owns its list of stable result references. Ordinary indirect calls use
  `CALL`; jump calls and jump returns have separate helpers. Variadic scalar/address
  extraction and block copying use the same public instruction validator.
- Typed global-variable bindings attach to canonical symbols and ordered data/BSS
  items. Pointer initializers retain typed relocations; expression initializers retain
  their checked Function reference. Native loads/stores emit public instructions
  atomically, normalize booleans and enforce binding permissions. Cross-module imports
  validate types and permissions independently of module creation order.
- Eager typed expressions emit directly into canonical instructions, with arithmetic,
  bit operations, comparisons, explicit conversions and direct comparison branches.
  Return, assignment, memory and call helpers consume expressions through the same
  validator. No expression tree, cached body or old Builder adapter is involved.
- Native `SimplifyPass` traverses and rewrites these same stable nodes. Its rule
  order matches the C implementation: operand normalization, narrow argument and
  return extensions, unified returns, consecutive stack-allocation merging,
  algebraic identities, branch reversal/threading and label cleanup. Stack scopes
  materialize first. No second authoritative body or C IR adapter is involved.
  Floating and string literals append ordered, collision-free module data; typed
  provenance permits only verified scalar literal reads in expression initializers.
  Cloning remaps that provenance. Format writers restore those expression reads to
  literal operands on a clone because existing MIR files do not encode provenance.
- An independent simplify test hook runs only the C reference stage. Tests compare
  normalized instructions, label positions, signature operands, literal data order
  and contents, and hardware bindings on every public opcode, the original MIR
  corpus and focused rule boundaries. Direct operand injection additionally checks
  signed zero, quiet/signaling NaN payloads and unsigned immediate categories that
  textual normalization could otherwise hide. New-model fixtures also pass through
  the original interpreter/JIT at O0–O3 after native simplify and format roundtrips.
- Native `InlinePass` expands calls directly into owned basic blocks. It retains
  the 50/200 MIR-instruction limits (including labels), caller growth condition,
  traversal order, cold-code extraction, parameter/result moves, block copies,
  nested shared-frame alignment/reuse and dynamic stack save/restore. Hardware
  bindings reuse the caller's existing register identity. Cross-module references
  use stable symbols, checked renaming, exports/imports and signature relocation;
  original data positions and adjacent anonymous data stay in their ordered slots.
  No temporary authoritative function body is created. Direct use requires
  simplified callees; the Context pass-manager overload runs every function at one
  stage before advancing to the next. Scoped initialization is finalized even on
  callback failure. `examples/native-inline.cpp` demonstrates this ordering.
- A separate C-only inline hook compares exact instruction order, operand types,
  symbol identities, constants, label anchors and hardware bindings. It covers
  thresholds, recursion, nested/dynamic frames, cold code, all five block classes,
  RBLK, indirect and variadic exclusions, label data, cross-module private symbols,
  the original 16 MIR regressions and `reg2`. Native tests run without that oracle
  and check source lifetime, cloning, CFG construction, errors and both formats.

The transitional C++ compatibility runtime and its duplicate build targets have
been removed. The IR, machine pipeline, native linker and code publication use owning C++ objects.
The owning interpreter and JIT share one loaded program and independently callable
function handles, including opt-in lazy function and block compilation. The
x86_64 session path is connected, with native platform acceptance still pending.
Per-function engine selection now shares the same owning load pipeline;
native x86 acceptance and removal of the temporary C oracle remain.
No new-core execution entry silently calls the old model.
The architecture check guards the owning layers and rejects retired transitional
sources; CMake also rejects retired compatibility targets. The independent C
probe covers simplify, inline, graph preparation, dominance,
liveness, loops, the SSA lifecycle, GVN, copy propagation, dead stores, SSA dead
code, LICM, pressure relief, SSA combine and jump optimization. Each middle-end
pass now has a canonical implementation and owning pipeline/result API. The native
AArch64 backend is connected to this pipeline and the owning execution session.

## Remaining sequence

1. Complete native platform acceptance of the owning machine stages, host adapters,
   session execution, lazy entries and BBV on x86_64 Linux/macOS/Windows and
   aarch64 Linux. Local arm64 macOS execution and x86_64 build-only checks are
   recorded separately; see the [repeatable native gate](native-acceptance.md).
2. Remove the temporary C reference and its test-only observation protocol after
   replacement parity passes. The duplicate C++ core and runtime transport
   interfaces have already been deleted.
3. Finish all-platform installation, documentation and dependency checks with the
   temporary reference disabled.

### Transitional core retirement

The owning `include/mirnext` / `lib` graph is now the only product implementation.
Retirement removed 276 files (63,875 lines): the root-level transitional C++ core,
its C object projections and callback adapters, old `mir-tests/cpp-*` suites,
old `examples/cpp-*` examples and obsolete API documentation. C2MIR's transitional
C++ adapters were also removed; the original optional C tools remain outside the
MIR rewrite. No compatibility API or old library target is retained.

`MIRNEXT_BUILD_C_REFERENCE` is off by default and requires `BUILD_TESTING`.
Only this option or the independently explicit `MIRNEXT_BUILD_TOOLS` enables C.
The test oracle compiles `mir.c` and `mir-gen.c` independently; it does not link
the product or compile C2MIR. Private C implementation headers and target pattern
data remain while the original engine is needed for validation. Its test-only
observation protocol lives in `tests/Reference/COracle.h`; the former runtime
bridge header has been deleted. Removing all C sources is still an outstanding
final exit condition.
The previous `MIRNEXT_ENABLE_LEGACY_C` build option has been removed.

The six standalone C ADT drivers under `adt-tests` are also retired, including
the never-implemented memory-pool stub. VARR, DLIST and HTAB are implementation
details of the temporary reference engine; the product uses standard C++ owning
containers. Their old callback, allocator and macro APIs are not contracts of
the replacement architecture. The C reference itself still uses its original
private containers until the remaining native platform comparisons permit its
removal.

Coverage of replacement behavior stays in the native suites: stable node and
symbol ownership in `tests/IR/Core.cpp`, sparse-set operations against independent
ordered sets in `tests/Analysis/FunctionGraph.cpp`, and block-boundary,
dictionary, checksum and hostile-input compression cases in
`tests/Serialization/Binary.cpp`. The separate format oracle still compares C
and C++ compression bytes, reads both directions and covers multiple compression
blocks. This supersedes the old compression driver's dependence on the C2MIR
source file as its sole input. No optimization, machine-stage, format or execution
comparison is removed. CMake and the isolated legacy makefile no longer declare
the old ADT executables; the architecture guard rejects reintroducing them.

This cleanup removes six C files (416 lines) and five registered legacy ADT
tests. The C-reference-enabled build graph now compiles only `mir.c`, `mir-gen.c`
and `tests/Reference/Probe.c` as C translation units. Comparing CTest manifests
confirms that every native and semantic reference entry is retained; the
reference-disabled list remains exactly 62 entries. Native arm64 validation
passes Debug **71/71** (82.64 s), Release **71/71** (13.35 s), ASan **62/62**
(41.06 s, `detect_leaks=0`) and the external CXX-only consumer **3/3**. The
test-disabled product also rebuilds. Compilation uses two jobs, ordinary CTest
eight and ASan four. Other platforms' native execution acceptance is still
outstanding; this cleanup does not establish that gate or retire the C engine.

The root IR reference (`MIR.md`), target development guide
(`HOW-TO-PORT-MIR.md`) and code-memory guide (`CUSTOM-ALLOCATORS.md`) now describe
the owning C++ APIs. The IR reference retains public instruction semantics and
file spellings; the unused `mir-gen-stub.c` backend template has been removed
and is rejected by the architecture guard. No reference-engine capability was
removed with that template.

`examples/custom-code-memory.cpp` supplies a metered C++ `CodeMemoryProvider`.
Its lazy function remains callable after source Context and session destruction;
releasing the final function handle returns the active mapping count to zero.
The example is compiled and executed by CTest. The manual's loop also passes
native text/binary round-trip checks and interpreter, eager, lazy and BBV
execution. The example additionally compiles and links for x86_64 on the arm64
host, without execution or Rosetta.

After these guide/example changes, native arm64 Debug passes **72/72**
(82.13 s), Release **72/72** (13.53 s), reference-disabled ASan **63/63**
(43.27 s, `detect_leaks=0`) and the external CXX-only consumer **3/3**
(0.11 s), with no failures or skipped tests. The test-disabled product rebuilds.
Compilation uses two jobs, ordinary CTest eight and ASan four. Logs and JUnit
results use `/tmp/mirnext-native-guides-*`. Other platforms' native execution
acceptance and removal of the temporary C engine remain outstanding.

Native acceptance schema 2 binds execution receipts to the current source
fingerprint, compiled configuration, linkage and reference-build identity.
The fingerprint includes untracked core/test sources and normalizes checkout
line endings. CMake tracks additions, removals and content changes; the execution
wrapper also rejects a stale binary before running it. A generated complete test
inventory and acceptance identity in JUnit output let the test-only Python
auditor reject missing tests, skips, stale suites and mismatched host/ABI results.
The three platform workflows audit and retain this evidence. Python is required
only for tests; the CXX-only product and installed SDK remain independent of it.

Validation of this evidence chain on native Apple M1 arm64 passes Debug **73/73**
(84.10 s), Release **73/73** (13.55 s), reference-disabled ASan **64/64**
(41.75 s, `detect_leaks=0`) and standalone Release **64/64** (1.46 s).
Every full-suite artifact passes the native audit. Disabling the C reference
removes exactly its nine comparisons and retains every native test. All eight
local platform/configuration/reference/linkage combinations are accepted against
one source fingerprint; the reference-disabled Debug evidence is the ASan run.
The default all-platform audit correctly rejects the missing 32 combinations on
the other four hosts. Synthetic hostile/partial artifact checks are separate
from actual execution evidence. The three acceptance executables also compile
and link as x86_64, with their architecture inspected and no foreign execution.
The no-tests product builds with Python discovery disabled, and the external
CXX-only installed consumer passes **3/3** (0.06 s). Logs use
`/tmp/mirnext-evidence-*`; collected local artifacts are under
`/tmp/mirnext-evidence-artifacts`. Compilation remains two jobs, ordinary CTest
eight and ASan four. Native execution on the other hosts and retirement of the
temporary C oracle remain open; local evidence does not establish those results.

Public `Result` helpers now evaluate factory expressions once and move successful
payloads when extracting a value. The previous error macros could call a failing
factory twice, and extraction could not compile an owning `unique_ptr<Context>`.
Six support cases and a subprocess fatal-diagnostic check cover these regressions,
first-error preservation, move-only ownership and multiple extractions on one line.
Temporary identifiers remain stable in inline functions across translation units.
The native-session example uses these helpers with an owning ExecutionSession;
the installed static/shared SDK also exercises owning parser and session results.

Test-enabled installations no longer install the private doctest dependency.
A clean SDK contains 105 files, including all 84 public MIRNext headers, with no
doctest or C-reference files. The test-disabled product builds with Python
discovery disabled. External CXX-only consumers pass **7/7** (6.40 s), including
the native-session example and owning parser factories with both library linkages.

The Linux arm64 workflow now uses the native hosted `ubuntu-24.04-arm` runner;
the public repository's runner inventory contained no registered self-hosted
runners for its previous label. This removes a scheduling dependency, but does
not count as execution evidence until the current sources actually run there.

Current native Apple M1 validation passes Debug **75/75** (96.81 s), Release
**75/75** (18.56 s) and reference-disabled ASan **66/66** (82.22 s,
`detect_leaks=0`). Standalone Release with the reference disabled passes **66/66**
(16.65 s); disabling the reference removes exactly its nine comparison tests.
All eight local configuration/reference/linkage receipts pass the audit against
source fingerprint `910faba0a30f03fd47b2f47b5b21fabe03cedd6322f6d1fadc5a80829d3855ea`;
collected evidence is under `/tmp/mirnext-result-artifacts`. The full-platform
gate still rejects the 32 missing combinations on the other four native hosts.
The new support helpers and session example also pass strict
C++20 x86_64 syntax-only compilation; no foreign output is executed. Logs use
`/tmp/mirnext-result-*`. Compilation remains limited to two jobs; ordinary CTest
uses eight and ASan four. Other hosts' native execution and retirement of the
temporary C oracle remain outstanding.

Retirement reviewed an inventory of 706 old test declarations. That is not a
claim that 706 identical tests were mechanically ported: old projection corruption,
allocator adoption and bridge-selection contracts cease to exist in this design.
Semantic coverage is maintained in the following owning suites:

| Retired test concern | Owning coverage / disposition |
| --- | --- |
| Builder, instruction legality, module items, typed ownership and errors | `tests/IR/{Core,Calls,Expressions,Variables,Scopes,Rewriter}.cpp`; `tests/IR/PublicInstructions.hpp` |
| Text/binary codecs, compression, every public instruction and original MIR corpus | `tests/Serialization/{Text,Binary,Export}.cpp`; independent format reference |
| CFG, SSA, all middle-end passes and observations | `tests/Analysis`, `tests/Transforms`; `tests/Reference/{Analysis,SSA,Simplify}.cpp` |
| Machine lowering, legalization, register analysis/allocation, spills, post-allocation, frames and encoding | `tests/CodeGen`; independent `tests/Reference/MachineIR.cpp` and x86 pattern oracle |
| Builtins, ABI entries, executable memory, relocations and native recovery | `tests/Execution/{Builtins,NativeEntries,NativeBoundary,RuntimeEntry,CodeMemory,Recovery}.cpp` |
| Linking, globals, expression data, source lifetimes and repeated execution | `tests/Execution/{Linker,Session,Callable,InvocationState,SourceSnapshots}.cpp` |
| Eager/lazy/BBV and per-definition execution | `tests/Execution/{Lazy,BlockEntry,BBVRuntime,SessionBBV,SessionModes,MixedEngines}.cpp`; independent C execution comparisons |
| Old `LegacyC` selection, C array/list projections, C owner adoption, bridge corruption | Removed with those interfaces; owning identity, rewrite and architecture checks validate the new contracts |
| C2MIR storage adapters | Out of MIR scope; original C tools remain explicitly optional |

`SourceSnapshots.cpp` adds an explicit replacement for a meaningful old session
case: load two module snapshots around a source rewrite, rewrite the source again
before either cold compilation, then check independent instructions and mutable
global data under all four engines. Both static and shared execution suites run it.
The x86 pattern alias declarations used only by the C oracle now live in
`tests/Reference/X86PatternAliases.hpp`. The encoding microbenchmark no longer
links or measures the deleted transitional encoder; it still checks byte fingerprints
and reports native encoding timings.

The full Debug suite after retirement passed 108/108 (135.06 s), Release passed
108/108 (34.20 s), and C-reference-disabled ASan passed 36/36 (55.35 s,
`ASAN_OPTIONS=detect_leaks=0`). Static and shared execution suites each passed
142/142 cases and 36,968 assertions, with no skips. The 19 removed
CTest entries comprise eleven retired compatibility test entries and eight
old API examples; no owning suite was removed. Validation recorded in the next
sections predates this retirement unless explicitly identified otherwise.

Additional retirement gates on native Apple M1 arm64:

- A fresh default no-tests/no-examples configuration succeeded with a nonexistent
  C compiler and without overriding the C-reference default. Its actual Ninja
  `all` graph has 102 C++ compile/link commands, with no transitional core,
  C engine or C2MIR. The no-tests Release build succeeded, including all optional
  native benchmarks in a separate configuration step.
- Debug and Release installations were each renamed before configuring external
  CXX-only consumers; neither staging path remained. Each consumer passed 3/3,
  including static/shared execution and incompatible-linkage rejection, with
  exceptions and RTTI disabled. The CXX-only subproject rebuilt all 114 commands
  and passed 6/6. Every consumer used a nonexistent C compiler.
- The independent x86 reference sources passed `-arch x86_64 -fsyntax-only`
  after moving their private pattern aliases. This is a compile check, not native
  x86 execution acceptance; no Rosetta or foreign execution was used.
- The explicitly enabled original C tools built successfully. Sieve returned the
  same result (1027) through their interpreter and JIT, and text/binary/table
  conversion passed a byte-stable roundtrip. This preserves the out-of-scope tools;
  it is not a dependency of native tests or an installed product library.
- The updated native encoding benchmark completed its three sizes with stable
  fingerprints across seven repetitions at each size. No before/after speedup is
  inferred from removing its obsolete transitional comparison path.
- Enabling the C oracle with tests disabled returns a configuration diagnostic.
  Product layer creation requires C++ sources under `lib`; architecture checks
  also reject C source/header files under the owning product directories.

All builds used two jobs. Logs use `/tmp/mirnext-retirement-*.log`. These results
close the local retirement gate; they do not establish native x86 platform parity
or complete removal of the temporary C reference and original C tool sources.

### Retired runtime service handoffs

The temporary C oracle no longer contains the eight runtime service callback
tables formerly installed by the deleted transitional C++ engine: code memory,
thunks, wrappers, stack helpers, variadics, builtins, interpreter entry shims and
foreign calls. Their installation APIs, context fields, callback-only test probes
and always-false bridge failure checks have been removed. The standalone C
interpreter and JIT continue directly through their existing C implementations;
their algorithms and emitted instruction rules are unchanged.

The unused `mir-legacy-entrypoints.def` dispatch manifest has also been deleted;
the architecture guard rejects its reintroduction. This increment removes another
403 lines of migration code. The original C engine and the IR, linker, format,
interpreter-storage and generator transports were still present at that increment.
The following increment removes the linker/interpreter handoffs as well. None of
these are product dependencies.

Validation on native Apple M1 arm64: Debug full CTest passed 108/108 (133.71 s)
and Release passed 108/108 (33.76 s), including the independent per-stage,
format and execution comparisons. Both builds recompiled only the C oracle and
its adapters, with no native product object recompilation. The C tools rebuilt
without test hooks; interpreter/JIT sieve results remained 1027 and the binary
roundtrip remained byte-stable. The two original C translation units also passed
x86_64 syntax checking, without execution or Rosetta. The Release oracle's symbol
table no longer exports any of the eight service installation endpoints, the
bridge failure query or the code-region transport. The updated architecture guard
passed in the ASan configuration. Logs use `/tmp/mirnext-runtime-services-*.log`.

### Retired linker and interpreter object handoffs

The standalone C oracle no longer accepts an alternate linker or interpreter
storage owner. The callback structs, context fields, private takeover APIs and
callback-only probes have been removed. Public C linking and interpretation in
that oracle use their existing C implementations directly. Current independent
comparisons still use the same stage hooks and public C interpreter/JIT entries;
none of their fixtures or comparison assertions were removed.

Prepared C interpreter code now belongs to a descriptor private to `mir-interp.c`,
with inline instruction storage instead of a projected descriptor and a pointer
into another owner's code. The C function object no longer stores a C++ body
destructor callback. The interpreter initializes its own dispatch table once and
always creates its own stack helpers; dispatch exchange, bridge preparation flags
and the unused optional-helper initialization mode have been deleted.

This grouped cleanup removes 249 lines. The optional oracle still contains the
old IR storage, Context owner, format storage and generator transports at this
increment; the following increment retires the Context/format tables. The native
product keeps its independent C++ objects and has no dependency on those tables.

Validation on native Apple M1 arm64: Debug full CTest passed 108/108 (146.67 s,
two test jobs); Release passed 108/108 (29.25 s, eight test jobs). Builds remained
limited to two compiler jobs and recompiled no native product library objects.
All 22 retired endpoints were absent from the Debug oracle's exported symbols.
The standalone C tools rebuilt without test hooks; interpreter/JIT sieve results
remained 1027, and text/binary/table conversion preserved the 145-byte fixture.
Both C translation units passed x86_64 syntax checking without execution or
Rosetta. Interpreter-disabled, switch-dispatch and trace configurations passed
syntax checks; switch and trace variants also executed repeated interpretation,
host interface calls, stack save/allocation/restore and context destruction.
The architecture guard passed in the ASan configuration. Logs use
`/tmp/mirnext-object-bridge-*.log`.

Test concurrency is independent of compilation concurrency: CI now runs CTest
with four jobs, native eight-core validation uses eight, and the documented ASan
command uses four. Compilation remains at two jobs. The Debug run above was
already running when test concurrency was increased and was allowed to finish.

### Retired Context and format ownership handoffs

The standalone C oracle now owns its Context lifecycle, instruction descriptors,
prototype allocation and simplify value table directly. It no longer accepts a
Context owner callback table. Simplify values are private implementation values,
rather than a transport type for the deleted transitional C++ containers.

Its text scanner, binary reader/writer, compression streams, output string table
and scan label table also use the original C implementations directly. The format
takeover table and dispatch branches have been removed; scanner labels are private
C auxiliary state. No native C++ format or execution code changed. Existing
per-stage comparisons and complete text/binary compatibility checks remain intact.

This cleanup removes 215 lines and nine unused takeover/storage inspection probes.
Only the IR runtime storage and generator callback tables remain in the optional
oracle's Context at this increment; the following increment removes IR runtime
storage as well. Neither is a native product dependency. The independent C oracle
and native x86 platform acceptance are still required migration work; retiring
these handoffs does not establish complete removal of the C implementation.

Validation on native Apple M1 arm64: Debug full CTest passed 108/108 (83.28 s)
and Release passed 108/108 (16.78 s), both with eight test jobs. Compiler jobs
remained at two; no native product library objects were rebuilt. The nine retired
inspection endpoints were absent from the Debug oracle's exported symbols.
Syntax checks passed for both C translation units targeting x86_64 (no execution
or Rosetta), and for the C runtime translation unit with IO disabled, scanning
disabled, both disabled, and binary compression disabled. A standalone build
without test hooks also wrote/read uncompressed binary, destroyed the source
Context before interpreting the decoded function, and repeated the complete
lifecycle successfully. The original optional C tools rebuilt without test hooks;
interpreter/JIT sieve results remained 1027 and text/binary/table roundtrips
preserved the 145-byte fixture. The architecture guard passed in the ASan
configuration. Logs use `/tmp/mirnext-context-format-*.log`.

### Retired IR runtime storage and object projections

The optional C oracle now allocates and destroys its own strings, aliases,
register tables, signatures, modules, items and instructions. Its public C
construction, queries, operand operations, dump and instruction-list edits no
longer dispatch through the deleted transitional C++ IR storage table. Module and
instruction owner descriptors, alternate register/signature flags, separate
payload pointers and cold C/C++ register/symbol transfer endpoints have been
removed. Results and data payloads use the C implementation's inline storage;
register descriptors are private C values. Current stage comparisons and format
compatibility fixtures retain their existing assertions.

C-to-C module transfer still re-interns register/argument names, strings and
memory alias identities, preserves both instruction lists, and invalidates a
prepared interpreter body before moving its module. It no longer attempts to
adopt projected instructions or swap container owners. The surrounding optional
C2MIR tool's startup no longer requests a deleted core storage transport; its
zero-initialized C storage remains the selected implementation. This is dependency
removal, not a C2MIR rewrite. The core C file no longer includes the C2MIR storage
header. Native product code did not change.

This grouped cleanup removes 712 lines. At this checkpoint the generator table
and per-function generator-stage takeover fields remained in the optional oracle;
the following group retires them. Native x86 platform acceptance and eventual
retirement of the C oracle are still outstanding.

Validation on native Apple M1 arm64: Debug full CTest passed 108/108 (91.29 s)
and Release passed 108/108 (17.47 s), with eight test jobs and two compiler jobs.
Neither rebuild compiled native product library objects. All 16 retired private
entry points (including the unused list-reordering entry) were absent from the
Debug oracle's exported symbols. Both C translation units passed x86_64 syntax
checks without execution or Rosetta; IO-disabled, scanner-disabled, combined and
uncompressed configurations also passed compilation checks.

A standalone C-to-C transfer check compiled the C runtime with AddressSanitizer
and without test hooks. It moved a module containing variadic prototypes, inline
data, BSS, references, label references and expression data; remapped string and
alias operands in both original and duplicate instruction lists; destroyed the
source Context; restored the original body; and executed/link-initialized the
result in the receiving Context. Repeated execution and destruction passed with
`ASAN_OPTIONS=detect_leaks=0`. The optional C tools rebuilt and retained sieve
results of 1027 through interpreter/JIT and byte-stable 145-byte text/binary/table
roundtrips. The architecture guard also passed in the ASan configuration. Logs
use `/tmp/mirnext-ir-storage-*.log`; the removed source group is backed up in
`/tmp/mirnext-ir-storage-retirement-before.tar.gz`.

### Retired generator and CFG takeover

The optional C oracle now owns its generator root, target context, CFG nodes,
SSA/GVN storage, live ranges, allocation state and BBV storage directly. The
generator and CFG callback tables, alternate target-pattern pointers and
per-function stage/payload fields have been removed. Its driver runs its own
simplify, middle end, lowering, allocation, frame and encoding stages in the
existing order and with the existing optimization-level conditions; there is no
longer a path for installing native objects or skipping previously completed
stages. Native product code and its compilation pipeline did not change.

The test adapter still reads independent C stage snapshots, call registers,
liveness, ranges, allocation, frame, labels and encoded bytes. Its machine-state
accessors remain because frame comparisons explicitly exercise injected target
facts. These test records do not install a C++ pipeline into the C generator.
Unused takeover endpoints and probes have been removed rather than retaining
unreachable implementations: 78 retired private symbols are absent from the
Debug oracle library. The grouped cleanup removes 1,612 lines.

Validation on native Apple M1 arm64: Debug full CTest passed 108/108 (88.68 s)
and Release passed 108/108 (18.86 s), with eight test jobs and two compiler jobs.
Neither rebuild compiled native product library objects. x86_64 syntax checks
passed for both C translation units without execution or Rosetta. IO-disabled,
scanner-disabled, combined, uncompressed, generator-debug-disabled and
generator-call-trace configurations also passed compilation checks.

A standalone C check, compiled with AddressSanitizer and without test hooks,
passed 48 generator lifecycles across eager JIT, lazy JIT and BBV at O0–O3. It
checks repeated calls to multiple functions, dynamic stack storage, loops and
global writes, followed by generator and Context destruction. The ASan
architecture guard also passed; these checks use `detect_leaks=0`. Optional C
tools rebuilt and retained interpreter/JIT sieve results of 1027 and a byte-stable
130-byte text/binary/table roundtrip. Logs use `/tmp/mirnext-generator-*.log`;
the source group is backed up in
`/tmp/mirnext-generator-retirement-before.tar.gz`.

Native x86 runtime/platform acceptance and eventual removal of the optional C
oracle remain open. This cleanup does not establish those results or claim a
native product performance improvement.

### Retired machine bridge header and projected operands

`mir-machine-bridge.h` has been deleted. Its unused CFG, graph, range, allocation,
rewrite, encoding and split-state installation records no longer exist. The
independent comparisons declare only the seven records they consume in
`tests/Reference/COracle.h`. Stage-observer declarations have also moved out of
the C public header. Observation accessors are compiled only with
`MIR_OPT_TEST_HOOKS`; the standalone C generator has no corresponding exported
symbols, and neither C translation unit depends on a test header with hooks off.

The C oracle's module, item, instruction and CFG edits now use their original
intrusive-list operations directly. Literal operation dispatchers, 45 unused
counter and transport probes, their instrumentation, and the unused alias-query
entry have been removed: 51 retired private entry points are absent from the
oracle library. The optional C2MIR tool no longer includes a deleted bridge or
calls those dispatchers; its frontend and storage subsystem remain outside the
MIR rewrite. Existing reference case, assertion and skip counts are unchanged.

C instructions again own an inline operand tail. The alternate operand pointer
and unused capacity field were only needed by the retired C++ projection and
have been removed. On native arm64 this reduces `sizeof(MIR_insn)` from 96 to 80
bytes. This is a storage reduction in the temporary C oracle, not a claim about
native C++ product performance. Including the strengthened architecture guard,
this group removes 678 source/test-protocol lines. The guard also rejects any
reintroduction of the deleted machine bridge header.

Native Apple M1 arm64 validation passed Debug full CTest 108/108 (90.23 s) and
Release 108/108 (16.51 s), with eight test jobs and two compiler jobs. Native
product library objects did not rebuild. x86_64 syntax checks passed without
execution or Rosetta, as did the IO/scanner/compression and generator-debug/trace
configurations exercised in the previous group. Without test hooks, ASan checks
passed both C-to-C module transfer after source destruction and 48 eager/lazy/BBV
generator lifecycles at O0–O3, with `detect_leaks=0`. The ASan architecture guard
passed as well. Optional C tools retained sieve results of 1027 and byte-stable
130-byte text/binary/table conversion. Logs use
`/tmp/mirnext-oracle-bridge-*.log`; the previous source group is saved in
`/tmp/mirnext-oracle-bridge-before.tar.gz`.

The original C implementation still serves as an optional oracle. Native x86
runtime/platform acceptance and its eventual retirement remain required before
the full migration can be called complete.

### Native machine preparation: module hardware bindings

Machine preparation still reserves hardware bindings declared by any function in
the same module. It now consults each function's existing binding index before
visiting its registers, avoiding scans of ordinary registers in unrelated
functions. No module cache or second source of binding identity is introduced.
Declaration order, physical-register deduplication and target diagnostics retain
their existing behavior. A regression covers declarations added after an earlier
preparation, compatible aliases, incompatible aliases, module isolation and a
clone used after its source Context is destroyed.

The machine-IR benchmark includes modules with 64, 256 and 1,024 functions, each
with 256 ordinary registers, with and without a hardware declaration. Five
alternating Release runs on native Apple M1 compare the median preparation time
of a small entry function. With one hardware declaration and 1,024 unrelated
functions, the median of run medians falls from 460.833 to 1.833 microseconds.
Instruction dump fingerprints and binding counts agree. This measures machine
preparation in a large module, not whole-program JIT throughput. The existing
4,096-symbol benchmark remains comparable at 3.342/3.360 milliseconds.
Benchmark logs use `/tmp/mirnext-hardware-index-{before,after}-*.log`.

Native arm64 validation passed Debug full CTest 108/108 (94.58 s), Release
108/108 (26.75 s), and C-reference-disabled ASan 36/36 (37.01 s,
`detect_leaks=0`). Installed static/shared CXX-only consumers passed 3/3.
Compilation used two jobs; ordinary CTest used eight and ASan used four.
Validation logs use `/tmp/mirnext-hardware-index-*.log`. Native x86 runtime
acceptance and retirement of the optional C oracle remain open.

### Native differential execution and partial local stores

`tests/Execution/SeededPrograms.hpp` owns a fixed-seed corpus and an independent
unsigned, bit-level evaluator. Bounded loops combine signed-overflow branches,
ordinary branches, overlapping memory arguments, partial local stores, indirect
cross-module calls and hardware-register side effects. Expectations include the
whole memory image, repeated calls and guard words. The native test runs without
the C oracle, with static and shared linkage. It covers raw interpretation,
O0–O3 optimized interpretation, eager/lazy JIT, BBV and both interpreter/BBV
boundary directions. Text, raw binary and compressed binary roundtrips feed the
same executions.

This corpus exposed an inherited address-conversion defect. For a natural-width
64-bit local, replacing `mov i32:(address),value` with `ext32 local,value` loses
the high bytes that a later 64-bit read must retain. Address conversion now
compares each family's narrowest store with the widest address view of the same
register, including direct register reads. Partial
writes to a wider object retain real storage; compatible
narrow-only families still scalarize. Separate native regressions cover signed
and unsigned 8/16/32-bit stores, direct reads and aliases through different address
widths. Functions without narrow stores avoid the extra width analysis.

The original C implementation remains unchanged as an independent oracle. A
minimal regression records its exact discrepancy: a local initialized to
`0x123456789abcdef0`, updated through a 32-bit store of 1, reads back as
`0x1234567800000001` in the C interpreter and C O0/O1 JIT, but as 1 in C O2/O3.
Native execution requires the preserved value at every level. This is an
intentional correction of the original optimizer defect, not a skipped native
pass. The shared memory-only corpus is compared against C interpretation and
C O0/O1 JIT/BBV; its C O2/O3 behavior is not treated as a semantic oracle for
partial local stores. Existing phase comparisons remain enabled.

Final native Apple M1 arm64 validation passed Debug CTest 108/108 (86.81 s),
Release 108/108 (17.57 s), C-reference-disabled ASan 36/36 (36.38 s,
`detect_leaks=0`), and installed static/shared CXX-only consumers 3/3. Each native
linkage's execution suite passed 144 cases and 64,329 assertions without skips.
The no-tests build passed with 102 commands and no C source commands; a fresh
configuration also succeeded with an unavailable C compiler. Compiler parallelism
was two, ordinary CTest eight, and ASan four; no Rosetta was used. Validation
logs use `/tmp/mirnext-seeded-final-*.log`, with the no-tests dependency audit in
`/tmp/mirnext-seeded-no-tests-audit.log`. Native x86 runtime/platform acceptance
and removal of the optional C reference still remain required.

### GNU make uses the native product and installation graph

The root GNUmakefile previously still built `libmir`, installed C API headers
and ran C2MIR from its default product/test recipes. It now delegates product,
Debug, native test, ASan, benchmark and installation work to the owning CMake
targets. The default graph does not evaluate shell probes for a C compiler.
All public installations contain the seven C++ layers, matching shared variants,
namespaced headers, exported CMake targets and native commands. C2MIR/ADT's
historical recipes moved to the explicitly invoked `c2mir/Legacy.mk`; the root
file neither includes nor evaluates them. `legacy-c2mir` remains a separate,
explicit out-of-scope tool build. The architecture guard checks the root entry
file against reintroducing C product rules/probes.

Build concurrency remains two, even under an outer `make -j8`; root target
orchestration is serial while CMake controls the compiler jobs. CTest has its
own parallelism (eight normally, four for ASan). Product, test, Debug, ASan,
reference and benchmark builds have separate default directories. Historical
MIR correctness aliases use the native execution suites. C comparisons require
the explicit `reference-test` target and never enter the default test graph.

GNU installation uses CMake's SDK export and records actual owned files,
including Unix DESTDIR staging. Uninstall neither configures nor builds and
uses the receipt's destination independently of current prefix/environment
arguments. It preflights the entire receipt before removing files, rejects
relative/outside paths and escaping parent symlinks, and preserves unlisted
files/directories. Repeated uninstall is harmless. Reinstalling the same prefix
merges file ownership; a different destination requires uninstalling remaining
owned files first. Native subprocess tests cover plain, absolute/relative staged
installations, changed file sets, destination changes, malformed receipts and
symlink behavior without any compiler or C oracle dependency.

Validation on native Apple M1 arm64: default GNU product build and installation
passed with a trap C compiler that was never invoked. The fresh cache enables
CXX only, and all 105 installed files belong to the native SDK/commands. Native
GNU tests passed 59/59 (1.45 s); explicit full Debug/Release C comparisons passed
73/73 (86.93/14.21 s). GNU ASan including all examples passed 59/59 (58.19 s,
`detect_leaks=0`). Final receipt/architecture checks passed in all three builds.
Plain and staged SDKs each passed an external CXX-only static/shared consumer
(3/3); both uninstalls removed only owned files and retained unrelated sentinels.
Staged uninstall also ignored deliberately different current PREFIX/DESTDIR.
The GNU quick benchmarks passed loop/sieve O0–O3 and a 32-function format
roundtrip. YAML syntax checks passed for the native package CI additions on Unix
Release hosts; remote CI/platform results are not claimed. Builds used two
compiler jobs and no Rosetta. Logs use `/tmp/mirnext-make-*`. Native x86 platform
acceptance and retirement of the optional C oracle remain outstanding.

### Native format commands and binary/embedded execution

`m2b`, `b2m` and `b2ctab` now construct owning C++ IR through the independent
serialization layer. `mir-bin-run` and the optional `mir-embedded-run` use
`ExecutionSession`, with interpreter, eager/lazy JIT and BBV selection. The
format and execution commands build/install by default under
`MIRNEXT_BUILD_UTILITIES`; they do not enable the C language or depend on the
C2MIR tool/original engine. A generated embedded array is compiled and executed
in native tests, rather than merely checking the array's declaration.

The runner validates a unique main and its argument/result contract before
loading code, passes the program's argv and host environment, and propagates
the integer process status. Direct import call prototypes supply checked host
signatures; inconsistent prototypes, unknown symbols and address-only untyped
bindings return diagnostics. Explicit arbitrary function/data bindings remain
available through the typed C++ execution API. System/custom shared libraries
use RAII handles retained until all session code and handles are destroyed.
Platform-specific loading stays at the OS ABI boundary. Environment library
lists use Unix colons or Windows semicolons, preserving Windows drive letters.

Buffered input replaces byte-at-a-time VARR growth. Format parsing completes
before output is published. Embedded arrays use a bounded output buffer,
checked I/O and canonical reencoding, with no shared temporary path. Tests
roundtrip the original sixteen MIR files through the actual command processes,
check canonical text/name normalization and binary stability, compare embedded
bytes (including output-buffer boundaries), and execute argv/env, system and
custom-library calls in all four modes. Diagnostics cover malformed input,
usage, missing/duplicate main, invalid main contracts, inconsistent prototypes,
unknown imports, missing libraries and invalid engine selection; Linux also
checks `/dev/full` output failure. Native architecture checks cover `tools`
and reject the five retired C sources as well as C representations/includes.

The five retired C command/embedded sources total 581 lines. GNU make's native
command aliases share one CXX-only build and copy its executables; its runner
test no longer compiles a C2MIR input as a prerequisite. The out-of-scope C2MIR
bootstrap recipe now references the optional native embedded target. That
compiler's bootstrap itself is not part of this core acceptance.

Validation on native Apple M1 arm64: Debug full CTest passed 72/72 (101.07 s),
Release 72/72 (24.34 s), and reference-disabled ASan 38/38 (54.34 s,
`detect_leaks=0`). Final command/embedded/architecture checks passed in all
three configurations after extending corpus and error coverage. The fresh
installed CXX-only static/shared consumer passed 3/3; installed commands passed
the subprocess suite in a work directory containing spaces. The public embedded
header option also compiled and ran all four engines with a header path containing
spaces. GNU make completed a fresh CXX-only command build, and its copied commands
passed the same suite. The default no-tests build has 112 commands and no C source
compilation; with utilities off, the graph retains 102 library-only commands and
does not enable C. New command sources passed x86_64 syntax checking only.
Build concurrency stayed at two; normal CTest used eight and ASan four. No
Rosetta was used. Logs use `/tmp/mirnext-cli-*`. Native x86 platform acceptance
and removal of the optional C oracle are still required.

### Utility drivers, native function pointers and matched format I/O

The remaining standalone `mir-tests/*.c` drivers now use native C++ tests.
`tests/Execution/UtilityPrograms.cpp` covers the old scanner syntax, memory
simplification shapes, API sieve, hello callbacks, issue 219 cross-module
inline loading and owning entries in place of raw thunk unwrapping. All five
execution paths (raw/optimized interpretation, eager/lazy JIT and BBV) cover
O0–O3 where applicable; retained entries survive source/session destruction.
The old incremental load example is expressed as successive immutable sessions
around additions to a stable source Context. Old and new handles keep their
independent loaded snapshots.

Migrating hello exposed a runtime gap: the native interpreter previously
required an indirect target to appear in its loaded symbol index. Ordinary
host function pointers supplied as arguments now use the call's checked MIR
signature and the existing owning `NativeCalls` ABI service. Known loaded
addresses still require matching signatures and conventions; external JCALL,
null targets and disabled native calls return diagnostics. Native adapters stay
cold until used. Their provider/observer live with the loaded program even
after source/session destruction, and failed compilation can retry without
poisoning the adapter cache. Virtual interpreted identities cannot escape to
native code without ordinary ABI entries. Foreign callback reentry shares the
original invocation's instruction/depth budgets and hardware transfer bank.
Tests include variadic and copied block arguments, cache reuse, rejected
observation, budget exhaustion and recovery. An isolated C execution check
retains the original hello callback results at every optimization level.

`tests/Fixtures/scanner.mir` retains the original comments, semicolon syntax,
labels and alias annotations. Its inclusive loop bound is past its allocation,
so it is a format/pass fixture only. The old memory sample ended with an
unreachable move and no final return: that exact shape now has an explicit
fallthrough diagnostic test; its valid transformation fixture adds a final
unreachable return. Arbitrary absolute/index-only addresses are never executed.
Independent C simplify snapshots cover these shapes and the safe API sieve;
the independent C format reader/writer also roundtrips four new fixtures.

`mirnext_format_io_benchmark` replaces the old I/O benchmark using the exact
same owning API-built sieve for API construction, text parsing and binary
reading. This removes the old comparison's algorithm differences. Canonical
text/binary checks validate the result before reporting timings. Private
temporary streams and buffered reads replace the shared `/tmp/__tmp.mirb`
path and byte-at-a-time scan. Construction, format conversion and stream I/O
are measured separately; the default remains 100,000 functions and smaller
counts support quick validation. GNU make correctness aliases share one native
corpus invocation. Benchmark aliases share one configuration/build prerequisite,
preventing simultaneous writes to their common native build tree.

Retirement deletes seven C drivers and three sole-use API headers (472 lines),
three CMake executables and three CTest registrations. Full local CTest now
registers 70 items rather than 73, while each native execution variant gains
nine cases. The architecture guard rejects any new `mir-tests/*.c` driver.
The old C engine/ADT oracle remains pending platform acceptance; two shared
scanner-constructor headers remain only for the out-of-scope MIR2C tool tests.

Validation on native Apple M1 arm64: Debug full CTest passed 70/70 (120.60 s),
Release 70/70 (27.86 s), and reference-disabled ASan 36/36 (46.58 s,
`detect_leaks=0`). Both native execution linkage variants passed 160 cases and
82,225 assertions without skipped cases. Independent C simplify, execution and
text/binary format comparisons include the new fixtures. API sieve stack sizes
use signed immediates, as in the original sample, so the strict C operand
snapshots agree across text and binary; text's documented signed normalization
and lossless binary unsigned modes remain unchanged. The scanner fixture's two
modules are checked explicitly in both comparison directions.

A fresh installed CXX-only static/shared consumer passed 3/3 tests. The no-tests
product build passed; its 102 build commands contain zero C source compilations.
GNU make's shared benchmark prerequisite built both native benchmarks with the
C reference and tests disabled. Four concurrent format benchmark processes
(1, 32, 32 and 1,000 functions) passed canonical and private-stream checks;
invalid counts returned diagnostics. New sources also passed x86_64 syntax
checking, which is not native ABI/execution acceptance. Builds used two compile
jobs, ordinary CTest eight, and ASan four; no Rosetta was used. Logs use
`/tmp/mirnext-utility-*`. Native x86 platform acceptance and retirement of the
optional original C oracle remain outstanding.

### Classic drivers and workloads use owning C++ services

`tests/Execution/ClassicPrograms.cpp` replaces the original loop, sieve,
greeting, mixed-argument and README execution drivers in both native linkage
variants, including reference-disabled builds. The original loop now uses the
owning block/label DSL. Native text fixtures retain the other programs' MIR
instructions, signatures, alias annotations and cross-module imports, without
including C constructors or C engine headers.

Raw/optimized interpretation, eager/lazy JIT and BBV run at O0–O3 through both
checked calls and actual host ABI entries. Checks cover negative/zero/positive
loop bounds; captured greeting characters and return values; nineteen mixed
arguments, exhausted register banks, signed extension and unsigned limits; and
repeated sieve memory updates. The large 819,000-flag sieve runs eagerly
(8,190 on Windows, matching the old stack-size restriction). Other engines use
127 flags. The README's odd-only sieve uses 257 flags and two iterations, and
runs all sixteen caller/callee engine pairs at every optimization level. This
retains real imports and host calls while separating correctness from benchmark
duration. The source IR is checked for mutation.

The first sieve intentionally starts with number 3, omits 2 and counts 4. Its
result equals the ordinary prime count for the tested sizes, although its
marked set differs. The independent C interpreter/JIT oracle now asserts the
same loop results and sieve count at all four levels. The README's different
odd-only algorithm excludes 2 normally. Independent C++ prime counting supplies
both expectations; migration does not change either original algorithm.

`mirnext_classic_benchmark` retains full loop/sieve workload sizes, configurable
function counts, checked/native ABI entry choices, interpreter/JIT execution and
compilation-only runs. Timing separates construction, load/compilation,
native-entry preparation and first-call latency (including a cold adapter for
checked calls); generated code bytes replace the old process-wide heap-growth
estimate. GNU make's interpreter/generator/README correctness aliases share the
native corpus invocation. Its loop/sieve benchmark aliases build this CXX-only
target with tests and the C reference disabled, using two compile jobs.

Six C drivers and their sole-use `scan-args.h` are deleted (391 lines), with ten
C build targets and ten redundant CTest registrations. Full local registration
count is now 73 rather than 83; the native suite gains four cases. Shared C
fixtures used by remaining utility/oracle or out-of-scope tool tests remain
until those consumers retire. The architecture guard rejects reintroduction of
these retired drivers.

Validation on native Apple M1 arm64: Debug full CTest passed 73/73 (86.94 s),
Release 73/73 (16.52 s), and reference-disabled ASan 36/36 (48.34 s,
`detect_leaks=0`). Each native execution linkage variant passed 151 cases and
80,996 assertions with no skipped cases. GNU make's new benchmark prerequisite
completed a fresh CXX-only, tests-off/reference-off build. That executable passed
32 quick workload/engine/entry execution cases and eight 1,000-function
compilation-only cases across O0–O3, plus an invalid-option diagnostic. A fresh
installed CXX-only static/shared consumer passed 3/3 tests; the no-tests product
build passed with 102 build commands and zero C source compile commands. New
test/benchmark sources passed x86_64 syntax checking only, not native execution
acceptance. Compilation used two jobs, ordinary CTest eight, and ASan four;
no Rosetta was used. Logs use `/tmp/mirnext-classic-*`.

### Original corpus execution without the C driver

`tests/Execution/OriginalCorpus.cpp` now runs the original `test1`–`test16.mir`
programs through owning C++ sessions. It is part of both static/shared native
execution suites and remains enabled when the C reference is off. Previously,
native tests roundtripped these files, while their dedicated execution driver
used C objects and mostly printed results without asserting them.

The replacement checks CSE/SCCP results, mixed argument calls, unsigned floating
conversions, dynamic stack allocation, long-double arithmetic and conversions,
multiple returns, expression initialization, switch results, block copies and
variadic block arguments. Host services capture format strings and typed values;
abort calls are failures, and the final exit calls in programs 8–10 record their
codes instead of terminating the test process. Raw/optimized interpretation,
eager/lazy JIT, BBV and both mixed interpreter/BBV directions run at O0–O3.
Double/long-double variadic observations use equivalent ABI bits where the
original copy pass aliases those representations, retaining strict distinct
types on extended-long-double platforms. Windows multiple-result native calls
still require the existing explicit diagnostic; interpreted execution remains.

Program 3 intentionally does not terminate. All levels compile native code and
verify the owning artifact; raw/optimized interpretation must exhaust its
instruction budget. Program 5's returned stack pointer is checked for a nonzero
value and never dereferenced after return. Program 10 keeps the original million
iterations in eager JIT; the other execution modes use a cloned 32-iteration
bound with the same inlining and stack-scope edges. Source files remain unchanged.

The old `mir-tests/run-test.c` and its sole-use `test-read.h` are deleted (114 C
lines), along with their two build targets and 25 CTest registrations. GNU make's
old file-corpus aliases share one CMake-built native corpus invocation. This
reduces local full CTest registration count from 108 to 83; it does not remove
the original files or their independent C format/pass/machine-stage comparisons.
The architecture guard rejects reintroducing the retired driver. The remaining
C oracle and other standalone C benchmark/utility drivers still await retirement.

Validation on native Apple M1 arm64: Debug full CTest passed 83/83 (91.19 s),
Release 83/83 (24.47 s), and reference-disabled ASan 36/36 (48.79 s,
`detect_leaks=0`). Both native execution linkage variants passed 147 cases and
78,074 assertions with no skipped cases; the focused original corpus passed
11,532 assertions. The no-tests product build passed and its 102 build commands
contain no C source compilation. A fresh installed CXX-only consumer passed
3/3 tests for static/shared execution and mixed-linkage rejection. Compilation
used two jobs, ordinary CTest eight, and ASan four. The new corpus source also
passed x86_64 syntax checking; this is not native x86 execution acceptance, and
no Rosetta was used. GNU make aliases were checked with a shared-prerequisite
dry run. Logs use `/tmp/mirnext-corpus-retirement-*`.

### Allocation-free binary name lookup

The native binary writer now uses heterogeneous string-table lookup. A typed
terminated-name view compares the implicit trailing NUL directly against owned
wire strings, so repeated register/symbol references do not allocate temporary
strings. Literal lookups also borrow their bytes. The existing single owning
table and insertion order remain authoritative; a name and a literal with the
same terminated bytes retain one wire identity. Unterminated literals, empty
strings and extra trailing NULs remain distinct. No format numbers or IR objects
change, and no C implementation is involved.

`mirnext_serialization_benchmark` measures raw binary writing with repeated or
distinct destinations and short or long identifiers. It checks native decode
and canonical re-encoding before timing. Five alternating before/after runs on
Apple M1 arm64 produced these medians for 65,536 instruction pairs:

| Identifiers | Destinations | Before | After |
| --- | --- | --- | --- |
| Short | Repeated | 12.09 ms | 10.50 ms |
| Short | Distinct | 47.34 ms | 46.07 ms |
| Long | Repeated | 22.40 ms | 10.34 ms |
| Long | Distinct | 78.59 ms | 65.76 ms |

All eight input sizes/workloads retained their byte counts and fingerprints.
Timing is informational, not a test threshold. Native static/shared regressions
cover both name/literal insertion orders, common prefixes, empty bytes and
single/double trailing NULs in raw and compressed formats. Benchmark logs use
`/tmp/mirnext-serialization-{before,after}-*.log`.

Native arm64 validation passed full Debug CTest 108/108 (97.11 s), Release
108/108 (39.38 s), and C-reference-disabled ASan 36/36 (44.79 s,
`detect_leaks=0`). Strengthened wire-table assertions also passed the final
Debug static/shared binary targets. The no-tests build has 102 commands and
zero C source commands; installed static/shared CXX-only consumers passed 3/3.
Both updated translation units pass x86_64 syntax
checking, which is not native x86 execution acceptance. Compilation used two
jobs; ordinary CTest used eight and ASan four. No Rosetta was used.

### Hardware-bound registers and addressable copies

`ADDR`, `ADDR8`, `ADDR16` and `ADDR32` require an ordinary local source. A
hardware-bound register has no addressable memory representation. Previously,
raw interpretation exposed its shared register bank as memory, while simplify
inserted an implicit copy and JIT wrote only that copy. This made the same
program observe different hardware values across execution modes.

Construction, rewriting and both native readers now reject that ambiguous input
with a diagnostic requesting an explicit ordinary local copy. Failed Builder
operations publish neither a temporary register nor an instruction; rejected
rewrites preserve revisions. Storage materialization and private machine IR
also reject combinations of hardware binding and memory storage. This is an
intentional checked-input contract change from the original C implicit-copy
behavior; ordinary hardware assignments and call effects retain their semantics.

Copy the value with `builder.assign(copy, Operand(bound))`, then call
`builder.address_of(copy)`. Writes through the resulting pointer affect the
local copy. Native regressions exercise all four address widths, partial stores,
indirect calls, repeated execution, O0–O3, raw/optimized interpretation,
eager/lazy JIT, BBV and both mixed interpreter/BBV directions. The C stage
fixtures use the same explicit copy rather than relying on implicit semantics.

On native Apple M1 arm64, final Debug and Release CTest each passed 108/108;
C-reference-disabled ASan passed 36/36 with `detect_leaks=0`. The no-tests
static/shared library build and installed CXX-only consumers (3/3) passed.
Build parallelism remained two, ordinary
CTest eight and ASan four, without Rosetta. Logs use
`/tmp/mirnext-bound-address-*`.

### Per-definition execution engines

`ExecutionOptions::select_engine` returns a checked engine choice for each owned
optimized definition. Choices are immutable; aliases retain the definition's
identity and engine. Selection precedes symbol resolution and code publication,
and its captures are not retained after loading. Optimization and inlining still
run first, in the original pass order. A reported `ExecutionMode::Mixed` is not
an input default policy.

One stable loaded-function record owns the interpreted preparation or native
artifact/lazy destination/BBV runtime for a definition. All engines use the same
symbol image, data allocation, relocation and expression-initialization order.
Expression initializers invoke their selected definition through a checked ABI
adapter when needed; no second host copy of the source function is compiled.
Native-only programs avoid constructing the interpreter's address-to-callee index.
Per-function statistics and aggregate statistics retain the same records, without
counting prepared interpreted bodies or expression copies as native compilations.

Mixed sessions publish ordinary ABI entries for interpreted definitions and
protect native calls with the owning recovery service. Interpreter reentry uses
the active invocation, retaining instruction/depth budgets across native frames.
Typed callback errors unwind their C++ scopes normally. Hardware bindings share
a bank keyed by physical identity, transferred at interpreter/native boundaries
through typed ABI frames. Adapter compilation preserves the host ABI while MIR
callee effects retain their original optimizer/backend behavior.

An imported function handle in an interpreted or mixed session uses checked
interpreter dispatch, preferring its typed callback and enforcing foreign-call
policy. Native MIR callers use the resolved ABI address, or an automatically
generated entry for a callback-only import. A wholly native session retains its
ordinary ABI import behavior. All-interpreted selection works without native call
services even when the default policy was JIT.
Callback-only variadic and jump-return imports require a real native ABI address
for native exposure; they cannot be installed as virtual pointer tokens in native
code. In particular the jump-import check runs before lazy code can resolve such
a token as a jump destination.

`JCALL/JRET` retain their nonstandard continuation convention. Native eager/lazy/
BBV functions can jump-call one another, and interpreted functions can jump-call
interpreted functions. Direct interpreter/native jump edges fail during load;
indirect edges report the same diagnostic during invocation. Interpreted jump
definitions exposed to native code have immutable rejection gates, initialized
to their cold stub according to the block-entry protocol. A regression caught
an initially null destination that branched to zero before reaching the resolver.
The resolver now reports the diagnostic and uses the native recovery checkpoint.

`tests/Execution/MixedEngines.cpp` covers all engine pairs and O0–O3, block/RBLK,
variadics, all five block classes with by-value isolation, hardware transfers,
cross-module aliases/data and retained handles, callback precedence and retry,
recursive budgets, jump conventions and label relocations. Fixed-seed loops
compare results and global bytes against the unoptimized owning interpreter.
The ABI fixtures use writable function-pointer slots and assert that an indirect
CALL survives optimization. Direct calls alone could be inlined at every level,
which would not establish an interpreter/native ABI crossing.
Raw hardware-binding entries are invoked through an explicit host-preserving
checkpoint. Calling them directly from C++ can overwrite a host callee-save,
including the register used by an ASan-instrumented caller's stack bookkeeping.
The mixed example uses only DSL and typed function handles; installed static and
shared consumers and the CXX-only subproject exercise the same path.
The independent C execution suite also runs its call, data and stack-scope corpus
with interpreted entries/native lazy callees and native BBV entries/interpreted
callees at O0–O3. These use the same C interpreter/JIT/BBV expected values as the
uniform-engine comparisons, including repeated writes and expression initializers.

Native Apple M1 arm64 validation for this increment:

- Debug full CTest: 127/127, 161.78 s; Release full CTest: 127/127, 40.94 s.
  The strengthened indirect-ABI fixtures and final raw-host preservation test
  additionally passed the static/shared execution suites in both configurations.
- C-reference-disabled ASan full CTest: 40/40, 54.78 s, with
  `ASAN_OPTIONS=detect_leaks=0`. Static and shared execution each passed 141/141
  cases and 36,852 assertions without skips; the owning interpreter suites each
  passed 20/20 cases and 10,013 assertions.
- The no-tests/no-examples/no-C-reference Release build succeeded. Its installed
  package was renamed before configuring consumers; the staging path no longer
  existed. Installed Debug and relocated Release consumers each passed 3/3,
  including static/shared mixed calls and the incompatible-linkage rejection.
- The CXX-only subproject passed 6/6, including both mixed-engine examples.
  Its actual Ninja `all` dependency graph contained 114 commands and no old
  core, runtime adapters or C engine objects. Installed consumers were configured
  with a deliberately nonexistent C compiler and never enabled the C language.
- Every compilation used two jobs; no Rosetta or foreign execution was used.
  Architecture dependency and whitespace checks passed.

Logs use `/tmp/mirnext-engines-*.log`. Native x86_64/SysV/Windows acceptance and
deletion of the old runtime/reference remain; these local results establish
the Apple arm64 increment only.

### Shared execution libraries

Every replacement layer now builds static and shared libraries from the same
object library. The two target graphs retain the same layer dependencies;
`ExecutionShared` reaches only shared IR, analysis, transformations, target,
code-generation and format libraries. The object targets have position-independent
code and the same no-exceptions/no-RTTI flags. No core source is compiled twice
to provide the two linkage variants. Exported CMake targets propagate a compatible
linkage property, so linking static and shared layers into one consumer is rejected
before building. This prevents duplicate object identities and runtime TLS state.

Installed DSOs use relative dependency search paths (`@loader_path` on macOS,
`$ORIGIN` on ELF systems). Windows exports the C++ symbols from each DLL and the
consumer test adds the installed DLL directory to its test environment. The
installed package remains CXX-only and carries no C engine dependency.

The owning execution regression sources are reused for static and shared tests,
including lazy compilation, BBV, native recovery, hardware effects, raw varargs
and nested callbacks. The interpreter suite also runs against both variants.
Both external consumers now exercise the complete Execution interface; previously
the shared consumer covered only serialization. They validate every installed
layer's type and dependency variant and reject mixed-linkage graphs explicitly.
The `add_subdirectory` consumer now uses native DSL/session examples and the same
static/shared layer targets. Its excluded subdirectory builds only requested
dependencies, leaving all old compatibility objects outside the consumer build.

Native Apple M1 arm64 Debug and Release each passed 126/126 CTest entries.
The C-reference-disabled ASan build passed 40/40 with `detect_leaks=0`.
In all three configurations the shared execution suite passed 126/126 cases
and 25,585 assertions; shared interpretation passed 20/20 cases and 10,013
assertions, with no skipped cases. Debug external consumers passed 3/3 checks.
A tests/examples/C-reference-disabled Release build was installed, its entire
prefix renamed, and a fresh CXX-only consumer passed 3/3 checks against the moved
package. Both variants disabled exceptions and RTTI and used a deliberately
unavailable C compiler. The fresh `add_subdirectory` consumer passed 4/4 checks
with the same flags; its parent requested C++11 and inherited C++20 from the
native targets. The actual Ninja command graph contains no old compatibility
objects, runtime sources or entry adapters. Architecture and whitespace checks
passed. Compilation used two jobs without Rosetta. Logs use
`/tmp/mirnext-shared-execution-*.log`. Native x86 acceptance remains pending;
these arm64 results do not establish Windows DLL or ELF execution parity.

### Owning BBV preparation, property identities and specialization

`CodeGen/BlockVersioning.hpp` now owns the preparation and selection side of
BBV. `prepare_machine_function(..., MachinePreparationKind::BlockVersioning)`
runs the same lowering, allocation, rewriting, post-allocation and frame passes
as whole-function compilation, retaining property operations and stopping before
encoding. Its move-only result owns both the machine function and frame facts;
it retains neither source IR nor compilation observers. Whole-function encoding
rejects unresolved property instructions instead of silently emitting them.

`BBVPreparation` partitions that one framed body into stable instruction ranges,
label anchors and successors. Unlabelled containers do not introduce flat MIR
boundaries. Consecutive labels share one address, calls remain inside a block,
and switch destinations retain order and duplicates. LADDR successors preserve
their position in the instruction sequence. The owning layout and machine body
remain stable when their owner moves or the source Context is destroyed.

The new frame oracle exposed an entry-anchor difference hidden by ordinary byte
comparisons: incoming ABI copies precede MIR's synthetic entry label, and combine
removes that label when it lies inside the allocation block. Machine instructions
now track incoming setup separately from call setup. At O0, frame formation
exposes the real anchor after incoming copies; combine follows the original
label-removal rule at O1–O3. No instruction is reordered. Only the test adapter
collapses the C CFG's empty nodes for consecutive label aliases, preserving edge
multiplicity and incoming/outgoing order; the flat BBV layout is compared exactly.

GVN memory equivalence classes now survive SSA teardown as function-owned
`ir::MemoryLocation` identities. Rewriter annotation edits update revisions and
SSA phase state. Cloning remaps those identities, and machine preparation maps
them into independent `MachineMemoryLocation` objects. Lowering, address
combination and allocation retain them by copying typed memory operands. These
annotations do not affect operand value equality and are not serialized. No
final physical address is used to guess an optimizer memory class.

`BBVProgram` owns versions, incoming property snapshots and immutable emission
plans. Its selection policy deliberately matches `get_bb_version`: the first
request fixes a block's properties and call-entry flag; later requests reuse that
same version. Specialization removes USE/PRSET from emission, folds property
branches, propagates move properties, applies alias/nonalias exclusions on stores,
and retains properties at each LADDR rather than substituting block-exit facts.
Calls clear memory facts only when terminal, following the original rule. A known
source register with no property leaves the destination's old property unchanged.
Plans borrow the one owned machine body and do not mutate its frame or identities.

Cloning copies only referenced memory classes, keeping repeated optimization
from accumulating dead provenance objects. Sparse memory identities are stored
in ordered maps, avoiding the original C
alias loop's compact-array indexing defect. Property validation scans existing
ordered snapshots without allocating or sorting them on every version request.
PRSET narrows a nonzero immediate to uint32; branch comparison retains all 64
immediate bits. A nonzero immediate whose low 32 bits are zero is retained as a
present zero-valued property. This defines the otherwise valid input for which
the original debug C `set_spot2attr` assertion fails; it is tested independently,
not reported as passing C debug parity.

Independent tests compare C property-preserving framed instructions, operands,
CFG, target state and flat BBV boundaries at O0–O3. A test-only observation hook
inside the actual C BBV generator records selected instructions and incoming/
outgoing properties; lazy C execution is compared with the native plans for
both property branches and outcomes, including warm version reuse. Native tests
also cover every public opcode, source lifetime, owner moves, loops, sparse
memory alias exclusions, property widths, observer errors and repeated optimization.

This is not yet an executable BBV engine in `ExecutionSession`. Block-entry
register preservation, cold/warm redirects and owning lazy publication are
implemented below; invocation error recovery, loaded label-data relocation and session integration
remain to be migrated. The old
compatibility runtime's BBV implementation does not satisfy that remaining work.

### Owning BBV block encoding and successor relocation

`encode_bbv` consumes a checked version's immutable specialization plan directly.
The AArch64 and x86 encoders share their existing native instruction recipes and
symbol/pool machinery with whole-function encoding; there is no cloned machine
body, C instruction view or binary-reader handoff. Folded property branches emit
an ordinary jump, and fallthrough emits the original explicit inter-block jump.
x86 uses the original forced rel32 patterns, without whole-function shortening
or loop alignment. AArch64 reserves the original conditional expansion NOP.

`CompiledBlock` owns its bytes, frame facts, label correspondence, symbols, pools
and successor relocations. `BlockVersionId` retains a program identity and typed
ordinal independently of version and machine lifetimes. Artifacts contain no
borrowed instruction, version, label or property pointers. A block cannot be
passed as a `CompiledFunction` to the ordinary publication API: unresolved
successors must be supplied to its explicit relocation operation.

Successor fixups distinguish AArch64 branch26, branch19 and ADR byte offsets,
x86 relative32, and absolute switch-table addresses. They retain operand identity
even when one operand emits several fixups, and preserve duplicate switch cases.
Relocation is repeatable and leaves the artifact unchanged. Operand lookup uses
a per-plan index, and a validated ordinal cache resolves each distinct successor
once in linear expected time, including large switches. The immutable preparation
is verified at construction, with a constant-time phase check at each block
encoding instead of repeated whole-function verification. Missing symbols,
foreign identities, overlapping fields, invalid
opcodes, alignment, address overflow and branch range errors return diagnostics.

The original AArch64 near-LADDR encoding is correct for aligned labels: ADR's
immhi field and a conditional branch's word offset coincide when immlo is zero.
They differ on far targets. The C BBV rebasing helper attempts conditional-branch
expansion for both; the native typed ADR relocation rejects an out-of-range
address instead of replacing the following NOP with a branch. Positive/negative
ADR range boundaries are covered separately from far conditional expansion.

A test-only hook snapshots actual C BBV bytes before address rebasing. Tests
compare those bytes at O0–O3 for both property branch outcomes, scalar branches,
loops, duplicate switch cases, LADDR/indirect jumps, NaN comparisons, stack
allocation and overflow branches. Both encoders receive the same optimized IR
and actual C literal addresses. The test adapter canonicalizes the C input's
unused scale to 1 after linking, matching native machine preparation: the file
protocol omits this field without an index, but it otherwise changes AArch64's
semantically irrelevant S bit for a zero index. No output bytes are ignored.

Native tests encode every public opcode at all backend levels and execute
eagerly published fragment batches after destroying source IR, preparation and
version owners. They check loops, conditional fallthrough, switch tables and
LADDR/indirect control flow, plus symbol diagnostics and repeatable rebasing.
This establishes block encoding and relocation; the eager test batch does not
implement or stand in for register-preserving lazy BBV entries.

This increment was verified on native Apple M1 arm64: Debug and Release each
passed 123/123 CTest entries; the C-reference-disabled ASan build passed 38/38
with `detect_leaks=0`. The native machine suite passed all 175 cases without
skips. A tests/examples/reference-disabled Release build installed successfully;
the installed CXX-only no-exceptions/no-RTTI consumer passed 2/2, and the fresh
README example consumer passed 1/1 and printed 42. The consumer's shared case
covers SerializationShared; it does not claim a shared execution library.
Architecture/dependency checks passed. Compilation used two jobs throughout,
without Rosetta or foreign-target generation. Native x86 acceptance remains CI
work. Validation logs use `/tmp/mirnext-bbv-encoding-validation-*.log`.

### Immutable BBV entries and register-preserving cold resolution

`CodeGen/BlockEntry.hpp` defines a block-specific native entry boundary. Its
64-byte gate uses a stable destination slot initialized to the gate's cold stub
at offset 16. The warm path reads that slot and jumps without changing flags,
stack or usable registers. Compilation redirects the data slot only; executable
instructions remain immutable. Descriptor storage, updates and execution require
external serialization, consistently with ordinary lazy function entries.

The AArch64 resolver saves usable GP registers, all 128 bits of every vector,
FPCR, FPSR and NZCV in a 784-byte aligned frame. The x86 resolver protects the
128-byte SysV red zone before saving RFLAGS, aligns an arbitrary incoming block
stack, saves all 13 non-reserved GP registers and complete FXSAVE64 state, and
provides Windows shadow space. It remembers the exact original stack position;
restoration does not assume ordinary function-entry alignment. Pure frame tests
cover SysV and Windows without generating foreign code.

The C++ callback returns a published target, or zero on failure. Zero dispatches
to an explicit recovery continuation after restoring the original block state.
An internal block has already entered a frame and may have allocated dynamic
stack storage; an ordinary lazy function's zero-result RET is therefore invalid
here. This boundary does not itself implement session error unwinding. The owning
publication service below checks branch/address ranges; session integration still
needs a recovery continuation at each supported invocation boundary.

Native execution probes seed all usable integer registers, full vector bit
patterns, flags and dynamic stack positions, then jump into a gate from inside
a frame. Cold, warm, failed, retried and repeated warm entries must preserve the
same state. x86 probes additionally check live x87 values and every red-zone
word. Callbacks deliberately change the floating environment, vector values and
condition flags. Unlike the original C BBV wrappers, this boundary explicitly
preserves condition flags and the complete floating environment; its state
contract is tested independently rather than claiming byte-identical wrappers.

A separate O0–O3 test actually encodes and publishes selected BBV fragments
inside the cold callback, preserving stack contents across dispatches and
compiling each reached block once. Stable gates and published bytes remain
unchanged on subsequent calls. It uses a controlled graph and its explicit common
epilogue for test-only recovery, not a substitute for `ExecutionSession` BBV.

The probe also exposed the legacy frame assumption that only the last return
needs an epilogue. Native frame formation now restores every RET and JRET exit,
including direct unsimplified register IR and multiple special jump-return
targets. The normal unified-return pipeline retains its original instruction
order and moves the epilogue operands without copying or extra exit bookkeeping
allocation. O0–O3 tests execute both ordinary exits with dynamic stack storage;
machine tests check restoration at every special exit. This is a documented
correction to the C frame algorithm, not a change to ABI lowering.

Final validation on native Apple M1 arm64: Debug and Release each passed
123/123 CTest entries, including all 177 native machine cases and 95 owning
execution cases without skips. The C machine oracle passed 17/17 cases with
3,684,412 assertions. C-reference-disabled ASan passed 38/38 (`detect_leaks=0`).
The tests/examples/reference-disabled Release build installed successfully, and
its external CXX-only no-exceptions/no-RTTI consumer passed 2/2. That consumer's
shared case still covers SerializationShared, not shared execution. Architecture
and whitespace checks passed. Compilation used two jobs, without Rosetta or
foreign-target generation; native x86 execution acceptance remains CI work.
Logs use `/tmp/mirnext-bbv-block-entry-final-*.log`.

### Bounded code arenas and owning lazy BBV publication

`CodeArena` reserves one page-rounded native address range. Reservations pack
their slots densely at 16-byte alignment, but different reservations own disjoint
whole pages. A batch can publish before or after any other batch without changing
an already published batch's bytes. Protection and cache operations receive
only that batch's page range. Linux/Windows apply protection to that range;
Apple arm64 uses the existing per-thread JIT write-protection switch, which is
not a per-mapping permission. No allocation or user callback runs while that
switch permits writes, and only new slots are copied. Releasing the shared owner
releases the original whole allocation once. Published handles and unconsumed reservations retain it
after arena destruction. Failures keep their reservation for retry at the same
address; abandoned reservations deliberately do not reclaim monotonic space.
The ordinary `CodeMemory` batch path shares its layout and publication logic,
without an additional slot-vector allocation.

`Execution/detail/BBVRuntime.hpp` owns a complete prepared BBV program, fixed
storage for block descriptors, all stable entries, compiled artifacts and code.
Creating the runtime publishes the shared resolver and packed gate bank only.
Each gate starts at its own cold stub. Executing a cold gate encodes the actual
selected version, remembers its typed successors, reserves another page batch,
relocates to stable successor gates and publishes it. The destination slot changes
only after successful protection/cache publication. Warm execution reads that
slot directly. No original context, C object, compatibility array or callback
installation protocol participates. Descriptor storage is allocated once, rather
than once per block, and artifacts are cached across publication retries.

The first version of a block is still selected when its predecessor specializes;
future paths reuse that version. The runtime does not traverse or pre-specialize
the graph eagerly to predict capacity. Taking a machine label as a data root
selects empty initial properties, whereas LADDR retains its instruction's
snapshot. Addresses remain stable before and after compilation. The loaded
program's source-label/data relocation layer is still to be connected to this
service.

The arena budget is explicit and checked, with no independent-mapping fallback
on exhaustion. Every branch and ADR displacement is checked by typed relocation
before publication. A range no larger than one MiB ensures all aligned AArch64
ADR references inside that range fit; larger budgets still diagnose actual
out-of-range references instead of applying the C far-ADR bug. Branch26 and
x86 rel32 constraints likewise remain enforced. This does not silently replace
target instruction selection with unrestricted indirect branches.

Observer, symbol, relocation and platform errors leave entries cold, return a
function/target/stage diagnostic and retain reservations for retry. Reentrant
compilation of the same block is diagnosed. Cold callbacks report the error to
`ExecutionInvocation`, restore block state and enter the caller-supplied recovery
checkpoint. That checkpoint must work before the function prologue and at any
internal block. The service deliberately requires it; it is not yet enabled as
an `ExecutionSession` mode or exposed as a general checked function call.

Native tests execute cold branches, loops/backedges, switch tables, LADDR and
property joins at O0–O3, checking unused blocks stay cold, exactly-once warm
compilation, source destruction, stable addresses and typed foreign-handle errors.
ABI adapter tests cover block/RBLK arguments, variadics, builtins and multiple
results with the existing Windows restriction diagnosed. Fault injection checks
observer rejection, recursive compilation, unresolved symbols, each publication
failure, exact-address retries and budget exhaustion. Memory-provider traces
prove publication never touches a previous batch's page range.

The independent C tests now execute the actual owning runtime as well as the C
lazy BBV engine. They compare the cold compilation order, incoming/outgoing
properties, emitted opcodes, unrelocated block bytes, results and warm reuse,
including joins reached in either property order. Branches, loops, switches,
LADDR, NaN, dynamic stacks and overflow fixtures retain byte comparisons with
the same literal/symbol environment.

This increment passed native Apple M1 arm64 Debug and Release at 123/123 CTest
entries each, including all 177 machine, 102 owning execution and 12 code-memory
cases without skips. The C machine oracle passed 18/18 cases and 3,685,276
assertions. The C-reference-disabled ASan build passed 38/38 with
`detect_leaks=0`. A tests/examples/reference-disabled Release build installed;
the installed CXX-only no-exceptions/no-RTTI consumer passed 2/2, including native
arena publication/execution in the static case (the shared case remains format
only). The freshly extracted README example passed 1/1 and printed 42.
Architecture/dependency and whitespace checks passed. Compilation stayed at two
jobs, without Rosetta or foreign-target generation. Native x86 acceptance and
shared execution-library acceptance remain outstanding. Logs use
`/tmp/mirnext-bbv-runtime-validation-*.log`.

### Checked native invocation recovery

`CodeGen/InvocationEntry.hpp` emits an invocation shim and a failure continuation
for the native target. The private checkpoint records host SP, return state,
nonvolatile registers and floating state. AArch64 saves full vectors and FPCR/
FPSR; x86 uses FXSAVE64, including x87 state, and separately saves the native
nonvolatile integer registers. The x86 host contract includes BP even though it
is reserved for frame formation and absent from the allocator's callee-save
query. Windows preserves XMM6–15 and has an independent shadow-space layout.
Pure layout tests cover both x86 ABIs without generating foreign code.

`Execution/detail/Recovery.hpp` owns both immutable entries in one code batch.
Each checked invocation keeps its checkpoint on the host stack and installs a
nested thread-local activation. There is no checkpoint heap allocation or
per-call code generation. `NativeCalls` can use this service for its existing
two-pointer, IR-built ABI adapters; the ordinary eager/lazy path retains its
existing direct call. Scalar adapters also work through the checked shim.

A failed cold block resolver first returns normally through all compilation,
observer and publication C++ frames. Only then does its generated continuation
find the innermost checkpoint, recover the host stack/registers/floating state
and return to the checked caller, which reads `ExecutionInvocation`'s first
diagnostic. This does not use `setjmp`/`longjmp` or skip C++ destructors. A nested
checked call made inside a host callback recovers to that callback's own call
site; its local destructors run normally and the outer checkpoint remains
available. Normal successful calls preserve actual result registers and leave
the program's floating-environment effects observable. A failed invocation
restores the floating environment from its entry.

The service is a boundary for generated frames, not a general stack unwinder.
Every host-to-MIR reentry must establish its own checkpoint. Calling the failure
continuation without an activation is an internal ABI violation. The existing
raw entries and interpreter/host callback entry paths have not all been connected
to it, and source-label/data roots still need session integration. Consequently
this increment does not enable BBV in `ExecutionSession`.

Native tests inject errors before prologues and at named blocks after dynamic
stack allocation at O0–O3, check visible memory effects and diagnostics, then
retry the same stable entry successfully. They cover mixed scalar, block, RBLK,
variadic scalar/block and multiple-result adapters, source-owner destruction,
nested callback failures, first-error isolation and ordinary floating effects.
An independent raw machine probe deliberately overwrites host nonvolatile
registers and vectors, changes SP and destroys x87 state before recovery. It
compares the recovered stack, register/vector bit patterns and floating state,
including both legal x86 block stack alignments on native x86 hosts.

This increment added six native regression cases. Apple M1 arm64 Debug and
Release passed 123/123 CTest entries each; owning execution passed 108/108 cases
and 20,212 assertions without skips. The C machine oracle retained 18/18 cases
and 3,685,276 assertions. The C-reference-disabled ASan build passed 38/38 with
`detect_leaks=0`. The tests/examples/reference-disabled Release build installed,
and the installed CXX-only no-exceptions/no-RTTI consumer passed 2/2 (static
execution and shared serialization, not shared execution acceptance).
Architecture and whitespace checks passed. All compilation used two jobs,
without Rosetta or foreign-target generation. Logs use
`/tmp/mirnext-recovery-validation-*.log`; native x86 recovery execution remains
subject to its platform CI acceptance.

### Raw native invocation boundaries

`CodeGen/NativeBoundary.hpp` and `Execution/detail/NativeBoundary.hpp` now provide
owning native entry/return gates that retain the original argument-stack layout.
They forward unknown variadic arguments and exhausted register banks without a
signature-specific marshalling frame. A thread-local pool owns one checkpoint
per active nesting depth; warmed calls reuse these frames. Successful returns
preserve MIR hardware-register and floating-environment effects. Failed cold
compilation restores the innermost caller checkpoint, including nested host
callback reentry, without abandoning C++ callback destructors.

Native regression cases cover scalar and aggregate parameters/results, explicit
MIR BLK/RBLK contracts, scalar/block varargs, long double, dynamic stacks,
O0–O3 retry, and independent raw machine probes of hardware-register effects.
The new raw-boundary cases exposed a moved-from `ExecutableCode` query bug;
address/byte queries now return zero/empty for a moved-from handle.

The boundary-only increment initially passed Apple M1 arm64 Debug owning
execution with 114/114 cases and 21,428 assertions without skips. Its Release,
ASan and installation validation was subsequently included in the session
increment below. x86 emitters require native platform acceptance. These services
were not yet connected to `ExecutionSession` BBV at that increment. The following section
records its integration; per-function engine selection remains outstanding.
The preceding section records the earlier fully validated baseline.

### Owning BBV execution sessions

`LinkOptions::compilation = JITCompilation::BlockVersioning` now connects block
execution to the same owning loaded image used by eager/lazy JIT and the
interpreter. Each definition owns its prepared machine function, selected
versions, stable gates and bounded executable arena. Loading prepares machines
without encoding blocks. Only reached blocks are encoded and published; explicit
`FunctionHandle::compile()` compiles its call-entry block without eagerly
compiling the rest of the function. Specialization selects successor versions
using the original first-version policy.

Ordinary definition bindings, imported aliases, exports and function reference
data share native checkpoint gates. These retain unknown native variadic stacks
and preserve successful MIR register effects; failures recover the nearest
ordinary call boundary. JCALL/JRET retain their explicit continuation protocol
under the ordinary caller's checkpoint. Source labels map to stable block gates
for address, difference and displacement data relocations; loading label roots
does not encode them. An identity index built during preparation replaces
repeated scans of source/machine label lists during data relocation.

Expression initializers use `NativeCalls` with checked recovery and the same
function bindings, rather than separate host-body copies. Symbols, builtin
requirements, globals and relocations resolve through the existing loaded image.
No C object installation, runtime callback bridge or engine fallback is involved.
Source destruction and session destruction leave retained callable/native handles
valid. Machine-preparation observers remain separate from block observers, whose
borrowed version/artifact views are valid only during the callback. A rejected
block remains cold and retryable. Budget exhaustion returns a contextual error.

`FunctionHandle::block_statistics()` and `LinkedFunction::block_statistics()`
return copied per-function counts; session/link statistics aggregate block
attempts, published blocks, gate/code bytes and batches. A published call entry
counts as a compiled function. `PartiallyCompiled` distinguishes a function with
some published blocks; whole-function `artifact()` remains null for BBV even if
all blocks have eventually compiled.

Native tests exercise every call fixture at O0–O3, global/expression data,
cross-module aliases and recursive function pointers, distinct label address
differences, JCALL/JRET, dynamic-stack failure/retry at generic/scalar/raw entries,
nested host reentry, unknown raw varargs, arena exhaustion and fixed-seed loop
inputs against interpretation and eager/lazy JIT. The independent C execution
adapter now uses actual C BBV thunk entries, including property-branch execution
and shared call/data/stack-scope fixtures. It does not call full-function
`MIR_gen` when observing BBV behavior.

The hardware-call fixture confirms that O2/O3 retain the original reverse
allocation rewrite's preservation of live bound virtuals across calls; O0/O1
retain the physical callee's changed values. All three owning native strategies
are checked. The standalone C reference aborts on this fixture at
`process_bb_ranges`, `mir-gen.c:6833`, in both local Debug and Release. A native
debugger confirms the same implicit-global live-in inconsistency already
recorded for variadic calls. This particular fixture therefore does not claim a
successful numeric C execution comparison; native construction/lowering/execution
coverage remains active. No unrelated allocation decision was changed.

See `examples/bbv-session.cpp` for the public DSL/session example. Native x86
execution acceptance, per-function engine selection and old-core removal remain
outstanding. Shared execution-library acceptance was completed subsequently as
recorded in the shared-library section above.

This increment passed native Apple M1 arm64 Debug and Release at 124/124 CTest
entries each. Owning execution passed 126/126 cases and 25,585 assertions without
skips in Debug, Release and ASan. The independent C machine comparison retained
18/18 cases and 3,685,276 assertions; C execution comparisons, now including actual
BBV execution, passed 4/4 cases and 85,853 assertions. The C-reference-disabled
ASan build passed 38/38 CTest entries with `detect_leaks=0`. The tests/examples/
reference-disabled Release package built and installed. A fresh installed
CXX-only no-exceptions/no-RTTI consumer passed 2/2, covering static execution
(including session BBV) and shared serialization; this is not shared execution
acceptance. Compilation used two jobs, without Rosetta or foreign-target
generation. Logs use `/tmp/mirnext-session-bbv-validation-*.log`.

### Owning x86 instruction selection and operand legalization

`Target/X86Patterns.def` now declares all 729 original expanded x86 patterns
using canonical opcodes and a typed private aligned SSE move. The new matcher
consumes owning `MachineOperand` values directly. Constraints are decoded and
indexed by opcode at compile time, retaining declaration order without runtime
string parsing or a scan of unrelated instructions. The same declaration data
now drives the precompiled encoding recipes connected to the owning pipeline.

The matcher preserves tied operands, signed immediate widths, typed memory
widths, signed 32-bit displacements, SIB scales, fixed hardware registers, x87
storage, call tails and conservative short-branch bounds. Symbol and builtin
relocations only match full-width immediates until linking; no numeric address
is inferred. Metadata does not change equality of identical memory locations.
Private aligned moves carry two operands, with encoding operand positions
adjusted from the original three-operand UNSPEC representation.

`legalize_operands` now implements the x86 tied input/output reload sequence
and the original single-memory trial order, tied-memory retry and remaining
memory reloads over stable machine nodes. x87 values retain memory storage.
The previously documented fallback operand-alias correction is retained.
Pattern queries dispatch by target rather than being implemented by the
AArch64 selector. The subsequent owning ABI and allocation stages are described
below. Frame formation, encoding and the execution boundary remain; this does
not enable cross-target compilation or advertise x86 JIT support.

Native constraint tests exercise host-independent x86 rules even on arm64.
Fixed x86 physical-register rows and actual x86 legalization run only on native
x86 CI. A separate test adapter compares all expanded declaration rows, order
and encoding templates to the original table on every host. This declaration
comparison is not a substitute for the remaining x86 phase and execution
comparisons.

Native M1 validation passed Debug and Release full suites (122/122 each), the
C-reference-disabled ASan suite (38/38), a Release build without tests or C
reference, and a fresh installed CXX-only consumer (2/2). After the final malformed
memory-type guard and Windows x87 fixture correction, the focused Debug/Release
rule suite passed 9 cases and 5,867 assertions; the declaration oracle passed
2,921 assertions. ASan includes those final corrections. Architecture dependency
checks and `git diff --check` passed. All compilation used two jobs and native
arm64 binaries; no x86 execution or Rosetta validation is claimed.

### Owning x86 ABI lowering and allocation consumers

`Target/X86ABI.hpp` describes ordered SysV and Windows parameter/result locations,
independent versus shared register banks, Windows shadow space, all five block
classes, hidden return buffers and variadic XMM counts. These pure facts can be
tested on arm64 without creating a foreign target. `CodeGen/X86Lowering.cpp`
consumes them directly: parameter moves, stack/block copies, ordinary and jump
calls/returns, builtin expansion, variadic cursors, fixed division/shift registers,
floating comparison ordering and per-function target state use owning nodes.
Mutation order follows the retained implementation, including reverse prepend
groups and fixed call insertion anchors. This code is compiled on arm64, but
actual x86 lowering decisions still require native x86 phase comparison.
Windows retains its single-result restriction. The typed result planner currently
diagnoses exhaustion of either two-register FP/x87 bank. The C source instead
falls through to integer moves for those shapes; their validity and behavior have
not been established on native x86, so parity for them is not claimed.

Allocation rewriting and edge spill placement now use target-dependent stack
bases, signed displacements and reload registers. SysV variadic frames reserve
176 bytes; frame-pointer slots grow downward, while frameless slots grow upward.
Windows and SysV use their respective SSE temporaries. x87 values occupy two
slots and remain memory operands. Signed displacement overflow is diagnosed
before an invalid x86 memory operand can reach encoding.

Large copied block arguments retain a typed `CallArgumentDependency` after
allocation. It preserves the original virtual value's liveness identity without
leaving a virtual register in a machine address. Dependencies belong to their
call and function, are checked atomically, survive instruction rewriting, and
participate in liveness/death metadata. The encoder consumes only ordinary
allocated locations. The independent C adapter normalizes this representation
while retaining value identity and def-use information.

Post-allocation combine uses the owning target pattern matcher for extension
folding, producer retargeting and operand substitution; DCE protects each target's
stack/frame registers. The virtual dependency index is allocated only for
functions that need it. Register-effect traversal visits sorted dependencies in
linear time rather than rescanning them for every operand.

Native x86 tests now cover the stages through post-allocation DCE, with separate
C snapshots for lowering, legalization, coalescing, ranges, assignment, rewriting,
edge placement, combine and DCE. They include ABI boundaries, raw combine rules,
seeded pressure and critical edges. Corresponding native hosts must run these
tests; arm64 compilation and pure ABI tests do not establish x86 execution parity.

M1 validation of this integration passed Debug/Release full suites (122/122 each),
the C-reference-disabled ASan suite (38/38), and a C-reference-disabled Release
build without tests. The native machine suite passed 135 cases and 99,175
assertions. A fresh installed CXX-only, no-exceptions/no-RTTI consumer passed
static/shared tests (2/2) with an unusable C compiler setting. The updated machine
example also passed its focused Debug test after the final example change.
Architecture dependency checks and `git diff --check` passed. Compilation used
two jobs throughout; no Rosetta or native x86 execution was used.

### Canonical machine preparation

`mirnext/CodeGen/MachineIR.hpp` and `MIRNext::CodeGen` now implement the direct
preparation boundary from canonical register IR. `MachineFunction` owns its blocks;
each block owns a single list of stable `MachineInstruction` nodes. Checked
`MachineRewriter` insertion, replacement, removal and cross-block splicing update
the function revision. Splicing preserves instruction and operand addresses.
Registers, labels and stack slots are noncopyable owned identities; virtual
register references, physical registers, stack slots, memory addresses, symbol
relocations and builtins occupy separate operand alternatives. Register categories
are never decoded from ranges of otherwise interchangeable integers.

Preparation copies signatures, strings, memory alias annotations and qualified
symbol identities directly, including displacement and block ABI information.
There is no binary/text intermediate, borrowed public instruction, duplicated flat
body, C CFG installation or runtime callback. The result survives destruction of
both the Builder Context and the owning optimized program. Symbols remain typed
relocations rather than guessed numerical addresses; they do not copy global data
storage into each machine function. Anonymous relocation identities retain their
module ordinal. Hardware-bound aliases share a virtual register and ordinary
returns get explicit backend USE operands; memory-backed bindings do not acquire
a second hardware identity.

`TargetMachine` supplies the native ABI, scalar layout, register banks, reservation
and call-clobber rules, long-double canonicalization and checked binding lookup.
It supports the current native x86_64 SysV/Windows and aarch64 Linux/macOS host;
requesting another architecture returns a diagnostic. The independent C oracle
checks each physical register's type class, reservation and clobbers.

The machine CFG is derived from block ownership and label anchors. It preserves
duplicate switch edges, direct-edge order, delayed indirect-edge insertion,
layout-ordered address targets, and entry/exit boundaries. Revision checks reject
stale graph views. Independent preparation comparisons use the same optimized
input at O0–O3 and retain opcode/operand types, constant bits, symbol identities,
hardware bindings, and incoming/outgoing edge order. Only signature-prototype
names are compared structurally, since the portable writer may synthesize a
different name for the same signature. On hosts where long double is double, the
test loader also corrects the C scanner's noncanonical `ld` data directive; the
literal values are unchanged and instruction constant bits remain checked. This adaptation exists
only in the independent test executable.

`examples/native-machine-ir.cpp` demonstrates DSL construction, owning optimization,
machine preparation and CFG inspection after source destruction. This boundary
does not yet run target machinize, operand legalization, register allocation or
encoding on the new model. Those existing algorithms and runtime execution still
need migration before the default JIT can switch.

Preparation indexes register names, hardware bindings and owned symbol identities
instead of rescanning their owning vectors. CFG construction performs the one
complete machine verification; preparation no longer repeats it immediately
beforehand. `mirnext_machine_ir_benchmark` measures the complete preparation call,
including source validation and derived CFG construction, with source construction
and dump hashing outside the timed interval. The comparison is against the initial
linear-lookup version of this new model, not the old JIT or C engine.

Five alternating native Apple M1 Release before/after runs, each with seven
repetitions per size, produced these medians of run medians. Every run retained the
same instruction count and diagnostic-IR fingerprint:

| Referenced data symbols | Initial version | Indexed version | Instructions |
| ---: | ---: | ---: | ---: |
| 256 | 0.578 ms | 0.232 ms | 770 |
| 1,024 | 7.914 ms | 0.949 ms | 3,074 |
| 4,096 | 106.504 ms | 4.171 ms | 12,290 |

The large case removes about 96.1% of preparation time in this workload. This does
not measure machine lowering, generated-code speed or whole-JIT latency.

### Machine graph and register analyses

`mirnext/CodeGen/Analysis.hpp` analyzes `MachineFunction` directly. The graph
analysis retains ordered edges, iterative DFS numbering, back edges, disconnected
blocks and the original backend loop forest. Its loop membership index uses path
compression independently of the observable tree, and epoch marks replace a fresh
set for each candidate. Depth propagation visits the tree once rather than walking
every block's ancestors. A 512-level native loop case and a 4,096-block chain cover
deep structures without recursive traversal.

Register effects distinguish virtual and physical identities, preserve repeated
address operands and classify private USE and aligned SSE moves. Liveness consumes
these occurrences directly from the block-owned instructions; it does not project
them into another function body or install them into C containers. Its typed,
analysis-local register universe supplies gen/kill/in/out sets, original loop-weighted
frequencies, pressure and ordered last-use metadata. Compact scans keep their specified
order, including the original pressure heuristic that classifies compact bit numbers
in the full register universe. The original allocator's input classification for
LADDR is deliberately preserved and tested.

Machine functions own the module-wide physical binding set. A hardware declaration
affects calls and exits even in a different function of the same module; it is not
enough to keep only that function's virtual bindings. Preparation gathers the set
from the canonical module, and its lifetime is independent of that module. Analyses
borrow stable machine nodes, require the function to outlive them, and use revisions
to reject stale graphs and typed queries after mutation.

The independent C comparison now checks traversal numbers, loop parents/children,
all four live sets (including physical registers), integer/floating pressure,
per-register frequency, operand occurrences and last-use order. It covers O0-O3,
every public opcode, nested and irreducible control flow, address/inline fixtures,
fixed-seed programs and the MIR corpus. The C test hook resets empty-boundary pressure
before its fresh frequency-enabled run: the enclosing oracle has already run one
analysis, and otherwise the retained empty-block pressure would be counted twice.
The loader also attaches label-reference data to its C functions, the step normally
performed during module loading. It does not load the module or rerun simplify on
already optimized instructions. This preserves the same address-root environment
and catches adjacent-label/indirect-edge cases rather than dropping their coverage.
These are test isolation steps, not changes to the original product algorithm. Compact
scan boundaries and explicit physical call masks also have native cases; the
comparison above uses the complete register scan before ABI lowering.

`mirnext_machine_analysis_benchmark` measures frequency/pressure/death analysis on a
straight-line function whose repeated definitions leave almost all instructions
without last-use metadata. Function/CFG construction and result hashing are outside
the timed interval. Only nonempty death vectors now acquire a map node. Five
alternating native Apple M1 Release pairs, seven repetitions per size, gave these
medians of run medians; ordinary `operator new` calls are counted only during analysis:

| Instructions | Entry for every instruction | Entries only for nonempty deaths | Allocations before / after |
| ---: | ---: | ---: | ---: |
| 32,769 | 2.359 ms | 0.979 ms | 32,881 / 99 |
| 131,073 | 11.497 ms | 4.514 ms | 131,187 / 99 |
| 524,289 | 88.175 ms | 16.272 ms | 524,405 / 99 |

All pairs retain identical frequency, live-set, pressure and death-count fingerprints.
This isolates the new analysis's metadata representation; it does not measure the
old C implementation, total JIT latency or generated-code speed. The owning
machine pipeline and its current execution boundary are described below.

### Owning AArch64 target lowering

`mirnext/CodeGen/Lowering.hpp` exposes `codegen::machinize(MachineFunction&)`.
The AArch64 implementation rewrites these owning nodes directly, with no old
function projection, generator installation or C call. The owning x86 implementation
uses the same lowering entry with its own ABI rules; this API has no fallback.

The port retains the Linux/macOS branches for incoming scalar and block arguments,
hidden RBLK in r8, small aggregate save areas, register exhaustion, aligned outgoing
stack areas, builtin copies above sixteen words, variadic calls/extraction/start/end,
multiple results, narrow extensions, long-double builtins and NaN-safe floating
branches. Large incoming and outgoing offsets use the original r10/address-temporary
rules. USE remains an internal liveness boundary. Instruction identities remain
stable across insertion and replacement; previous/next navigation uses the stored
list position instead of rescanning a block for each call.

Lowered calls carry an explicit checked state: results must be physical registers
and arguments physical registers or ABI memory locations. Ordinary public block
operands retain their stricter pre-lowering rules. Builtin callee references are
typed, and their prototype identities are separate from user symbols even if the
module and spelling coincide. A second lowering invocation is diagnosed. Preflight
rejects unsimplified operands, unsupported result counts and unpromoted variadic
floats before rewriting. Both outgoing layout walks also check combined-size and
alignment overflow before emitting any block copy. The function owns its lowered state independently of both
the Builder and optimized program.

The isolated C hooks duplicate the same already optimized input and run either
original target machinize or machinize followed by operand legalization.
Comparisons retain matching decisions, instruction order, physical
registers, constants, memory offsets/types, signature and symbol identities, ordered
CFG edges, call-register sets and function state; virtual IDs and label IDs are
normalized. Scalar prototype size fields are not read: the original builtin
constructor leaves those unused fields uninitialized. This is an oracle adaptation,
not a new native representation of that undefined state.

Native and independent tests cover every public opcode at O0-O3, 77 ABI fixtures,
the sixteen original MIR files, inline/address/label cases, fixed-seed CFGs and a
4,110-argument boundary that exceeds scaled memory offsets. Native-only tests also
check atomic diagnostics, typed builtin identities, repeat invocation and source
destruction. Local comparison is on native macOS arm64; the sixteen-byte long-double
and Linux variadic branches require the native Linux arm64 CI run. This stage does
not include register allocation, frame generation or encoding,
and does not yet execute the new machine model. The installed static consumer and
machine example exercise lowering, operand legalization, coalescing and subsequent
graph/liveness and live-range analysis on arm64.

### Owning AArch64 operand legalization

`codegen::legalize_operands(MachineFunction&)` requires completed machinization
and records its own checked phase state. It directly rewrites the owning machine
nodes, reuses failed trial registers by scalar type, and preserves the original
one-memory trial order, all-memory fallback and before/after reload ordering.
AArch64 has no tied input/output instruction constraints. x86_64 legalization
remains outstanding and returns a diagnostic. Inserted reloads are not visited a
second time. A failed speculative operand change is tested using an operand buffer
before the checked rewriter commits it, avoiding needless validation and revisions.

`has_instruction_pattern` matches typed operands against the target declarations.
`Target/AArch64Patterns.def` is the single source for constraints and encoding
templates, consumed by the new matcher, earlier encoder and independent C target
through selector adapters. The native matcher builds its opcode index and decoded
constraints at compile time; no C matching function, runtime initialization or
middle-end addressing approximation participates. Unresolved symbol relocations
retain typed identities and reserve a full four-chunk move. Overflow branches use
the same one-label legality constraint regardless of their eventual flag encoding.
Negative shift constants produce a non-match instead of triggering the C assertion.

Native tests instantiate every shared pattern and cover immediate/shift/address
boundaries, floating zero and NaN, unresolved relocations, multi-memory reloads,
alias preservation, scalar temporary reuse, stale analyses and invalid phases.
Independent snapshots compare matching decisions and both stages for the same
optimized O0-O3 input. A separate named regression records an original C fallback
defect: it takes an operand pointer, overwrites that operand with a register, then
uses the pointer to emit a self move. With three memory ADD operands it loses two
loads and a store. The new implementation copies the memory operand before
replacement; the independent regression explicitly checks the three original
self moves against the three preserved native memory accesses at all levels.
The C oracle is intentionally unchanged for this case; this difference is not
hidden by excluding the input from coverage.

`mirnext_machine_patterns_benchmark` excludes construction and checks 65,536
instructions with register, memory and immediate operands. Five alternating native
M1 Release runs, each taking the median of nine repetitions, retained fingerprint
`15828787558110245953`. The median of run medians changed from 8.817 ms for the
initial linear/string matcher to 2.108 ms with the compiled constraint/index table
(4.18x). This measures the new matcher's local refinement, not C-versus-C++ or
end-to-end compilation speed. It does not alter pattern priority or rewrite rules.

### Owning coalescing and live ranges

`mirnext/CodeGen/RegisterAllocation.hpp` exposes `coalesce_registers` and
`MachineLiveRanges::build`. They operate directly on `MachineFunction` blocks and
instructions, without projecting a second body, C CFG or register-number ABI.
Analysis-local indices map typed virtual and physical registers to bitmap positions.
Results borrow stable node identities and reject stale or foreign queries after
function mutation. The function must outlive these analysis views.

Aggressive coalescing retains the original O2/O3 condition and strict 10,000-register
scan threshold. Moves are weighted by loop depth and tied by stable instruction
creation order, even when layout has changed. Interference excludes the move's
source, merged groups check every member, and an existing hardware binding wins
over an unbound representative. Distinct hardware bindings cannot merge. The checked
rewriter substitutes register and memory address uses, deletes redundant moves and
creates an anchor when deletion would leave an empty unlabelled block.

Live-range construction retains the original O0 layout order, O1 reverse traversal
and O2/O3 order, including unused definitions, physical call clobbers/arguments,
early clobbers, call-crossed values and whole-block references. It owns raw and
compressed ranges, the compression map and spill gen/kill sets. Compact scans retain
their own typed register universe; no magic numeric interval distinguishes physical
from virtual registers. Fresh graph/liveness analyses follow coalescing mutations.

The isolated C comparison includes machinize, operand legalization, coalescing and
live-range construction on the same optimized input. It compares
ordered machine instructions and CFG, matching decisions, physical identities,
signature/constant bits and function state. The range stage also compares raw and
compressed intervals, ordered whole-block references, the point map, call-crossed
registers and spill sets. Coverage retains all public opcodes, ABI fixtures, original
regressions and seeded CFGs at O0–O3. Ten native allocation-preparation cases cover
conflicts and merged groups, hardware identities, empty anchors, scan boundaries,
stale/foreign queries, creation order, calls, early clobbers, whole-block merging and
packed-matrix symmetry across machine-word boundaries. Public-opcode and ABI cases
exercise the same stages with the C reference disabled.

The coalescer stores only the symmetric conflict matrix's upper triangle, uses a
direct local index inside group comparisons, and skips operand buffers for unchanged
instructions. `mirnext_coalescing_benchmark` excludes construction and CFG analysis;
it times coalescing and checks the final two-instruction body and representative
fingerprint. Five alternating native M1 Release runs, each taking the median of
seven repetitions, produced these medians of run medians:

| Copy-chain moves | Initial native implementation | Refined native implementation | Fingerprint |
| --- | --- | --- | --- |
| 256 | 0.357 ms | 0.232 ms | `6809664774356722732` |
| 1,024 | 4.072 ms | 1.970 ms | `16295358552873177388` |
| 4,096 | 84.255 ms | 25.951 ms | `4435754425942132012` |

The largest case is 3.25x faster in this local refinement; these are neither
C-versus-C++ nor whole-JIT timings. Coalescing decisions and ordering are unchanged.
Frames, encoding and runtime execution still require connection to this new model.

### Owning priority assignment

`MachineAllocation::assign(function, graph, liveness, ranges)` now consumes these
same machine objects and analyses. The result owns typed physical-register or
`StackLocation` values, priority order, live lengths, point occupancy, spill gen/kill
sets and whole-block gaps. Stack locations use byte offsets, sizes and alignment;
physical and stack occupancy have separate bit domains. No location number encodes
both meanings, and no C register-renumber array or allocation input projection is
involved. Gap records own their range value and borrow only stable machine register
and block identities, so the input analysis results can be destroyed. The function
must outlive the result; mutation makes checked location/length queries stale.

The port retains frequency/live-length/declaration priority, hardware-binding
precedence, call-clobbered preference, fixed-register exclusion, unique addressable
storage, aligned two-unit long-double storage and reusable spill slots. O0/O1 use
simple assignment; O2/O3 retain whole-block splitting, boundary spill costs, profit
and gap-size ties, and spilling either a previous value or the current value's gap.
Global hardware registers include the compilation environment supplied to liveness.
Compact or frequency-free analyses, mixed levels/environments, stale and foreign
objects return contextual diagnostics before assignment. Results do not mutate IR.

The original C priority comparator returns -1 in both directions for two hardware
bindings. The native result uses stable declaration order for those ties rather than
copying that invalid comparator; two-binding regression coverage checks this policy.
Checked spill-cost/profit arithmetic diagnoses overflow instead of copying signed
overflow. The earlier native allocator already used these corrections.

Independent C hooks compare assignment after range compression. Checks include
actual register/stack assignments, live lengths, complete
priority order, every used/busy point, selected hardware, spill sets and ordered gaps.
The existing public-opcode/ABI/regression corpus still runs at O0–O3. A seventh C
group adds eight deterministic 40-parameter programs with branches and loops, using
the same input without middle-end folding to preserve allocation pressure. Nine new
native assignment cases cover priority, unreferenced values, slot reuse/alignment,
addressable storage, hardware effects, both splitting directions at the exact
profitability boundary, compilation globals and invalid analysis combinations.
The installed CXX-only consumer and machine example now exercise assignment too.

Boundary costs for a whole-block range are cached within one allocation call across
candidate registers. The cache does not change candidate order, cost or profit.
`mirnext_assignment_benchmark` excludes construction and all analyses, retains 64
blocks, limits available integer registers and checks locations, spills and point
occupancy in its fingerprint. Five alternating M1 Release runs (median of seven
repetitions per run) produced these medians of run medians:

| Pressure registers | Initial native assignment | Cached boundary costs | Fingerprint |
| --- | --- | --- | --- |
| 128 | 0.279 ms | 0.266 ms | `16452243461603900358` |
| 512 | 1.462 ms | 1.411 ms | `1236638997876586950` |
| 1,024 | 4.494 ms | 4.293 ms | `13973989299353047494` |

These are small local timing differences, with run-to-run variation; they do not
establish C-versus-C++ or whole-JIT speedups. All result fingerprints match.
Assignment, operand rewrite and edge preservation now share this model;
it still has no end-to-end native JIT execution path.

### Owning allocation operand rewrite

`rewrite_allocated(function, graph, liveness, ranges, allocation)` consumes typed
allocation metadata and rewrites the instructions owned by each machine block.
It returns `MachineRewriteResult`, owning updated allocation metadata, preservation
slots and the live-in/gen/kill facts needed by the following edge-spill stage.
There is no second instruction body, C CFG installation or numeric register/stack
transport. Checked queries use the function revision; the result borrows stable
machine identities and must not outlive its function.

The AArch64 port retains O0/O1 forward and O2/O3 reverse traversal, two integer and
two floating reload temporaries, input/output reload reuse, direct memory matching,
large-offset address materialization, fully spilled indexed addresses, local
ADDR/ADDR8/ADDR16/ADDR32 and the 16/32-byte ABI frame header. O2/O3 retain call
preservation, reusable preservation slots, block-entry saves and global-register
exclusion. Removing a redundant move preserves a USE anchor where the original
backend requires a nonempty block. The final phase state rejects virtual operands
and addresses; stale, foreign, mixed-level/environment and unsupported-target
inputs return contextual diagnostics. Ordinary source IR and symbols are untouched.

Independent C snapshots now compare seven machine stages. The rewrite comparison
includes actual instructions, physical register identities, stack displacements,
ordered CFG edges, call-register sets, range/allocation metadata, preservation
slots and function state. All public-opcode/ABI/original fixtures and deterministic
branch/loop pressure programs reach this stage at O0–O3. Nine native regression
cases additionally cover stable nodes, reload/store order, all local address widths,
indexed address preloads, floating reload families, large offsets, empty anchors,
repeated call preservation and invalid inputs. Native public-opcode and ABI tests
also exercise rewrite with the C reference disabled. The installed CXX-only
consumer and machine example use this API.

This stage includes local/call preservation. Control-flow edge spill placement is
a separate native pass described below. Frames, encoding
and the execution session must still be connected before this object model can JIT.

No-op deletion checks the next list node to determine whether an unlabelled block
needs an anchor, rather than counting all remaining instructions on every deletion.
This makes a consecutive deletion run linear without changing deletion or anchor
decisions. `mirnext_register_rewrite_benchmark` times rewrite only, after machine
construction, liveness, ranges and assignment. It verifies deletion count, retained
instructions, stack area, creation identity and used hardware. Five alternating M1
Release runs (seven repetitions each) gave these medians of run medians:

| Consecutive no-op moves | Initial native rewrite | Constant-time anchor check | Fingerprint |
| --- | --- | --- | --- |
| 4,096 | 22.884 ms | 0.688 ms | `11081467936496871130` |
| 16,384 | 371.509 ms | 2.762 ms | `14052029658858718938` |
| 32,768 | 1,489.480 ms | 5.562 ms | `4598050238414259930` |

Fingerprints match in all runs. This deliberately stresses repeated no-op deletion;
it is neither a representative whole-JIT timing nor a comparison against C.

### Owning control-flow edge preservation

`split_allocated_spills(function, rewrite_result)` consumes the checked rewrite
result and returns `MachineSpillResult`: updated allocation and preservation slots,
placements and a CFG retaining the original incoming/outgoing edge order. O0/O1
keep the original no-split condition; O2/O3 retain collection order, common-boundary
selection, edge reuse, critical-edge splitting, deduplication and insertion order.
The AArch64 spill emitter is shared with operand rewrite, including large offsets,
frame headers and cache-slot reuse. Immutable live intervals are shared by the
allocation result, so destroying input analyses does not invalidate preservation.

Instructions and label anchors remain owned by machine blocks during edits.
Moving a jump or label retains its identity; aliases and duplicate switch targets
are kept distinct. Block insertion uses stored list positions, and incoming-edge
redirection uses list splices, avoiding repeated linear searches at high fan-in.
Short spill-emission sequences use fixed three-element storage. These changes
reduce traversal and allocation work; no whole-JIT speedup is claimed.

Checked CFG verification compares the retained topology with block-owned control
flow. Graph analysis can consume that verified topology without losing edge order.
Foreign/stale/forged graphs and unsupported critical indirect edges return diagnostics.
These APIs do not install data in a C CFG or call the reference backend.

Nine native regression cases cover reuse, fallthrough, jump/label moves, aliases,
critical edges, repeated switch destinations, analysis lifetime and rejected edits.
The same actual-allocation fixtures run with C disabled and in an eighth C comparison
group: O0–O3 produce ten preservation placements and two critical split blocks in
total. Snapshots compare physical registers, stack offsets, instruction order,
incoming edges, allocation occupancy and preservation slots. Equal-priority fixed
bindings are sorted in snapshots because C qsort leaves their order unspecified;
free-register allocation order remains checked. The machine example and installed
CXX-only consumer now run this stage. Frame formation, encoding and the current
runtime integration limits are described below.

### Owning post-allocation optimization

`optimize_allocated_machine(function, spill_result, options)` consumes the checked
edge-preservation result and edits its block-owned instructions. O0 strips
properties for full-function codegen; O1 then runs combine and DCE. O2/O3 strip
properties during combine, then run DCE. `remove_properties=false` preserves the
original BBV convention. No C objects, installation callbacks or instruction-body
projection participate. `MachinePostResult` retains allocation/frame inputs and
returns a freshly derived, verified CFG; the machine function must outlive it.

Combine retains last-use guards, memory/call/stack barriers, source obsolescence,
producer movement, extension widths and signedness, commutative opcode selection,
target pattern priority and repeated block scans. Candidate operands are matched
without editing the instruction; successful changes go through `MachineRewriter`.
Producer moves retain creation identity. Deleted definitions transfer filtered
death markers before destruction; required deleted definitions return diagnostics.
The model cannot contain interior instruction labels: all label anchors are owned
at block entries, including multiple aliases.

DCE retains memory stores, calls, dynamic stack/variadic effects, FP/SP writes,
module hardware effects, call argument masks and early clobbers. Overflow producers
remain when a BO/BNO/UBO/UBNO is reachable through exactly MOV/EXT32/UEXT32 nodes.
The reverse scan maintains that predicate over surviving instructions, including
deletions, avoiding repeated forward suffix scans. A sole dead instruction gets
an empty USE anchor only when the block has no label anchor.

The original backend intentionally retains conservative input edges after removing
property branches and uses pre-combine boundary liveness for DCE. This port freezes
that topology as analysis input rather than relabeling a stale graph as current.
Only the private post-allocation consumer can calculate liveness from it; public
analysis entry points still reject stale/foreign CFGs. Observer views explicitly
expose `input_cfg`, boundary facts and surviving death markers, separately from the
current function body. Result-returning observers run after stripping, liveness,
combine and DCE when those stages are enabled. Failures include function, target
and stage; the final phase is not marked complete on failure.

Fourteen new native tests cover positive and prohibited transformations, all 36
extension pairs, floating ordering, memory/call/stack barriers, early clobbers,
overflow guards, property modes, singleton anchors, source lifetime and diagnostics.
A ninth independent C group compares combine and DCE instructions, physical
registers, constant bits, target matching, frozen ordered CFGs, boundary sets and
death markers. It covers public opcodes, ABI fixtures, the original 16-file corpus,
actual edge spills and raw fixtures requiring substitutions, extension folding and
dead deletion. Seeded branch/loop pressure cases also reach both stages. These
native capabilities remain covered with C disabled. The installed consumer and
machine example now run the full post-allocation sequence. AArch64 session
execution was connected at this post-allocation increment. The x86 session
increment below adds that boundary; BBV remains outstanding.

`mirnext_post_allocation_benchmark` measures post-allocation analysis/combine/DCE
after construction, allocation, rewrite and edge preservation. It deliberately
places dead overflow producers before a surviving FP-write MOV suffix: the
original forward search repeatedly traverses that suffix. Five alternating native
M1 Release runs, each taking the median of seven repetitions, gave these medians
of run medians:

| Dead producers / surviving MOVs | Native forward search | Reverse frontier | Fingerprint |
| --- | --- | --- | --- |
| 2,048 / 2,048 | 11.372 ms | 1.345 ms | `1040557949555039545` |
| 8,192 / 8,192 | 131.349 ms | 5.465 ms | `12300214597370527033` |
| 16,384 / 16,384 | 473.730 ms | 11.421 ms | `10217609926294502713` |

Every result retains the same instruction identities, physical operands, spill
area and deletion count. This isolates a repeated-suffix stress case; it is not
an end-to-end JIT timing or a comparison against the C implementation.

### Owning AArch64 frame formation

`form_frame(function, post_result, options)` consumes checked post-allocation
state, retaining its typed physical-register usage and byte-addressed spill area.
`MachineFrameResult` contains the current verified CFG, FP-relative frame size,
separate aggregate area, variadic/spill bytes, typed callee-save offsets and inserted instruction
identities. The machine function owns every instruction and must outlive the
result. `preserve_host_registers` additionally saves the host ABI's nonvolatile,
allocatable registers for wrappers whose MIR bindings can clobber them.
The final result does not retain the consumed allocation/live-range/occupied-bit
arrays or spill analysis. Those inputs can be released at this phase boundary
when no earlier observer/caller holds them; encoding receives only frame facts
and the actual CFG, rather than a chain of obsolete analysis results.

The AArch64 rules retain the original empty-leaf decision, 16-byte alignment,
integer/vector save order, full 128-bit vector saves, dynamic-stack and block
argument conventions, macOS/Linux variadic differences, aggregate rounding and
JRET's link-register behavior. Large frame sizes and save offsets use the same
physical temporaries and target pattern constraints. Sizes and all candidate
instructions are checked before the function is changed. Prologues precede
entry labels: a separate unlabeled entry block is created only when required,
so branches to the old entry bypass stack allocation. The original final-return
selection is retained; nonreturning functions receive only a prologue.

Ten native cases exercise actual spill areas through 4,096 slots, leaf omission,
volatile/nonvolatile hardware, full vector saves, label backedges, host mode,
dynamic/variadic/block/aggregate state, multiple returns, stable identities,
source destruction and stale/foreign/oversized diagnostics. All public opcode
and ABI fixtures now reach frame formation with C disabled. A tenth independent
C group checks identical input and resulting instruction order, physical
registers, constant bits, offsets, labels and state at O0–O3, including actual
edge spills, the original MIR corpus and synthetic boundary/host frames. C's
retained input graph is compared separately from the freshly built result CFG.
The local comparison includes 848 nonempty frames and 7,972 callee-save entries;
the tests require actual frame generation and saves rather than empty snapshots.
This stage does not install instructions into a C CFG or call a C frame routine.
New-model AArch64 encoding is described below. Code publication and runtime
integration remain separate work.

### Owning x86 frame formation

`form_frame` also consumes the owning x86 post-allocation result. It creates an
ephemeral typed target sequence, checks every candidate with the native selector,
then inserts instructions through the same rewriter as AArch64. No instructions,
CFG or allocation arrays are installed into an old runtime. Callee-save offsets
are signed; `MachineFrameLayout::frame_bytes` is the actual SP subtraction and
`saves_frame_pointer` records the separate BP save. Save offsets use BP or SP
according to the target state. Entry anchors, backedges, final-return selection,
nonreturning functions and stable instruction identities retain their meaning.

The rules preserve the original leaf omission, frameless spill rounding, stack
alignment, SysV 176-byte variadic area, Windows argument homes, vector-before-GP
save order and host-preservation option. Windows vector saves preserve all 128
bits. `MachineFrameOptions::no_red_zone` and the corresponding compilation option
select allocation before storing the old BP. This explicit C++ option replaces
the old compile-time switch without exposing a legacy macro to the new core.

Three original defects have independent regressions in `tests/CodeGen/X86Frame.cpp`:

- Without a red zone, Windows variadic homes must use the computed new BP in
  R10 until BP is installed; using the caller's BP writes to the wrong frame.
- Without a red zone, FP-relative JRET frames require another 16 bytes below
  their lowest slot. JRET lacks the return-PC slot; the original subtraction
  leaves the lowest save eight bytes below SP, including nonempty frames.
- Windows JRET entry has no stacked return PC. Its FP-relative vector saves
  can be eight-byte aligned, requiring MOVDQU rather than MOVDQA. A distinct
  private `X86Mov128Unaligned` and three appended target patterns express this;
  the original 729 declaration rows and their ordering remain unchanged.

The C stage adapter records the MOVDQA/MOVDQU exception explicitly: raw snapshots
must differ, and changing only these move opcodes must make them identical.
It does not normalize away operand, offset, ordering or state differences.
The two no-red-zone defects are tested with explicit register/memory simulation.
Both ABIs' plans run on any host, checking restored SP/BP, dynamic allocation,
nonvolatile vector upper halves, home addresses, variadic stores and signed-range
boundaries. This generates and executes no foreign code. Native x86 tests add
owning insertion and full public-opcode/ABI pipelines; a separate C group compares
identical post-allocation inputs at O0–O3, including host wrappers and the corpus.
Those stage comparisons still require native x86 CI. The encoding increment below
adds bytes and relocations, followed by the owning x86 session increment.

Native M1 validation of this frame increment passed Debug/Release full suites
(122/122 each), the C-reference-disabled ASan suite (38/38), and the
C-reference-disabled Release build without tests or examples. The machine suite
passed 143 cases and 107,876 assertions in Debug, Release and ASan. A fresh
installed CXX-only consumer passed static/shared checks (2/2), with exceptions and
RTTI disabled and an unusable C compiler setting. The C++ example extracted from
the rewritten README built against that installation and returned 42 (1/1).
README and INSTALL now describe the replacement API and its actual platform
limits. Architecture checks and `git diff --check` passed. All builds used two
jobs; no Rosetta or x86 execution was used.

### Owning x86 encoding and relocation

`encode_machine` now dispatches a native x86 machine function to an owning
encoder. It preserves the original two-pass maximum-size layout, conservative
short-branch selection, per-label innermost-loop alignment, exact NOP sequences,
switch-table placement, and one 16-byte cell per constant reference. Loop-header
facts are retained from the allocation input graph, before property removal and
frame insertion. Public encoding still requires the native target; this does not
add cross-target generation.

The centralized declaration table is compiled into typed encoding operations and
word-size bounds at C++ compile time. Encoding does not rescan replacement strings
per instruction, and the layout estimate reserves the output buffer. No timing
improvement is claimed without a native x86 measurement. A test-only C hook
compares the original maximum-size decisions on native x86 hosts.

Both target encoders transfer mutable byte state through an internal checked
artifact builder. Published `CompiledFunction` objects independently own bytes,
frame facts, label identities, typed symbol/builtin relocations and x86 pool
metadata. They hold no machine-node pointers. Absolute x86 symbol fixups support
unaligned immediates; switch entries retain base-relative local relocations.
Relocation repeats safely at different bases and preserves the original six-byte
pool CALL/JMP shortening to REX + E8/E9 when the target fits signed rel32. It does
not mutate the source artifact. Verification checks pool bounds, displacements,
padding, call opcodes, label identity and overlapping fixups before publication.

`tests/CodeGen/X86Encoding.cpp` checks all 732 recipes and reconstructs every
base/index/scale combination across signed displacement boundaries on any host.
Raw artifact fixtures check near/far CALL and JMP at both signed rel32 limits,
resolver caching, copied symbol identity, local addresses, repeated relocation
and malformed metadata. They never construct a foreign target or execute foreign
code; the publication service rejects a nonnative artifact. Native x86 tests add
byte goldens, duplicate pool cells, all public opcodes and ABI fixtures, execution,
observer failures and source-lifetime checks. The C adapter compares exact bytes,
label offsets, pool references and loop alignment at O0–O3. The documented JRET
MOVDQU correction permits only its required prefix-byte differences; sizes,
addresses and all other bytes must still agree.

Stack-allocation rounding now precedes final pattern selection and checks for
overflow. In particular, rounding INT32_MAX no longer selects a signed-32
immediate and silently truncates the larger aligned value. Such an unencodable
final immediate returns a diagnostic; a native regression records this difference
from the original assertion/truncation path.

At this encoding increment the native x86 execution-session and host-wrapper
boundary was still pending; the following increment connects it.
Arm64 pure-rule tests and AArch64 regressions do not establish x86 byte or ABI
parity; the native-only comparisons are wired into the existing x86 CI suite.

Native M1 validation of this encoding increment passed Debug/Release full suites
(122/122 each), the C-reference-disabled ASan suite (38/38), and the standalone
Release build without tests or examples. The final machine suite passed 154 cases
and 203,327 assertions in all three configurations. The independent native C
adapter compared 1,420 AArch64 encodings, 87,680 bytes and 4,376 relocations, while
retaining the new x86 checks for native CI. A fresh installed CXX-only consumer
passed static/shared checks (2/2), with exceptions/RTTI disabled and an unusable C
compiler setting. The README example built against the fresh installation and
returned 42 (1/1). Architecture checks and `git diff --check` passed. Compilation
used two jobs throughout; no Rosetta or x86 execution was used locally.

### Owning x86 session and immutable lazy entries

The owning session now connects x86 compilation/publication to the existing
IR-built scalar and general host-call adapters, native interpreted entries and
eager/lazy compilation services. These services already use typed symbols and
the loaded program's independent state; no parallel x86 adapter object model or
C runtime hook is introduced. Standalone examples and the installed consumer
exercise compilation, session calls, interpreted callbacks and cold/warm entries
on either native architecture.

Each immutable x86 entry occupies 64 bytes and shares one resolver per loaded
program. The publication service reserves fixed slots and emits each entry only
once, avoiding the former per-function placeholder encoding and buffer allocation.
Resolver save facts use a fixed-size array. No throughput improvement is claimed
without measurement. Warm dispatch reads the destination slot and tail-jumps using reserved
R11, without changing AX/AL, arguments, bound registers or SP. The cold gate places
the descriptor in reserved R10 and pushes the signature's x87 result count.
The shared resolver saves all thirteen nonreserved general registers and uses
FXSAVE64/FXRSTOR64 for the full sixteen XMM registers, x87 stack and floating
control state. Its pure frame layout uses 624 bytes on SysV and 656 bytes on
Windows, including the latter's separate 32-byte callback shadow area. The
metadata push and these frames align both callback SP and floating-state storage.
All temporary stack storage is removed before tail-entering the function body;
the original stack arguments and return address remain in place.

Compilation errors retain the cold state and report through ExecutionInvocation.
The resolver returns zero integer/SSE result banks and exactly zero, one or two
x87 results as declared by that function. A void or scalar failure must not leave
extra x87 entries; repeated failures must not overflow that stack. Double-width
long doubles are canonicalized before computing the count. Windows retains the
original one-result restriction, with explicit native error assertions and
continued multiple-result interpretation coverage.

Host-independent tests check both frame layouts, every saved register, alignment,
shadow storage and nonoverlap. Native tests call gates with overflowing integer,
floating and variadic argument banks, force volatile XMM clobbers on GNU x86
hosts, verify floating control restoration, and repeat failed x87 calls beyond
the hardware stack depth before retrying. The session suites now enable native
x86 coverage for ownership, source destruction, module/data references, callbacks,
block arguments, variadics, hardware effects and lazy errors. Independent C
execution comparisons also exercise eager/lazy sessions and native interpreted
entries on x86. Actual x86 execution and ABI acceptance remains pending the
corresponding native CI hosts; local arm64 layout checks do not establish it.
BBV and per-function engine selection remain separate outstanding migrations.

The floating-environment regression also exposed a bug in the earlier owning
AArch64 resolver: it saved GPRs and vectors but not FPCR/FPSR. A callback that sets
downward rounding and raises inexact caused three independent assertions to fail
on native M1: the target observed downward rounding, the host retained it, and
the inexact flag leaked. The resolver now saves/restores both system registers
and aligns vectors in a 784-byte frame. The isolated reproduction passes after
the fix; success, failure and retry all check the environment. This is an owning
runtime correction, not a claim of an independently reproduced C defect.

Final native M1 validation passed Debug/Release full suites (122/122 each) and
the C-reference-disabled ASan suite (38/38, `detect_leaks=0`). The runtime suite
passed 91 cases and 13,675 assertions in all three builds. The standalone Release
build without tests, examples, tools or C references passed. A fresh installation
passed both CXX-only consumers (2/2: owning execution/static components and shared
serialization), with exceptions/RTTI disabled and an unusable C compiler setting.
The current README example was extracted, built against that installation and
returned 42 (1/1). Architecture checks and `git diff --check` passed. Compilation
used two jobs; no Rosetta, foreign target generation or native x86 execution was
used locally. These results leave native x86 acceptance, BBV, per-function engine
selection and removal of the old core/C reference outstanding.

### Owning AArch64 encoding and relocation

`encode_machine(function, frame_result, options)` verifies the final owning body
and consumes no C representation. It returns an independent `CompiledFunction`
containing little-endian code bytes, full label offsets, frame layout and typed
symbol/builtin or local-table relocations. No machine instruction, label, symbol,
Context or analysis pointer remains in this result. Compiled label handles carry
artifact identity; copied artifacts share that identity, while another compilation
rejects them. `find_label` and `label_addresses` support label addresses, differences
and signed displacements after source destruction.

The shared AArch64 declaration table now also produces dense encoding recipes at
compile time. Checked field operations replace runtime parsing of hex templates,
while retaining target pattern priority, wide-immediate inversion/chunk order,
memory scale/offsets, allocation rounding, overflow condition selection, switch
padding/tables and final alignment. Overflow context is maintained during the
forward walk, with the same label/branch barriers as the original backwards
search. No encoder decisions call C matchers or traverse a second instruction body.

Unknown symbol moves reserve all four MOVZ/MOVK chunks and retain owned symbol or
builtin identity plus displacement. `relocate(base, resolver)` creates a new byte
buffer, resolves each identity once, checks fields/overlaps/ranges and applies
local addresses and symbol chunks. It does not publish executable memory or alter
the original bytes. An explicit `known_addresses` environment enables the original
shortest resolved-immediate choice; no runtime address is written back into IR.

Eleven native cases cover exact words, wide constants, known/unknown symbols,
builtins, label ownership, switch aliases, overflow barriers, displacement limits,
stale/foreign input, diagnostics and independent lifetime. Every public opcode and
ABI fixture reaches checked encoding with C disabled. A standalone OS-level test
publishes relocated bytes through the owning code-memory service and executes O0–O3 arithmetic,
dynamic-stack memory writes, loops, switch, external calls and hardware bindings.
`LinkedProgram` now uses this publication service for owning module data and
ordinary-function linking; the full execution session remains separate.

An eleventh independent C group compares 1,420 encodings, 87,680 bytes and 4,376
relocations, with identical explicit symbol addresses. It checks both resolved
encoding and relocation of the unresolved artifact, branch/address/data target
offsets, absolute switch slots and the entire byte sequence. Unused synthetic
entry/exit labels and same-position alias multiplicity are excluded from offset
comparison; all observable targets are retained. No C bytes are used as product
output. The example and installed CXX-only consumer now reach encoding/relocation.

`mirnext_encoding_benchmark` constructs identical alternating arithmetic and
four-chunk-immediate physical instructions, then times checked encoding after
allocation/frame work. With the optional reference build it also measures the
previous numeric C++ encoder/template parser. Five sequential native M1 Release
runs (median of seven repetitions per run, then median of run medians) produced:

| Instructions | Previous template encoder | Owning encoder | Byte fingerprint |
| --- | --- | --- | --- |
| 4,097 | 2.902 ms | 0.522 ms | `5768167062198650037` |
| 16,385 | 11.890 ms | 2.207 ms | `7448058458853972661` |
| 65,537 | 47.181 ms | 8.810 ms | `1401093221149700789` |

Every repetition requires identical bytes. These timings include each model's
validation and result allocation, so the difference cannot be attributed solely
to template parsing. They measure neither the C encoder nor end-to-end JIT time.

### Whole-function compilation and immutable publication

`mirnext/CodeGen/Compilation.hpp` provides `compile_function(function, target,
options)`. Input is verified, materialized, non-SSA register IR, normally from
`optimize`. It runs machine preparation, target lowering, legalization,
coalescing, liveness, live ranges, allocation, rewrite, edge preservation,
post-allocation passes, frame generation and encoding in the original order.
O0–O3 retain their original stage conditions. Options control backend level,
host-register preservation, known encoding addresses and a checked observer.
Snapshots contain only callback-lifetime machine/artifact views. Observer failures
stop compilation and retain the error code plus module, function, target and stage.
The input IR is never modified, and results retain no closures or analyses.

One invocation owns all machine state. The coalescing conflict matrix is released
before allocation analyses are built; the input graph, liveness and live ranges
are released after rewrite. Edge preservation and frames keep only the facts they
need. Repeated compilation has no Context-level generator state to reset.

`mirnext/Execution/CodeMemory.hpp`, exported as `MIRNext::Execution`, owns native
code pages. `reserve(sizes)` lays out one immutable batch with 16-byte slot
alignment and exact page rounding. Its move-only `CodeReservation` provides checked
addresses so the linker can resolve all code before publication. `publish(buffers)`
validates every buffer before writing, opens write access once, writes the batch,
closes write access and flushes instruction caches. No executable handles are
returned until all steps succeed. Reservation abandonment and resolver failure
release unpublished pages; platform failures allow publication to be retried.

`ExecutableCode` handles retain a shared immutable mapping and its C++ provider.
They outlive reservations, publishers and other handles. Published pages are never
reopened by a later batch. Linux/Windows use writable non-executable pages followed
by read/execute protection; Apple arm64 uses MAP_JIT and per-thread write protection.
Relocation and result storage allocation happen before native JIT write access opens.
`CodeMemoryProvider` is an injectable C++ OS service, with no untyped callback data,
old runtime context or borrowed C memory regions.

### Owning lazy function compilation

`LinkOptions::compilation` selects `JITCompilation::Eager` (the default) or `Lazy`
for `LinkedProgram` and JIT sessions. Optimization still runs on the complete
program at creation. Lazy mode publishes stable native entries first and runs
the owning machine pipeline when a function body is first entered. Untaken
functions have no machine artifact or body allocation. No legacy generator is
used. Interpretation rejects a lazy-JIT policy.

Each definition has a stable C++ compilation record and destination slot.
`FunctionHandle::compilation_state()` reports `Uncompiled`, `Compiling` or
`Compiled` (and `Interpreted` for interpreter sessions). `compile()` explicitly
prepares a JIT definition; repeated requests reuse its artifact. `artifact()` is
null until successful compilation/publication. Existing artifact pointers remain
valid as other functions compile. Imported aliases share their definition's state.
`LinkStatistics::functions` counts all definitions and `compiled_functions` counts
materialized ordinary bodies; expression host-boundary copies are counted separately
by expression initialization, and ABI adapter caches retain their own entry count.

Function relocations, function-pointer data and retained `NativeEntry` handles
always identify the stable entry. Recursion and cross-module calls use the same
identity before and after materialization. Label-reference data must identify an
actual instruction address, so its label owners compile before data relocation.
JCALL/JRET definitions also compile before execution because their nonstandard
ABI has no ordinary failure-return path. Expression initializers compile their
host-preserving boundary copy at load time; any ordinary calls they execute can
materialize further bodies. Other ordinary definitions stay cold.

On AArch64, each immutable entry is 64 bytes and shares one resolver frame per
loaded program. PC-relative address literals reduce warm dispatch to four
instructions: two loads, a compiled-state branch and a tail-branch. Cold calls
save the integer registers and complete SIMD registers on a 768-byte aligned
frame, preserving stack arguments, native varargs and hardware bindings across
the C++ compiler call. Only the target's reserved r9/r10 are scratch registers.
Successful publication updates a writable data slot; published entry code is
never patched or reopened. A 128-function regression checks one entry-code batch,
shared resolver size and compilation of only the invoked body.

Observer, relocation and publication failures leave the body uncompiled and its
destination unset. Failed mappings are released, diagnostics propagate through
`ExecutionInvocation`/checked calls, and a later call can retry. Reentrant explicit
compilation of the same in-progress record returns a diagnostic. A failed nested
callee leaves an already compiled caller cached; effects performed by that caller
are not rolled back. Function/data/native-entry handles retain all compilation
services after the source IR and session are destroyed. External serialization
still covers calls, global mutation and compilation across handles.

Cold hardware-binding coverage found an independent reference defect: C CFG
liveness treats module bindings as implicit call inputs, while its live-range
and reverse rewrite scans drop caller-clobbered physical bindings. The standalone
Debug C probe aborts in `process_bb_ranges` at `mir-gen.c:6833` for a variadic call
using r5/v20 (confirmed with a native debugger backtrace). The owning scans now
retain these implicit inputs consistently, including last-use analysis, and keep
all boundary consistency checks enabled. A separate regression executes that
case at O0–O3 under eager and lazy compilation and returns 42. This is an explicit
correction of the C reference's assertion failure.

The execution-session benchmark checks eager/lazy creation, first call and cached
calls separately. Three sequential native M1 Release runs (seven trials per run,
100,000 calls per warm interval) gave these medians of run medians:

| Integer arguments | Eager creation | Lazy creation | Eager first call | Lazy first call | Eager cached checked call | Lazy cached checked call |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 125.1 us | 65.8 us | 85.6 us | 129.4 us | 16.9 ns | 17.0 ns |
| 17 | 465.5 us | 287.7 us | 253.5 us | 410.3 us | 29.6 ns | 29.9 ns |

Lazy creation defers the machine pipeline to the first call; it still performs
middle-end optimization and publishes entries. Both modes reuse their compiled
body afterward. The measurements are local stage costs, not a whole-program
speedup or a portable latency guarantee.

### Owning native linking and runtime helpers

`mirnext/Execution/Linker.hpp` adds `LinkedProgram`, an owning C++ link state for
optimized modules. It lays out named and anonymous data sections in MIR order,
materializes scalar, zero, reference, label and expression data, resolves imports
through typed `ExternalSymbol` metadata, and publishes all ordinary functions in one
immutable code batch. Program definitions win over host resolution, imports are
resolved once per name, and function/data handles retain private state and code pages
after the source `Context` and `OptimizedProgram` have been released. Hardware
register effects stay in the internal MIR ABI; host-register preservation is used
for expression initializers and session entries at their explicit host boundaries.

`mirnext/Execution/Builtins.hpp` exposes runtime helpers through typed byte spans and
`Builtin` identities. Variadic extraction, block copies and scalar conversions validate
ABI, bounds, alignment and destination sizes before committing cursor updates.
`ExecutionInvocation` keeps first-error diagnostics scoped to one native entry, so nested
helpers cannot overwrite an earlier failure. These services contain no C runtime
object or callback-table dependency.

The linker uses one reservation/publication batch for a module's functions. This
reduces page protection transitions and mapping overhead while keeping relocation and
expression evaluation outside executable write access. Tests cover cross-module calls,
function-pointer data, label address/difference relocations, read-only globals,
external storage, expression values and supported variadic/block helper classes.

`CodeMemory::publish(compiled, resolver)` checks the artifact, reserves its final
address, relocates, and returns an `ExecutableFunction` owning the artifact and
code mapping. Compiled label identities and label-address calculations stay valid.
The API does not offer arbitrary writes to published code or a generic foreign
call interface. `examples/native-compile.cpp` uses the new DSL, optimizer,
compilation entry and code publisher, destroys the producer objects, then invokes
the known host signature at the ABI boundary. Session interpretation and typed
builtin/variadic helpers and host-callable interpreted entries are provided by the
native execution layer. Whole-function lazy compilation is provided by the owning
session; BBV remains outstanding.

### Owning execution sessions

`mirnext/Execution/Session.hpp` provides `mirnext::execution::ExecutionSession` on
the replacement object model. It owns the loaded program, persistent globals and
cached native entries. Creation accepts a `Context`, a `Module`, or a move-only
`OptimizedProgram`. `ExecutionOptions::mode` selects JIT or interpretation once per
session. With no explicit optimization options, JIT uses O2 and interpretation
executes materialized original IR. Explicit options run the selected C++ pipeline
for either mode. Consuming an `OptimizedProgram` never reruns optimization and
rejects options requesting another optimization pipeline. JIT and native-call
adapters currently support AArch64. Eager whole-function compilation remains the
default; `LinkOptions::compilation = JITCompilation::Lazy` delays machine compilation.

Both execution modes now use one owning `detail::LoadedProgram`. The immutable
IR, resolved symbols and allocated data have one lifetime; interpreter bodies,
interpreter entry adapters and compiled artifacts are derived caches on that same program.
`ExecutionSession` and `FunctionHandle` refer directly to this state instead of
wrapping separate linked/interpreted program variants. The explicit
`LinkedProgram` and `InterpretedProgram` APIs use the same state as the session.
The IR owner is destroyed after symbol bindings and host callbacks, including
callbacks whose captures borrow the resolved import's module/name metadata.
Retaining any function, data or native-entry handle retains the complete program.

`mirnext/Execution/Function.hpp` provides the owning callable interface separately
from session creation. A `FunctionHandle` supports `call`, `call_i64`, `call_u64`,
`call_double` and `native_entry` directly. Source IR and the creating session can
both be destroyed before the first call. Scalar and generic ABI adapters, their
observer/provider services and persistent globals belong to `LoadedProgram`;
handle copies and session calls reuse the same caches. `SessionState` has been
removed. The session still rejects foreign handles, while a direct handle call
uses its own program. `valid()` identifies a moved-from handle; its call methods
return a diagnostic. Metadata access requires a valid handle. Interpretation
retains the existing reentry activation, instruction/depth budgets and callback
policy when invoked without a session.

`find_function` returns an owning, typed `FunctionHandle`; `find_export` returns
either that handle or owning `LinkedData`. Both modes use `call` with
`ExecutionArgument` scalars, block values or explicit writable RBLK buffers and
return all scalar results as `std::vector<ScalarValue>`. Mixed scalar types, all
block classifications, unnamed variadic arguments and multiple results use the
same checked API. Unnamed F32 arguments must be promoted by the caller. Invalid
counts, types, block sizes, ABI classes and return buffers fail before entry
generation or user execution.

`call_i64`, `call_u64` and `call_double` check ownership, calling convention,
argument count and types in both modes. These helpers require fixed, homogeneous scalar signatures
with one result; integer helpers accept narrow integer, boolean and pointer bit
patterns, and the double helper accepts F64. They support stack-passed arguments
through target ABI lowering without a fixed arity limit. `artifact()` provides
the compiled definition when available; `native_entry()` explicitly obtains an
owning ABI entry. Interpreted handles never expose an integer token masquerading
as a callable address. JIT rejects interpreter-only
budgets or native-call policy options instead of ignoring them.

Each distinct callee address, signature and unnamed argument type sequence gets
one cached generic ABI adapter, including aliases of the same import. Different
typed contracts at the same address retain separate adapters. An adapter loads
typed values or block pointers from scalar storage slots, uses an ordinary IR
call, then stores every result in separate output slots. The fixed scalar helpers
retain a smaller cached wrapper taking one pointer to contiguous 64-bit slots.
Both use normal C++ ABI lowering and code publication.
Only these small boundary wrappers use O0; user functions retain the requested
O0–O3 middle-end and backend behavior. Host callee-saved registers are preserved
while hardware-binding effects within MIR remain observable. Hardware bindings
are collected once per loaded program, avoiding a full-program scan for each new wrapper.
Cached calls validate the handle and signature and reuse executable code directly.
Generic adapters use a per-invocation inline frame for up to 16 combined argument
and result slots; larger calls use one shared frame allocation. Each recursive
call owns its frame, so host callback reentry cannot overwrite its caller's
values. Cache hits compare unnamed argument types in place; only a new adapter
allocates the owning type sequence. Validation still precedes code generation
and user execution. The execution-session benchmark includes fixed and variadic
calls with 1 and 17 arguments. On the local arm64 Release build, the median of
three runs (each reporting seven trials) changed fixed single-argument generic
calls from 102.6 to 67.7 ns, fixed 17-argument calls from 261.0 to 244.7 ns, and
17-argument variadic calls from 290.1 to 237.3 ns. These are local measurements,
not portable latency guarantees. Regression coverage includes zero results,
multiple results, frame boundaries, heap overflow frames and repeated calls.

Linked handles retain their program after session destruction, but session calls
reject handles from another program. Session moves preserve handle ownership.
Observer and publication failures do not cache a partial entry or execute user
code, and later calls may retry. Builtin diagnostics are scoped to each invocation;
a failed call does not poison later calls. Calls and global mutation require
external synchronization. Host imports and borrowed resources captured by the
compilation observer must outlive all retained program handles as applicable.
Owning captures are retained by the program's compilation service.

`examples/native-session.cpp` returns a callable DSL counter, destroys its source
IR and creating session, then uses checked calls to update persistent data. `examples/execution-values.cpp`
uses identical calls for interpreted and JIT block/mixed/multiple-result functions.
`examples/native-interpreter.cpp` adds a typed C++ callback and execution budgets.
The external CXX-only consumer links and executes both modes. Native tests cover
0/1/8/9/17 integer and F64 arguments at O0–O3, narrow extension, unsigned and NaN
bits, hardware side effects, moves, alias caching and failure/retry paths. An
independent test-only C engine compares the same serialized source with both its
interpreter and JIT for call/data fixtures and narrow argument boundaries. The
numeric oracle also exercises the common session's interpreted and generic native
calls at O0–O3, retaining integer widths and floating bit patterns.

`mirnext_execution_session_benchmark` separately reports session creation, first
host-entry generation/call and repeated checked calls for 1 and 17 arguments. It
measures session and direct-handle scalar/generic calls plus generic variadic calls,
uses seven repetitions and verifies that handles and sessions share their adapters. It does
not compare the old runtime or claim end-to-end optimization throughput.

Three sequential native M1 Release runs gave these medians of run medians (each
warm interval includes 100,000 checked calls):

| Scalar arguments | Session creation | First scalar entry/call | Cached scalar call | First generic entry/call | Cached generic call |
| --- | --- | --- | --- | --- | --- |
| 1 | 0.135 ms | 88.4 us | 16.9 ns | 88.3 us | 67.7 ns |
| 17 | 0.454 ms | 253.3 us | 29.0 ns | 249.3 us | 244.7 ns |

Creation includes optimization, ordinary-function compilation and linking; the
first call includes wrapper construction, compilation and publication. The warm
measurement reuses both program and wrapper. These are different operations,
reported separately to make the cache cost visible, not a speed ratio against C.
Generic entry generation follows scalar measurements on the same loaded program;
the two adapters have independent cache entries.

After moving adapter services into `LoadedProgram`, three sequential Release
runs also measured direct owning-handle calls. The medians of run medians were
16.5 ns for one scalar argument and 28.5 ns for 17 arguments; generic handle calls
were 70.1 ns and 247.3 ns respectively. Each timed loop checks results and reuses
the same adapter as session calls. These local timings include checked dispatch,
not function compilation, and do not establish a whole-program speedup.

The loaded-state unification and frame/cache optimization passed all 122 native
arm64 Debug and Release tests, including the C oracle, and all 38 ASan tests with
the C oracle disabled. Regression cases retain source-name metadata in callback
destructors, destroy source IR and sessions, and release function, native-entry
and data handles separately. Release without tests or C references, installation
and both external CXX-only consumers also passed. Compilation used two jobs;
no Rosetta execution was used.

Owning-callable regressions additionally destroy both the source and session
before first invocation, exercise all call ABI fixtures at O0–O3, retain cold
observer/provider services, and release function/data handles independently.
Observer rejection and code-publication failure leave globals unchanged, release
failed mappings and permit a later successful retry. A retained interpreted
function shares reentry depth limits and recovers after a failed invocation.
This callable migration passed 122/122 Debug and Release tests, 38/38 ASan tests
with the C reference disabled, a no-tests/no-reference Release build, installation
and both external CXX-only consumers. The architecture dependency guard also passed.

Generic calls normalize checked values directly into their ABI input frame. This
avoids a temporary vector allocation and a full argument-array copy per
invocation; the interpreter and adapters share the same per-argument validation.
The measured warm intervals above include argument validation, the native call
and result validation, and exclude construction, compilation and first-call costs.

Four additional machine cases compare the compilation entry with individual
stages at O0–O3, reject invalid/unmaterialized inputs, stop observers at every stage,
and check publication resolution, diagnostics and independent artifact lifetime.
Eight code-memory cases cover exact layout/page rounding, malformed providers and
overflow, batch validation before writes, platform failures and retry, move/shared
ownership, abandoned reservations and concurrent native publication/execution.

`mirnext_code_publication_benchmark` compares individual immutable mappings with
one batch for the same functions. Both paths check identical bytes and execute
every function. Output validation, calls and unmapping are outside the timing
intervals; the service's own input checks remain inside. Three sequential native
M1 Release runs, each taking the median of seven repetitions, yielded the following
medians of run medians:

| Functions | Individual immutable mappings | One immutable batch |
| --- | --- | --- |
| 64 | 0.340 ms | 0.0056 ms |
| 256 | 1.585 ms | 0.0073 ms |
| 1,024 | 5.596 ms | 0.0135 ms |

The last case uses one 16 KiB mapping instead of 1,024 mappings. This compares two
uses of the new publication service, not the old runtime's pooled allocator. It
measures neither optimizer nor end-to-end JIT throughput. `LinkedProgram`, including
session creation, uses the batch API for ordinary functions; host wrappers are
published individually when first requested.

### Owning canonical interpreter

`mirnext/Execution/Interpreter.hpp` provides `InterpretedProgram`, which owns an
immutable, materialized canonical IR snapshot and its mutable loaded data. It can
clone a Context or Module, or consume an independently owned `OptimizedProgram`
without rerunning optimization. Its function and data handles retain the snapshot
after source destruction and moves. Foreign program handles are rejected. Calls
and data mutation require external synchronization, as with native sessions.
Optimized inputs retain their original function-name lookup even when cross-module
inlining renames private symbols. Shared double/long-double representations follow
the verifier's host-specific scalar rules while preserving bits.

The interpreter reads canonical instruction operands through a derived preparation
cache. Register indexes, label destinations, symbol bindings, instruction
classification and operand counts are resolved once. This cache owns no second
mutable IR body and does not write preparation or runtime addresses into source
nodes. Hardware bindings share slots by physical identity across nested calls.
Label addresses identify stable prepared locations; same-position labels share an
address. Ordinary calls, indirect calls and JCALL/JRET have separate checked
signature/convention paths.

`ScalarValue` preserves scalar types and floating bit patterns at the C++ call
boundary. `ExecutionArgument` accepts scalars, copied blocks of all five ABI
classes, and explicit writable RBLK return buffers. Typed C++ import callbacks use
these same values and return `Result<std::vector<ScalarValue>>`; result counts and
types are checked before returning to IR. A callback import can omit a native ABI
address, and repeated imports of the same host name share identity. Callbacks take
priority when provided. Normalized callback arguments own independent copies of
fixed and unnamed by-value blocks, preserving their ABI classification; RBLK
continues to share writable caller storage. This matters when another argument
aliases the source block: writing through that alias must not change the callback's
by-value parameter. A regression reproduced the old callback result of 99 where
both native ABI paths returned 42, then verified all three return 42 after the fix.
A native address without a callback uses the C++ ABI
adapter service, compiled lazily on first invocation and cached by exact signature
and unnamed argument types. This supports actual C++ host mixed scalars, by-value
blocks, hidden return buffers and variadics. Nonstandard external JCALL has no
ordinary-ABI adapter and returns a diagnostic. `native_calls=false` requires typed
callbacks and prevents native import adapters. IR function bodies continue to be
interpreted; this service never invokes the C engine. Imported code and resources
captured by providers/observers must outlive their retained program handles.

The interpreter and native linker share `ProgramImage` symbol resolution, ordered
data layout, imports/exports, references and label relocation. Expression data is
initialized by the selected engine. Read-only data ranges are merged once and
searched during stores. A segmented bump stack implements dynamic allocation and
nested BSTART/BEND scopes without changing the host stack. Fixed block arguments
are copied; RBLK storage remains shared with its caller. Native-layout synthetic
va-lists preserve independent copied cursors, mixed scalar/block extraction and
the special address meaning of undef memory operands.

Instruction and call-depth budgets apply across nested IR calls. Exhausted budgets,
invalid switch/jump/restore targets, vararg exhaustion, read-only stores and invalid
calls return diagnostics with the module, function and instruction. Division by
zero, signed division overflow, invalid shift counts and floating conversions
outside the integer domain return errors instead of reproducing C undefined
behavior.

Native tests execute original IR and O0–O3 products after source destruction, with
calls, globals, expression data, block copies, return buffers, hardware sharing,
label differences, recursive limits, stack scopes and error/retry cases. A rejecting
code-memory provider confirms pure interpretation never requests executable pages.
Independent tests compare scalar opcodes, comparison branches, NaNs, signed zero,
infinities and overflow branches against the C interpreter and O0–O3 JIT. Integer
fixtures include negative and high-bit inputs. As specified by MIR, 32-bit results
are explicitly extended before comparing their otherwise undefined upper halves.
`examples/native-interpreter.cpp` uses DSL construction and a typed host callback;
the installed CXX-only consumer also executes this path.

`mirnext_interpreter_benchmark` checks results and separately measures preparation
and repeated interpreted calls with 0, 64 and 1,024 loop iterations. Moving dispatch
classification to preparation, avoiding unused fixed-argument copies, reserving
operand storage and disabling unused instruction counting produced these checked
call medians on native M1 Release. Each number is the median of three sequential
runs, each of which takes the median of seven 500-call intervals:

| Loop iterations | Before these dispatch changes | After |
| --- | --- | --- |
| 0 | 0.313 us | 0.289 us |
| 64 | 4.037 us | 4.001 us |
| 1,024 | 59.855 us | 57.522 us |

The longer loop improved about 3.9% in this measurement; the shorter loop difference
is small. Preparation and source construction are outside these call intervals.
This is a comparison within the new interpreter, not against C or JIT throughput.
The owning x86_64 session path now awaits native platform acceptance; BBV still
needs migration. Each session
currently selects one engine; sharing mutable
loaded data while switching individual functions between engines is not yet exposed.

The interpreter/session migration passed native arm64 Debug/Release full suites
(121 tests each), with additional Debug/Release execution and C differential
checks after the callback alias and ownership fixes. The callback-argument owner
is move-only, preventing copies whose views could outlive their block storage.
All 38 tests passed with ASan and the C reference disabled, followed by focused
ASan execution checks (`detect_leaks=0` on macOS). A Release build without tests,
examples, tools or C references was installed, and both CXX-only external consumers
passed with a deliberately unavailable C compiler. Compiler parallelism remained
two; these runs used no Rosetta.

### Native entries into interpreted IR

`InterpreterOptions::native_entries` (also available in `ExecutionOptions`) enables
ordinary native ABI entries at load time. The option is separate from
`native_calls`, which controls outgoing native calls. `native_entry(handle)`
returns an owning `NativeEntry` containing the declared signature and a real code
address. The handle retains the loaded IR, data, callbacks and executable pages
after session destruction. A JIT session offers the same accessor for its native
MIR entry; explicit hardware bindings retain their MIR side effects there.

Entry construction uses canonical C++ IR and the owning target pipeline, with O0
only for these ABI adapters. It marshals fixed scalar/block parameters into typed
storage, captures native variadic state, calls a typed C++ dispatcher, and restores
all scalar results through the target ABI. Interpreter function bodies are never
compiled. RBLK remains a shared hidden return buffer. Normal parameter copies,
narrow extension, register-bank exhaustion and stack arguments use the same target
ABI lowering as ordinary compilation. Entries are published in one immutable batch
before data references and expression initializers are evaluated. Function-pointer
data, symbol operands and exposed entries therefore share the same stable address.
Repeated callback-only imports share an entry by resolved symbol identity.

Pure interpretation keeps this option disabled, allocates no executable memory,
and exposes no native function address. Passing a known virtual function identity
directly to native code returns a diagnostic; function pointers embedded in host
memory also require entries to be enabled before loading. This is a load-time
contract, not an attempt to scan arbitrary host memory. Enabling entries does not
silently select a different execution engine. Observer, target and publication
failures return diagnostics and abort loading; there is no C fallback.

`JCALL/JRET` functions retain internal interpreted identities and label semantics;
requesting an ordinary native entry for them returns a diagnostic. A fixed typed
callback-only import can receive its own entry. Exposing a variadic callback-only
import requires a native implementation, because a native call frame provides no
type or count metadata for reconstructing arbitrary unnamed C++ values. Variadic
IR functions instead receive a real captured va-list and consume it through MIR
`VA_ARG`/`VA_BLOCK_ARG`. Copies and nested uses retain independent cursors. Unlike
synthetic va-lists created by checked C++ calls, native va-lists rely on the caller
supplying the correct types/count; the native ABI supplies no end pointer.

A stack-scoped C++ invocation activation links native callbacks to the invocation
they reenter. Instruction/depth budgets and logical hardware bindings survive a
round trip through native host code, including repeated nested callbacks. Independent
sessions keep their own loaded state. Calls and data mutation still require external
synchronization. `ExecutionInvocation` collects the first error across a native ABI
entry; direct callers create this scope and check `status()`, while checked session
calls create it automatically. A failed interpreted entry returns zero scalar
storage and reports the error; side effects already performed are not rolled back.
Nested scopes restore their parent and a failed call does not poison later calls.

`examples/interpreted-callback.cpp` builds a DSL callback, passes its symbol to a
real C++ host, and executes the callback under the original interpreter budget.
Tests cover original and O0–O3 inputs, independent entry lifetime, function-pointer
and expression data, all block classes, actual C++ large-struct returns, mixed
scalars, floating bits, 17 arguments, real variadics, callback import aliases,
nonstandard internal calls, reentry limits and failed compilation/publication.
The independent C numeric oracle also checks these actual native entries against
the C interpreter/JIT. The installed CXX-only consumer invokes one without the C
runtime. `mirnext_interpreter_benchmark` reports entry-enabled preparation and warm
native ABI calls separately from checked C++ interpretation. Native timings include
an error scope but rely on a known signature; checked C++ timings include dynamic
handle/argument validation, so they are not a throughput comparison between engines.

This entry migration passed native arm64 Debug/Release full suites (122 tests
each), final Debug execution/C-oracle checks, and all 38 ASan tests with C references
disabled. A final regression also passed in Debug, Release and ASan: an interpreted
function entered through a real native va-list calls another function with synthetic
varargs, whose exhaustion must still return a bounds diagnostic. The Release
no-tests/no-reference build, installation and both external CXX-only consumers
passed with an unavailable C compiler. Compiler concurrency stayed at two and no
Rosetta process was used.

### Canonical data-reference use relationships

The IR records data references at their checked construction point,
`Module::define_data`. `Symbol::reference_data_uses()` identifies reference-data
initializers that point to that symbol identity, including imports, self references
and declarations completed later. `Function::label_data_uses()` and
`Function::expression_data_uses()` identify label-address/difference data and
expression initializers. A difference with identical anchors registers one use.
The sparse indexes belong to the module and borrow its stable `GlobalData` nodes;
they do not copy initializer payloads or add another authoritative data body.
Objects without data references allocate no per-object use list.

Registration follows successful owner/type/initializer checks. Rejected initializers
and repeated definitions publish no use. Symbol renaming and export retain the same
identity, and label movement changes the existing anchor rather than its use record.
Cloning and native text/binary reading rebuild use relationships through the same
factories, so resulting uses never depend on the source program's lifetime. Borrowed
use spans record registration order and can change when more uses are added; ordered
module items retain the existing data-layout and serialization order.

CFG construction, optimization graph roots, label cleanup, block-erasure checks,
jump optimization, simplify's expression classification and machine preparation now
query these owner-specific uses. They no longer scan every module data item once
per function. Inlining reads the callee's live label uses and no longer keeps a
separate whole-program label-address snapshot. Its size/growth/ABI restrictions
remain intact, including the prohibition on inlining a function referenced by label
data. Whole-program runtime initialization still follows ordered module items.

Native tests reconstruct expected use relationships independently from all initializer
variants and compare them after construction, cloning and format roundtrips. They
cover forward completion, imports, anonymous reference data, self references, label
differences, expression initializers, sparse-index growth, source destruction, failed
construction, repeated definitions and protected label anchors during structural
rewrites. An inlining regression adds label data after stage initialization and
checks that the newly referenced callee remains uninlined. The installed static/shared
consumer also checks the new use-query APIs.

`mirnext_data_uses_benchmark` measures CFG construction and simplify on data-heavy
modules with 128/512 functions and 4,096/32,768 unrelated data items. Construction,
cloning and serialization are outside the timed regions. It checks CFG roots,
transformation counts and complete binary fingerprints over seven repetitions.

On native Apple M1 arm64, Release measurements alternated the previous and current
executables over five process rounds, with seven repetitions inside each process.
The table reports medians of the process medians, in milliseconds. Compilation and
tests were idle during timing; both executables used the native C++ implementation.

| Functions / unrelated data items | CFG before / after | Simplify before / after |
| --- | --- | --- |
| 128 / 4,096 | 0.517 / 0.075 | 1.348 / 0.308 |
| 512 / 32,768 | 13.862 / 0.293 | 30.733 / 1.477 |

These data-heavy fixtures improve CFG time by 6.9–47.3 times and simplify time by
4.4–20.8 times; they do not measure whole-program compilation or index construction.
Both versions retain exactly 480/1,920 transformations and binary fingerprints
`9720072835355677602` / `17908669999997526662`. Build the optional
`mirnext_data_uses_benchmark` target with `MIRNEXT_BUILD_BENCHMARKS=ON` to run it.

Validation for this change: Debug and Release each pass 73/73 CTests, including the
optional C transformation and machine-stage oracle; native ASan passes 59/59 with
the C oracle disabled. Compilation uses two jobs; ordinary CTest uses eight and
ASan uses four. The no-tests, CXX-only product build installs 105 files; its fresh
external static/shared consumer passes 3/3. Fourteen changed implementation, test
and benchmark units also pass x86_64 syntax-only compilation. This checks source
portability, not x86 execution or ABI acceptance; no Rosetta execution is used.

### Module and function pass ownership

`ModuleAnalysisManager` owns module analysis results and stable per-function
`FunctionAnalysisManager` instances. Both use one typed, RTTI-free cache implementation.
Instruction, CFG and register mutations propagate revision counters to their owning
module; symbol creation, definition, renaming, export and variable binding have a
separate symbol revision. Data still has its own revision, including label-address
data that can change CFG reachability. Checking a cached module analysis takes
constant time and does not walk function bodies. Preserving an analysis never
overrides a changed dependency. Renaming a symbol no longer bumps the data revision
or needlessly expires CFG/SSA state. Clearing module results clears child results while
retaining the child manager identities. Foreign-function requests return diagnostics.
Function cache snapshots include the module symbol version separately from public
function-body revisions, so creating a sibling declaration does not make an atomic
failed Builder operation appear to have changed the original function.

Function topology invalidation also follows canonical data-reference ownership.
Publishing label-reference data increments the owning function's control-flow
revision and the module's aggregate control-flow revision, without changing either
instruction revision. Both anchors belong to that function, so a label difference
publishes one topology change. Unrelated functions retain their CFG, dominance,
loop and register-liveness results. Ordinary initialized/BSS/reference/expression
data does not expire those analyses. Analyses that explicitly depend on
`ModuleData` or `Symbols` still expire on the corresponding module revisions.

SSA def-use validity depends on instructions, control flow and registers. Adding
unrelated data keeps its values, uses and PHIs valid and permits conventional SSA
and SSA exit to continue. Creating new label data during its owner's SSA phase is
rejected before publishing a symbol, item, use or revision, following the existing
public Builder phase discipline: a new externally reachable entry would require
new PHI predecessors. Deferred data definitions perform the same check. Existing
label data is valid throughout the SSA lifecycle; this check applies only to new
definitions, not verification or read-only traversal.

Native analysis tests cover ordinary initializer variants, module and child cache
preservation, data-dependent invalidation, unchanged SSA PHIs/def-use, continued
SSA exit, owner-only label entry changes, deferred/repeated definitions and atomic
phase/owner failures. The installed consumer checks CFG preservation after adding
reference data.

`ModulePassManager` runs each stage across its selected modules before advancing to
the next stage. `add_function<Pass>()` adapts existing native function passes and
uses the module-owned child managers. Initialization and finalization run once per
stage, including failed initialization and observer failures. Function passes take
a stable snapshot of their input function identities; appended helpers enter the
next adapted stage. Native module passes can use both module analyses and child
analyses through `ModulePassContext`. Module and nested function observations retain
separate typed views and transformation counts. Failures identify the module/stage,
clear affected analysis caches and return through `Result`.

The optimizer frontend now uses this module pipeline for scope materialization,
simplification and inlining. Scope overloads of `FunctionPassManager` delegate to the
same scheduler instead of maintaining a second whole-program execution loop. The
original phase-major order and inlining decisions remain unchanged. Symbol-only
metadata edits invalidate symbol analyses without expiring an otherwise current SSA
def-use graph. `examples/native-analysis.cpp` and `examples/native-inline.cpp` show
the new ownership and observation APIs; the installed CXX-only static/shared consumer
also invokes the module manager.

Native lifecycle tests cover independent dependency invalidation, move-owned analysis
results, cycles, hardware-register aliases, declarations completed after caching,
cross-module exports, new helper functions, SSA preservation, explicit preservation,
owner errors, failed passes, failed initialization, failed observers and invalid
post-pass IR. Existing inlining fixtures and O0–O3 differential optimization tests
continue through the same native pipeline.

Native arm64 validation for this change: Debug **73/73** and Release **73/73**
CTest entries passed, including the C differential stages; the C-reference-disabled
ASan build passed **59/59** with `detect_leaks=0`. The analysis executable passed
**23 cases / 12,680 assertions**. Existing Builder atomicity regressions in the full
suites also verify the separation of module symbol and function-body revisions.
The test-disabled, CXX-only product rebuilt and installed
**105 native files** without old C headers or `libmir`; an external CXX-only consumer
passed **3/3** with static/shared linkage and module-pass invocation. Compilation used
two jobs, ordinary CTest eight and ASan four. Thirteen relevant translation units
passed x86_64 syntax checks; these checks do not establish native x86 execution
parity. No Rosetta execution was used.

Follow-up validation after function-local data invalidation passes **28 analysis
cases / 13,123 assertions**, with no skipped cases. Debug passes **73/73** in
98.47 s, Release **73/73** in 27.77 s and C-reference-disabled ASan **59/59** in
75.32 s (`detect_leaks=0`). The no-tests CXX-only product installs **105 files**;
its fresh static/shared consumer passes **3/3**, including preservation of the CFG
after reference-data construction. Ordinary data changes no longer force topology
recomputation or make an unchanged SSA phase unusable. The existing data-heavy
benchmark retains both transformation counts and full binary fingerprints.

An additional build-only AppleClang x86_64 configuration compiles all **88 core
C++ translation units** and links all **seven static and seven shared libraries**
with the C oracle, tools, tests, utilities and examples disabled. Its relocated
installed package also builds and links both external CXX-only consumers. Native
`lipo` inspection confirms x86_64 for all fourteen libraries and both consumers.
The new analysis tests additionally pass x86_64 syntax checking. None of the x86_64
products is executed; these are compile/link checks, not native x86 JIT, lazy or
BBV execution acceptance. No Rosetta is used, and original-platform execution
remains a required retirement gate for the temporary C oracle.

### Owning canonical optimization

`mirnext/Transforms/Optimization.hpp` provides `transforms::optimize(Context)` and
`transforms::optimize(Module)`. Both make one direct typed clone, retain the source's
identities and leave the source untouched. The module overload does not inspect or
clone unrelated modules. Imports remain external in that overload; the Context
overload permits cross-module inlining. Function lookup retains original names
even when inlining renames private symbols. Results own their interned names,
symbols, data, instructions and statistics, and do not retain observer closures.

The frontend materializes stack scopes and simplifies every function before any
caller is inlined. All levels prepare CFG; O0 retains unreachable blocks. O2/O3
then run the original cloning/SSA, GVN, copy propagation, DSE, SSA DCE, loop analysis,
LICM, pressure relief, conventional SSA, combine, SSA exit and jump sequence.
The ADDR decision is taken before CFG pruning. Its path builds unrenamed SSA,
transforms addresses and exits that SSA atomically, clones, then rebuilds renamed
SSA. Analysis views in observers are borrowed and never force lazy analyses to
run. Completed results contain register IR and support native verification,
diagnostic dump, portable text, and compressed/raw binary output. `ExecutionSession`,
`LinkedProgram` and `InterpretedProgram` consume these results directly; this API
does not bridge to old IR.

Independent C snapshots compare the complete middle end at all four levels,
including both SSA constructions in the address path, every public opcode, the
original regression corpus and fixed-seed interactions. Integration exposed two
defects now covered separately: a closed single-input PHI must become an undefined
initial definition rather than leave dangling edges after deletion; adjacent copy
coalescing must retain an existing hardware binding. The C reference is repaired
at those sites instead of reproducing dangling state or losing observable writes.
These are explicit corrections to the reference, not relaxed normalization.
The transitional C++ optimizer received the same binding barrier and closed-PHI
repair while its differential tests remained active. That optimizer and its
compatibility tests have since been retired; the owning suites retain the fixes.

`IRRewriter` also distinguishes a branch-register rewrite from an edge change:
replacing operands at the same opcode preserves CFG revisions when label operands
are unchanged. SSA renaming therefore retains the current ordered graph; changing
a target label still invalidates it. Cache observations never recompute or erase
an analysis and return null for absent or stale entries.

The owning pipeline avoids constructing before-stage statistics without an
observer, moves completed statistics into reserved storage, reuses pass managers
across functions and constructs the address-specific manager only when needed.
`mirnext_optimization_benchmark` times cloning, verification and all enabled passes,
excluding source construction and result dumping. Five alternating native M1
Release rounds, each with seven repetitions, compared the statistics allocation
refinement on 512 four-block functions with 80-character module/function names:

| Level | Before | After | Ordinary allocations before/after |
| --- | --- | --- | --- |
| O0 | 4.957 ms | 4.768 ms | 100,504 / 92,301 |
| O2 | 40.801 ms | 39.301 ms | 893,866 / 859,102 |

Times and allocations are medians of run medians. IR/statistics fingerprints,
stage counts and transformation counts agree in every round. The measured time
change is about 3.8%/3.7% for this metadata-heavy workload; it is not a whole-JIT
speedup or a cross-platform performance guarantee.

## Performance changes and measurement

The new dominance analysis stores immediate dominators and traversal intervals
instead of a universal bitmap per block. It preserves the original dominance facts,
including universal initial facts for disconnected cycles. Sparse nonzero-word
register sets avoid allocating the whole register space for every block, and
unchanged unions avoid allocating temporary vectors. Addressable CFG targets use
layout-indexed flags instead of a tree set. Loop-root lookup uses path compression
without changing the ordered loop tree. `FunctionAnalysisManager` supplies shared
graph dependencies to analyses, and caches results by their actual revision inputs.
The liveness worklist is a fixed-capacity FIFO ring; its storage stays proportional
to block count even when loops require many updates. Hardware return boundaries
iterate a once-collected binding list instead of scanning all local registers.

`mirnext_ir_benchmark` compares the dominator tree with an independent dense-bitset
fixed-point implementation on the same graph and checks every dominance relation.
Reported fact bytes cover persistent dominance facts only, excluding shared CFG,
container overhead and construction scratch. This is an analysis microbenchmark;
it does not measure the original C optimizer or JIT performance.

One native arm64 M1 Release run measured the following, after excluding graph
construction and checking all relations outside the timed interval:

| Graph Blocks | Tree Time | Dense Solver Time | Tree Fact Bytes | Dense Fact Bytes |
| --- | --- | --- | --- | --- |
| 259 | 14.4 us | 13.5 us | 6,216 | 10,360 |
| 1,027 | 53.3 us | 70.4 us | 24,648 | 139,672 |
| 4,099 | 188.7 us | 690.4 us | 98,376 | 2,131,480 |

The smallest sample is slightly slower; the improvement grows with graph size.
These timings are informational, not CI thresholds.

The isolated C analysis adapter compares ordered CFG edges, DFS numbering,
dominance facts, live/gen/kill sets and the complete ordered loop tree at O0–O3.
It runs fixed fixtures, original MIR regressions and 128 seeded valid CFGs without
loading, linking, block cloning, SSA renaming or target lowering between stages.
Native tests also use independent fixed-point solvers with the C oracle disabled.
The adapter also covers every public opcode and co-located label anchors. `JCALL`
keeps the following code as a potential caller-supplied return location, as in MIR;
it can also end a function as a tail transfer. Its nonstandard ABI remains distinct
from ordinary `CALL`.
Public register-use indexing records `LADDR` as a definition; optimization liveness
retains the original C scan's input classification for that operand. Hardware-bound
registers have typed implicit uses at ordinary returns, without fake public NOPs.
`BuildSSAPass` consumes this same ordered graph. `SSARewriter` maintains both directions
of each def-use edge and batches edits to one canonical operand vector per affected
instruction. Values, edges and PHIs use stable segmented storage; ordinary instructions
are never copied into SSA nodes. The phase survives analysis-cache invalidation.
Ordinary Builder and rewriter edits are rejected during SSA; use `SSARewriter` instead.
Register-use and pre-SSA liveness analyses reject this phase rather than ignoring PHIs.
Format output and public cloning require completed register IR. Renamed SSA cannot be
discarded until `ConventionalSSAPass` has inserted the ordered predecessor copies.

The isolated SSA oracle compares full instruction operands (types, literal bits,
memory attributes, symbol and hardware identities), ordered CFG and def-use edges
after construction, conventionalization and exit. It covers the 16 original MIR
regressions, every public opcode, fixed CFGs and 128 seeded cases. Execution probes
compare original interpretation with native SSA exit followed by interpretation
and O0-O3 JIT, including loop-carried values, PHI cycles and global memory effects.
Native-only tests cover stable identities, lifecycle, invalid edits, observer errors,
repeated optimization and 12,000-block forwarding chains.

Original C `get_def` recurses indefinitely on a closed single-predecessor cycle
without a reaching assignment. The native implementation terminates iteratively
with an explicit undefined initial definition; a native regression records this
difference without invoking the C undefined behavior. Forwarding-path memoization
starts only after ordinary definitions are complete, preserving C's scan decisions.
`GVNPass` now consumes this SSA model directly. The remaining middle-end
transformations after GVN have not yet migrated to it.

`AddressTransformPass` requires unrenamed SSA, normally after simplify and CFG
preparation. It first records address-family decisions from the intact inverse
use edges, then exits SSA and rewrites canonical instructions in layout order.
Scalar loads/stores become the original register operations or narrow extensions;
escaping locals receive address/load/store instructions. Surviving instruction
identities, operand order and reverse insertion order for multiple result stores
match the reference. Storage registers retain their type and hardware binding,
but no longer participate in general scalar operations or implicit hardware
return uses. Address outputs and scalar moves can still write storage registers,
including conventional SSA edge copies for nested addresses. Actual definitions
rename through their uses exactly as in C; initial storage definitions retain
their identities because they are not visited by renaming. Exit derives storage
facts for the address sources introduced by PHI renaming.
This storage fact is cloned and verified; it is not a new file-protocol field.
Reloaded public IR can derive it again by running address conversion.

The independent C address hook compares initial SSA, address conversion plus
exit, rebuilt SSA, conventional SSA and final exit at O2/O3. Fixtures cover all
four ADDR variants, six narrow extensions, floating types, loops, PHI joins,
two address roots for one local, nested pointer addresses with conditional PHIs,
multiple call results and hardware bindings,
plus 128 fixed-seed copy/load/store programs. Executable fixtures compare the
original interpreter and all four JIT levels with serialized native results.
The external CXX-only consumer also exercises address conversion. See
`examples/native-address.cpp` for a DSL-only construction example.
The reference execution adapter preserves the host's r19/r12 across the fixture
call: a MIR global hardware binding intentionally changes that register on
return, while a C++ caller may keep its Context pointer there in Release mode.
The wrapper retains the original MIR result and prevents that intentional effect
from corrupting the test harness.

Two original C assumptions require explicit boundaries. Its address collector
asserts that a memory consumer is a scalar move and does not check the source
register class before substituting it. Native conversion keeps actual storage
for block ABI arguments and byte reinterpretation between floating and integer
types; native construction, SSA rebuild and binary coverage remain enabled.
Those execution comparisons use the original interpreter and O0/O1, which do
not invoke the unsafe C conversion. A raw unsimplified hardware address also
causes C to remove an already-cleared SSA edge; normal simplify prevents this
by introducing its hardware proxy. Native IR now rejects ADDR/ADDR8/ADDR16/ADDR32
on hardware-bound registers: a physical register has no memory address. Copy to
an ordinary local before taking its address. Native and reference fixtures use
this explicit copy, preserving the binding's independent return side effects.
On targets where long double equals double, the C reader cannot consume every
LD-tagged data/memory record after its host type normalization. Long-double
address execution fixtures use integer conversions and the C text reader;
native text and binary roundtrip coverage retains the distinct public LD type.

The C address conversion also deletes an ADDR assignment into a pointer slot
observed through an earlier address. If the pointer has direct uses too, C keeps
the assignment initially but SSA renaming moves it away from that slot. The
same loss occurs for self addresses. Stage-only regressions prove these changes
without executing their resulting invalid memory reads. Native
escape discovery propagates through stored pointer addresses with a worklist,
retaining the pointee storage and making the observable pointer slot assignment an
explicit memory store before SSA renaming. The dependency map is only allocated
for escaping families. This regression compares original interpretation/O0/O1
with native results through interpretation and O0-O3, retaining native SSA and
format coverage. Internal scalar alias loads can still be promoted to register
reads; their assigned pointee address must remain live. The unmodified C O2/O3
source paths for these defects are intentionally not executed.

Address-family discovery follows inverse SSA uses rather than scanning the whole
function per definition. The earlier `SSAGraph::transform_addresses` collector in
`mir-opt-address.cpp` scans all stored uses for the root and for each reached copy,
then repeats collection during rewriting. One root with N separate copies and
loads therefore incurs quadratic collection work. The new collector borrows each
definition's inverse uses and retains the family once, eliminating those scans.
Operand vectors are copied only for changed instructions,
and storage markings validate the entire selected set in one body scan.
`mirnext_ssa_benchmark` also times 1,024/4,096/16,384 address copies and scalar loads,
checking that every load becomes a register move and the root address disappears.
The timer includes address verification and exit, but excludes initial SSA
construction. These are scaling measurements, not whole-JIT speedup claims.
In a native arm64 M1 Release run, the complete conversion took 2.26 ms for 1,024
copies, 9.70 ms for 4,096 and 51.74 ms for 16,384 (about 2.2-3.2 us per copy/load
pair). Every run checked the exact replacement count. This is a new-model
measurement; no timing comparison against the older collector is claimed.

`BlockCloningPass` edits canonical blocks and instructions before SSA. It retains
the original hot prefix ending at the first RET/JRET, LIFO cold-root processing,
initial DFS back-edge exclusions and the strict `size > 3 * original_size` bound.
The first cold root starts with zero growth budget; subsequent original roots
initialize their budget after the worklist shrinks, exactly as in C. Boundary
fixtures keep label-address anchors and multiple roots so the recursive hot
checks reach the strict nine-versus-ten instruction boundary.
Construction ranks retain MIR's distinction between block ranks and anchor
instruction ranks in recursive hot checks; unreachable removal does not renumber
those ranks. Conditional clones receive an explicit continuation block. Edge
insertion order is retained when rebuilding analyses, since it determines PHI
input order. Materializing an empty SSA prologue preserves that order too.
`FunctionAnalysisManager::set` installs updated native analyses at the current
revision and rejects replacement of a running analysis. It does not bypass
invalidation after subsequent IR edits.

An independent C hook isolates cloning followed by SSA construction,
conventionalization and exit. Fixed and 64 seeded cases cover multiple cold
predecessors, growth boundaries, back edges, fallthrough, pre-existing entry
labels, dead layout, calls, hardware bindings and floating instructions. Native-only tests verify
identities, prohibited return/switch destinations, errors, cloning and format
roundtrips. Execution compares the original interpreter with serialized native
results and O0-O3 C JIT. The exported CXX-only package exercises the new pass;
`examples/native-cloning.cpp` builds and observes a complete cloning/SSA example.
The owning optimization API also runs and compares the second cloning stage
after address conversion in the complete native middle-end pipeline.

`GVNPass` requires renamed, nonconventional SSA after operand simplification.
It updates the canonical instructions and their inverse use edges directly;
there is no second function body and no call into the C optimizer. It retains
MIR's traversal and rewrite order, integer folding widths, overflow-flag guards,
single-use copy removal, PHI shortening, constant branch pruning, add/sub address
canonicalization, dominance checks and preferred displacement interval.
Memory availability uses the original alias/nonalias rules and call barriers.
`GVNAnalysis` carries value numbers, stack provenance, memory-location classes
and the full-escape fact for subsequent passes. Its borrowed identities are valid
only while the analysis remains current; leaving SSA invalidates it. Updated CFG
edges retain their relative insertion order, with removed edges discarded before
the analysis is published.

The independent C hook isolates GVN, conventionalization and SSA exit on identical
simplified inputs. Tests compare normalized operands, literal bits, ordered CFG
edges and SSA def-use for the original 16 regressions, every public opcode, rule
fixtures and 64 seeded programs at O2/O3. Execution compares unoptimized
interpretation, serialized native GVN followed by interpretation, and original
versus transformed O0-O3 C JIT. Cases include aliases, calls, multiple results,
loop memory effects, displacement boundaries, hardware bindings and nonfinite
floating values. Nonfinite oracle inputs use the independent C binary reader,
because its text scanner does not accept the signed inf/nan spelling. Native-only
tests retain full opcode/encoding coverage without that oracle, and check source
independence, repeated optimization, phase/owner errors, observer failure,
metadata invalidation and atomic SSA edits. `examples/native-gvn.cpp` uses the
DSL, stage observations and independent cloning; the CXX-only consumer also
executes the pass.

Constant folding uses unsigned bit arithmetic and explicit widths. Division by
zero, signed minimum divided by minus one, and out-of-range shift counts are left
unfolded instead of executing undefined C expressions. MIR specifies only the low
32 bits of short-operation results; their high bits are unspecified. Native GVN
retains C's constant-materialization choice, while execution comparisons extend
narrow results explicitly before observing a 64-bit result. A separate regression
checks narrow negation's specified low bits across all four C backend levels.

The owning middle-end pipeline interface now assembles these passes. The current
runtime does not yet consume this canonical pipeline as its default JIT path.

`CopyPropagationPass` and `DeadStoresPass` now work directly on the canonical
SSA. Copy propagation keeps the original 30-hop limit, hardware-binding barriers,
address-taking exclusions and immediate-neighbor coalescing pattern, including
private PHI positions. Integer strength reduction preserves the original signed
division bias sequence and narrow/full opcode choices. Redundant extensions use
the original width/sign rules and full-width comparison exclusions. Same-width
native `double`/`long double` representations are compatible in operand validation
and SSA edits, allowing their conversion copies to propagate just as the original
C target does. This compatibility is enabled only where both native layouts match.

`SSARewriter::coalesce_copy` validates the combined definition/use rewrite before
changing a producer's register. It can move a producer into a hardware binding
while retaining hardware return effects; attempts to lose an existing effect or
break conventional PHIs fail before editing. It does not create a new body or
copy hardware identities for SSA definitions.

DSE consumes the exact GVN location classes and stack-provenance flags, computes
backward memory liveness over the retained ordered function graph, and deletes
only unobserved stores. Global stores remain live at exit. Calls preserve memory
according to full address escape, stack arguments and the original MAY/MUST
rules. Load liveness keeps alias/nonalias and same-allocation byte overlap rules.
Interval overlap uses unsigned distances to avoid the original signed
displacement-plus-size overflow, including overlap across the signed boundary.
A native-only regression distinguishes a four-byte overlap from an eight-byte
adjacency around `INT64_MAX`. These artificial displacements exercise analysis
and encoding only; the corresponding C expression can overflow and is not used
as a defined-behavior execution oracle. The pass requires current GVN metadata and
diagnoses missing/stale metadata, wrong owners and unsupported SSA phases.

`FunctionAnalysisManager::take_cached` transfers an existing, current result
without computing or copying it. Missing/stale entries return an empty optional;
taking a running analysis returns a diagnostic. Copy propagation and DSE move the
GVN maps and memory-location storage between stages. Copy propagation also moves
and republishes the unchanged function graph after value-operand edits advance
the conservative CFG revision. Tests check that both backing allocations survive;
unrelated analyses still invalidate normally. Metadata borrows the living SSA
form and is discarded at SSA exit. Historical number entries may refer to dead,
function-owned SSA definitions, whose storage remains stable until that exit.

Native tests exercise rule boundaries, seeded loops, phase errors, checked edits,
source independence, retained metadata and every public opcode with the C oracle
disabled. The independent oracle observes GVN, copy propagation, DSE,
conventionalization and SSA exit, comparing instruction order, typed operands,
CFG and def-use. Additional execution tests compare original interpretation,
transformed interpretation and both O0-O3 C JIT inputs. `native-gvn.cpp` and the
installed CXX-only consumer run this extended pipeline.
Hardware coalescing fixtures include both ordinary and PHI producers, retaining
hardware reads and return effects through SSA exit, encoding and O0-O3 execution.
The broader CFG/PHI fixtures also cover GVN's removal of a copy from a
single-use PHI output, using the same rule as a single-use instruction output.

`SSADeadCodePass`, `LICMPass` and `PressureReliefPass` also consume the same
canonical instructions and function-owned SSA. DCE preserves the original
LIFO deletion order, call/allocation/vararg effects and overflow-flag barriers.
It removes unused PHIs through checked SSA edits; PHI cycles with uses remain,
matching the original algorithm rather than adding SCC-based elimination.
Its temporary adjacency index tracks logical private-PHI positions without
copying instruction bodies. Removed-node successor queries use path compression.

LICM retains the original innermost-loop restriction, unique fallthrough entry,
conditional-edge preheader creation, multiplication cost rule and register
pressure heuristic. Division/remainder, memory, calls, varargs, stack operations
and hardware-bound operands remain excluded. Preheader insertion updates SSA
predecessor identities and the ordered graph, reusing the split edge's index so
the graph contains no detached edge records. SSA construction already gives
initial-label functions a canonical prologue; LICM uses that same block.
Pressure relief runs with the loop tree discarded, as in the original pipeline,
and moves a literal with one non-PHI use immediately before its user. Hardware
return effects are handled at the return boundary without public USE opcodes.

Instruction moves use stable-list splices. Private PHIs retain their position
when their ordinary anchor moves; surviving instructions and SSA values keep
their identity. SSA inverse uses now have a stable C++ list order: insertion
prepends, individual retargeting splices to the new definition's head, and whole
definition replacement appends the transferred chain. Erasing an unrelated use
does not shuffle remaining uses. Independent LICM snapshots caught that such
shuffling changed invariant discovery and the resulting instruction order.
The pass uses inverse edges directly instead of searching every instruction for
each definition. Neither this ordering change nor splice alone is claimed as an
end-to-end JIT speedup.

Native tests retain these passes with the C reference disabled, exercising
ownership, instruction identity, phase errors, observer checks, ordinary and
conditional preheaders, initial-label loops, hardware effects and every public
opcode. The independent C hook observes DCE, LICM and pressure relief before
conventionalization/SSA exit on the same middle-end input. It checks typed IR,
instruction order, CFG and def-use, including seeded dependency DAGs and loops,
and executes serialized native results with the original interpreter and JIT.
These are explicit canonical passes; the runtime default has not yet switched
to this pipeline; the backend/runtime object-model migration remains outstanding.

Expanded CFG/loop execution checks exposed additional defects in the C
reference. LICM treated an explicit switch target at the next layout label as a
fallthrough, creating an unlabeled preheader that the actual jump bypassed.
Both implementations now require a real fallthrough (or reuse an adjacent
unconditional-jump predecessor). Replacing a loop-entry edge also preserves its
incoming-edge slot, so existing PHI operands remain paired with their original
predecessors, including irregular CFGs. The corrected baseline is explicit;
these cases are not silently excluded from stage coverage.
Property branches (`prbeq`/`prbne`) are also explicitly treated as control flow
when selecting preheaders and rejecting invariants. The original C branch helper
excluded these later-added opcodes. Isolated stage comparisons and native
encoding regressions ensure that property branches stay in their original blocks.
Their outgoing PHI copies are placed before the branch during C
conventionalization, so a taken edge cannot bypass its own copy.

A hardware-return regression also found a C CFG construction bug: a branch can
create a RET block before the scan reaches that RET. Inserting hardware USE
before the RET then appended it after RET in the block metadata, despite the
opposite physical order. AArch64 O1 live-range construction walked past the
function start and crashed. The C builder now inserts that metadata before the
existing RET node. The named regression and loop execution comparisons exercise
O0-O3, without skipping the hardware case. This metadata correction does not
change the intended instruction order or lowering decisions.

`mirnext_dce_benchmark` measures SSA DCE alone, outside parsing, SSA preparation
and output verification. Common instructions keep their logical detached-use
counts in eight stack slots; wide PHIs use a hash-table fallback. DCE reserves
its instruction/worklist indexes once and groups private PHIs by their anchors,
avoiding a PHI-list scan at every instruction. Five alternating Release rounds
on native M1 arm64, each with seven repetitions per shape, retained identical
IR/SSA fingerprints and deletion counts. For 8,192 dead independent definitions,
the median changed from 6.250 to 5.830 ms (6.7%); a dependency chain changed from
6.965 to 6.572 ms (5.6%). Ordinary allocation counts fell from 65,609 to 57,377
and 65,596 to 57,377, respectively (about 12.5%). The 1,024-definition shapes
improved by 5.0-5.9%; one first-round timing was an outlier. These measurements
compare the first canonical DCE implementation to its allocation/indexing
refinement, not to the original C optimizer or whole JIT compilation.

`SSACombinePass` runs after `ConventionalSSAPass` and before SSA exit. It preserves
the original forward block / reverse instruction traversal, including private
PHIs and opportunistic dead-definition deletion. Integer comparison branches
retain both truth directions; floating comparisons combine only a true branch,
preserving unordered/NaN semantics. Address exploration follows the original
move, add/subtract, multiplication and shift precedence, cyclic-PHI restrictions,
scale truncation and rollback decisions. Arithmetic on displacement bit patterns
uses unsigned operations, avoiding the C implementation's signed-overflow behavior.
Malformed ordinary definition cycles return a diagnostic rather than loop forever.

The new `MIRNext::Target` component provides `target::AddressingRules` for x86_64
and aarch64. It is the optimizer's predicate, not an encoder or a cross-target
code generator. Aarch64 intentionally retains the original `log2(size)` rule;
it is not replaced with a more permissive architectural addressing rule. Boolean
storage has MIR's eight-byte scalar representation. The original C predicate
divided by zero for nonzero byte displacements and requested unsupported sizes
for undef/block memory; the explicit C reference corrections reject those cases.
Native tests cover the rule boundaries on both targets, and all public opcodes
still construct, verify and roundtrip. Unresolved symbol addresses are not guessed
or folded into numeric displacements.

`SSARewriter::replace_memory` validates the complete operand and the owner/type
of both reaching definitions before changing it. Unrelated input edges retain
their identities and inverse-use order; base and index remain distinct typed
sites even when they use the same register. Literal-pool provenance and memory
alias annotations survive. Branch replacement keeps the canonical instruction
identity; erased comparisons and private PHIs use the checked SSA deletion path.

The independent C probe runs both isolated combine and the preceding value/memory/
motion pipeline. It compares typed instructions, ordered CFGs and both memory
def-use sites. C deliberately drops the edge to an initial fake self move when
combining an address. Only the test adapter reconstructs those omitted edges,
requiring the C engine to confirm the register's argument/undefined initial kind;
ordinary and PHI edges remain strict comparisons. No C layout is imported into
the canonical product. Tests also execute native combined results through the
independent interpreter and O0-O3 JIT, compare global storage bytes, and cover NaN,
hardware bindings, original regression programs and fixed-seed cases. The example
and installed CXX-only consumer now run combine before exiting SSA.

`JumpOptimizationPass` runs after SSA exit and keeps the original forward layout
traversal and revisit rule. It removes unreachable and singly entered empty blocks,
deletes ordinary branches to the immediately following label, and redirects ordinary
branches through adjacent labels and jump chains. Property branches, switches and
indirect jumps retain their operands and edges. All surviving labels and instructions
keep their identities, and address/data anchors remain valid. The pass does not
introduce an extra fixed-point sweep or change the original O2/O3 enabling condition.

The graph retains the address-taken roots discovered at CFG construction, even if
an LADDR disappears later. Jump cleanup walks real body/indirect edges from the
first block and those roots; synthetic entry edges do not make dead code reachable.
It updates ordered incoming/outgoing edges in place and compacts the derived graph
once. Removed IR blocks are validated and erased together, avoiding a whole-function
reference scan for each empty block. The IR remains the only instruction owner.

An original C defect could make branch threading loop forever on a multi-label
jump cycle. Both implementations now stop before revisiting a target label; the
isolated C oracle covers that correction independently. The test hook uses the
original enabling levels because C's O0 CFG lacks the instruction records needed
by jump optimization. Native tests include 4,096 adjacent label anchors, independent
ownership, invalid phases, repeated analysis construction and format roundtrips.
Stage comparisons retain physical instruction order, ordered predecessors and
successors, label/data identity and historical address-root flags. Execution probes
compare returns and global bytes through the independent interpreter and O0-O3 JIT.
The example and installed consumer run jump cleanup after SSA exit. This is not yet
the runtime's default canonical optimization pipeline.

`mirnext_jump_benchmark` times jump cleanup with CFG construction outside the
measurement, on dense conditional graphs with and without jump chains. Visited
label sets are now allocated only when threading a branch, avoiding two ordinary
allocations for a branch whose target cannot change. Five alternating native M1
Release rounds, seven repetitions per shape, kept identical IR/ordered-CFG
fingerprints and transformation counts. For 8,192 blocks without chains, the
median of run medians changed from 3.520 to 3.070 ms (12.8%), with allocations
falling from 65,631 to 49,251. With chains it changed from 3.757 to 3.368 ms
(10.4%), with 67,692 allocations becoming 55,406. The 1,024-block cases improved
by 14.6% and 14.8%. Allocation counts cover ordinary global `operator new` only.
These compare the initial canonical pass to its allocation refinement, not to
the original C optimizer or whole-JIT compilation.

The checked memory edit now transfers its already prepared operand storage through
`IRRewriter::replace_owned`, retaining full validation and atomic failure while
removing a second vector copy and validation. `mirnext_ssa_memory_benchmark`
times only those edits, excluding IR/SSA construction, output verification and
fingerprinting. Five alternating native M1 Release rounds, with seven repetitions
per size per round, kept identical IR/SSA fingerprints and unrelated edge identities.
For 8,192 edits, the median of the five run medians changed from 1.779 to 1.261 ms
(29.1%), and ordinary allocations fell from 24,704 to 16,512. At 32,768 edits,
7.526 became 5.444 ms (27.7%), with 98,816 allocations becoming 66,048. The
1,024-edit case improved by 28.1%. The allocation counter covers ordinary global
`operator new`, not every allocator or peak memory use. This is a local before/after
comparison of the new edit implementation, not C-optimizer or whole-JIT performance.

An independent regression exposed a defect in original C `copy_prop`: commuting
the operands of a constant-left AND for unsigned extension swapped the operands'
SSA edge pointers but left their inverse `use_op_num` fields unchanged. Stage
snapshots showed identical instructions with reversed def-use sites; subsequent
SSA DCE aborted in `remove_ssa_edge`. The C reference now repairs those two edge
positions when swapping, without changing any transformation choice. The named
`original C constant AND commutation updates inverse SSA edges before DCE` test
covers AND/ANDS and all JIT levels; native tests also verify both operand sites.
Comparisons involving this rule use that explicitly corrected C baseline, not
the crashing unmodified metadata. Native rewrites always update the operand and
SSA sites together.

DSE caches each load location's alias/intersection set, separately for MUST and
non-MUST stack provenance, and precomputes the call-visible nonstack set. Both
liveness setup and the deletion walk use sparse-word operations on these sets.
Deleting stores advances a saved stable-list iterator in constant time, without
searching the block again. `mirnext_dse_benchmark` isolates DSE after GVN/copy
propagation, outside parsing/preparation and output verification. It checks IR/SSA
fingerprints and transformation counts on every repetition. Five alternating
baseline/cached rounds on native M1 arm64 Release, with seven repetitions per
shape, retained identical outputs. Distinct-location shapes with eight
load/store pairs per block changed from 2.526 to 2.160 ms at 32 blocks (14.5%)
and 19.850 to 14.035 ms at 128 blocks (29.3%). Ordinary allocation counts increased
by 6.5% and 8.8%, respectively, for the cache. Reusing eight locations showed
differences within about 1.6%, including a 0.2% slowdown, so no benefit is claimed
for those shapes. Stores in these shapes are globally observable and no stores
are deleted; the timing measures memory-liveness work. These are isolated DSE
measurements, not whole-JIT or cross-platform speedup claims.

GVN availability transfer precomputes each block's alias kills once, then uses
sparse word subtraction and union at fixed-point iterations. This preserves the
original facts and traversal while avoiding repeated pairwise alias checks.
`mirnext_gvn_benchmark` measures only this pass, with input preparation and output
verification outside the timed region. It reports ordinary allocation counts,
transformations and a checked IR/SSA fingerprint. On native M1 arm64 Release,
five alternating baseline/optimized rounds (seven repetitions per shape) retained
identical fingerprints and transformation counts. For 128 blocks of eight loads,
median GVN time changed from 8.222 ms to 7.046 ms (14.3%); the store-containing
shape changed from 9.492 ms to 9.007 ms (5.1%). Kill caching increased ordinary
allocation counts by about 0.35% and 1.2%, respectively. Smaller shapes showed
only 0.4-4.0% differences, near measurement noise. These are isolated pass
measurements, not whole-JIT speedups or an assertion about other platforms.

Original C cloning assumes every destination outside its ordinary branch enum
has a following label. PRBEQ/PRBNE violate that assumption and trigger the
assertion in `clone_bbs`; JMPI does not have a fallthrough edge at all. Native
cloning preserves their explicit branch/indirect edges and removes unreachable
continuations. These regressions retain native construction, encoding and
all-level execution coverage. Their unmodified C execution comparison uses
O0/O1 and the interpreter, which avoid the failing C cloning rule.

Cloning normally borrows the destination's stable instruction operands directly,
removing the temporary vector and its per-instruction operand allocations.
Only self-cloning snapshots the changing range. A no-change pass returns without
rebuilding the graph, matching the original C condition. The standalone
`mirnext_cloning_benchmark` clones a 128-ADD block into 8/32/128 cold roots,
checking the exact transformation and instruction counts on each repetition.
It reports median pass time and allocation counts; parsing and CFG preparation
are outside the timer. Timings describe this pass, not whole-JIT performance.
With native arm64 M1 Release binaries, removing the scratch copies changed
ordinary C++ allocation calls as follows (both variants keep the same IR checks):

| Cold roots | Copying operands | Borrowing operands | Saved allocations |
| --- | ---: | ---: | ---: |
| 8 | 4,975 | 3,871 | 1,104 |
| 32 | 18,961 | 14,545 | 4,416 |
| 128 | 74,782 | 57,118 | 17,664 |

The 128-root case removes 23.6% of allocation calls. Five alternating runs of
each binary, each reporting the median of seven repetitions, gave median pass
times of 0.279/1.117/4.380 ms with copies and 0.222/0.886/3.660 ms with borrowed
operands (about 16-21% lower). Timing counters and allocation instrumentation are
the same for both binaries; no concurrent builds or tests ran during measurement.
The exact instruction and transformation counts passed in every repetition.

The SSA microbenchmark overwrites a source register and then uses its reaching
definition twice in every ADD, exercising high fanout and register renaming.
On the same M1 Release build, native SSA construction plus verification took
16.8 ms for 4,096 ADDs, 56.2 ms for 16,384 and 256.4 ms for 65,536
(roughly 3.4-4.1 us per ADD). Graph construction is outside the timed region;
def-use edge counts are checked afterward. This records scaling, not a speedup
claim against the previous SSA implementation.

SSA verification now reuses an operand-site byte buffer instead of allocating
an input vector and ordered set for every instruction. Each byte independently
tracks the output, register input, memory base and memory index; repeated base/index
uses cannot hide a missing edge. Output definitions are checked once, eliminating
the quadratic scan for calls with many results. Ownership sets reserve their
storage, and hardware return validation collects bindings once per verification.
The buffer is local to verification, so no check is skipped after an edit.

`mirnext_ssa_benchmark` measures construction and separate verification on high-fanout
ADD sequences and calls with many results. Both paths check definition/use counts
and complete conventional SSA and exit outside the timed region. In one native
arm64 M1 Release before/after run, with each verification time averaged over five
calls in the same process:

| Shape | Count | Previous Verify Time | Updated Verify Time |
| --- | --- | --- | --- |
| ADD fanout | 4,096 | 2.57 ms | 3.38 ms |
| ADD fanout | 16,384 | 12.53 ms | 9.51 ms |
| ADD fanout | 65,536 | 80.85 ms | 54.22 ms |
| Call results | 64 | 30.54 us | 9.67 us |
| Call results | 256 | 309.10 us | 40.24 us |
| Call results | 1,024 | 4,241.32 us | 190.47 us |

The smallest ADD sample was slower in this run; these measurements do not establish
an improvement for every function size. The wide-call verifier scales linearly in
operand count; this does not claim that all SSA construction and editing costs are
linear. Construction, including its verifications, changed from 276.23 ms to
200.23 ms for the largest ADD sequence and from 9.57 ms to 1.22 ms for 1,024 call
results. These measurements cover native SSA infrastructure, not whole-JIT speed.
The independent C stage comparisons are unchanged, and native regressions check
mixed-type calls with 257 results/arguments, post-construction renaming, malformed
input kinds and duplicate memory-base edges.

Instruction lists store instructions directly: each stable instruction needs one
list-node allocation, and moving it uses `splice` without copying its operand vector.
Instructions retain their list position, making node lookup for replacement, erasure
and movement constant time. Operand replacement copies its operand vector once;
register-use replacement copies only affected instructions.
Block splitting visits the transferred suffix; merging visits the successor and
splices its whole instruction list. Neither rebuilds or copies a function body.
Unused-label cleanup scans instructions, module data and label ownership once,
instead of scanning all instructions separately for each label.
Named basic blocks use an index keyed by Context-owned strings, eliminating the
linear duplicate-name scan on every creation. Split/merge and cloning maintain this
index; `find_block` also uses it. Anonymous blocks do not allocate an index entry.
Identifier keys refer to the Context's interned strings; heterogeneous lookups accept
`string_view` without temporary strings. Node traversal does not allocate a vector of
temporary const handles. Unchanged analyses are reused; even a pass that claims to
preserve everything cannot retain a result whose dependency revisions changed.

Enable `MIRNEXT_BUILD_BENCHMARKS` and run `mirnext_ir_benchmark` in Release mode to
compare cached and rebuilt CFG queries for the same 256-block function. Timings are
informational, with matching graph checksums; tests never impose timing thresholds.
The benchmark also reports per-instruction rewrite costs for 4,096, 16,384 and
65,536 instructions, checking every rewritten value.
These are infrastructure changes, not changes to MIR optimization heuristics.

Inlining builds its exported-symbol and label-address indexes once per Context
pass, not once per call or function. Cross-module signature/symbol relocation is
cached per destination module, and unique generated names use a hash index.
Copying traverses callee nodes once and creates only the nodes the caller owns;
the checked owned-vector insertion overload transfers remapped operands directly
into their instruction, eliminating a second vector allocation/copy. Invalid
insertion leaves that vector unconsumed.
Layout changes use block-list splice. The benchmark compares the same inlining
decisions with shared phase initialization versus repeating Context validation
and rebuilding indexes for each individual function at 256, 1,024 and 4,096
callers, checking complete dumps and constant
checksums. This compares initialization scope and is not a whole-JIT speedup claim.

For this fixture, an arm64 M1 Release run measured shared initialization at
0.37/1.43/5.64 ms for 256/1,024/4,096 callers; repeated per-function initialization
took 8.55/130.58/2,149.27 ms. Both paths produced identical diagnostic dumps and
constant checksums. These measurements include each API's verification work;
they do not compare against the C optimizer or measure generated-code speed.

The native text reader stores literals in one contiguous array; small tokens retain
indices instead of a full `Operand` variant for every name and punctuation mark.
It initializes its opcode lookup table once, uses ordered data
definition records instead of a hash allocation per item, and reserves a bounded
token buffer. The writer allocates displacement expansion records only for affected
instructions. Anonymous register name generation uses a monotonically increasing
index instead of repeatedly scanning earlier names. The benchmark now also measures
text writing and parsing at the three instruction counts, with one anonymous register
per instruction and matching parsed checksums.
Binary writing keeps one owned copy of each string table entry; ordered entries
refer to stable hash-table keys. The benchmark also measures compressed binary
writing and reading at the same instruction counts and checks decoded values.

Call construction validates detached result registers, then transfers those same
objects into Function ownership. The validated operand vector moves into the stable
instruction node without a second vector allocation or copy. Register storage keeps
amortized vector growth across calls. The benchmark also checks 4,096/16,384/65,536
calls with one result and one scalar argument; timing is informational.
A native Apple M1 Release run measured 1.09/3.88/15.52 ms at those counts
(267/237/237 ns per call), with matching argument checksums and result-register
counts. This is a construction baseline, not a measured whole-JIT speedup.
Global-variable accesses also move their validated operand vectors into stable nodes.
Boolean literal stores materialize 0/1 directly, avoiding a temporary and runtime
comparison; dynamic stores and all boolean reads still normalize integer values.
The benchmark checks load/store instruction counts, register counts and literal bits
at the same three sizes; no runtime speedup is inferred from construction timings.
A native Apple M1 Release run measured 428/392/397 ns per global load and
317/321/333 ns per literal boolean store at 4,096/16,384/65,536 accesses of each kind.
Each load emits two instructions and two registers; each literal boolean store
emits two instructions and one register, with no runtime comparison.
Primitive unary and binary value construction now transfers its validated operand
vector directly into the instruction, removing the second validation and vector copy.
The expression benchmark constructs 4,096/16,384/65,536 identical adds through both
explicit opcode and typed-expression APIs. It checks instruction/register counts and
all immediate values; timings are informational. Direct comparison branches emit
one branch with no comparison-result register, checked by the expression tests.
Integer conversion writes directly to the target register when a single extension
is sufficient; signed-to-wider-unsigned conversions retain both required extensions.
Tests assert the resulting opcodes and register/instruction counts.
Expression stores only scalar numbers or a register reference, rather than the full
Operand union. On native Apple M1 this reduced `sizeof(Expression)` from 152 to 56
bytes and `sizeof(CallArgument)` from 168 to 128 bytes. The benchmark reports these
sizes. At 65,536 adds, a local before/after run measured 383/389 ns per typed add
(checksums both 5,505,024 including the paired raw adds); this establishes storage
reduction, not a construction-throughput improvement or execution speedup.

The benchmark also measures named-block creation and repeated suffix splits/merges
at all three sizes. It checks block counts, name checksums, instruction identities
and operand-storage addresses. A local native M1 Release before/after run produced:

| Named blocks | Before | With name index | Name checksum |
| --- | ---: | ---: | ---: |
| 4,096 | 20.6 ms | 1.16 ms | 35,754 |
| 16,384 | 252 ms | 4.86 ms | 152,730 |
| 65,536 | 4,981 ms | 20.0 ms | 644,250 |

At 65,536 edits, splitting took 18.7 ms and merging 4.43 ms, with checksum
2,752,512 and unchanged instruction/operand addresses. These are single local
measurements of IR construction and editing, not whole-program JIT throughput.

Simplify uses stable list positions instead of copying a flat instruction body.
Scratch-register lookup uses typed hash keys rather than formatted operand strings;
unchanged operands avoid vector copies, validation and revision updates. Repeated
equal constants reuse scratch registers while retaining each emitted definition,
matching C traversal semantics. The benchmark checks 4,096/16,384/65,536 adds:
each becomes three instructions and only two scratch registers are needed for all
occurrences. It verifies every immediate, instruction count and register count.
Timings are a native pass baseline, not a whole-program JIT speedup.
A local native M1 Release run measured 2.35/9.59/39.84 ms, or 575/585/608 ns
per add at those sizes, including pass-manager validation. The largest case has
196,609 instructions, 65,538 registers and immediate checksum 2,752,512.
Literal-pool provenance adds one pointer to Memory, increasing the current M1
Operand and CallArgument sizes to 120 and 136 bytes; Expression remains 56 bytes.
That storage cost buys checked expression-initializer semantics across cloning.

## Original C regression found during migration

Simplify could replace `MULO`/`MULOS` by `MOV` for a multiplier of one while a
following overflow branch still used its flags. That leaves the branch without its
producer. The C reference, the earlier C++ optimizer and the canonical pass retain
the multiply when a branch observes its flags through allowed intervening moves.
The earlier runtime frontend uses the same correction for raw interpreter entries;
session regressions cover repeated execution without the C reference enabled.
This is an intentional correction of the reference behavior, covered by an
independent regression and O0–O3 interpreter/JIT checks. Canonical simplify also
reports stack-size arithmetic overflow and materializes a zero va-list address
instead of reproducing C signed-overflow/assertion failures on those inputs.
GVN now retains computations whose overflow flags are still observed instead of
inserting a constant move between producer and branch. The same guard applies to
the reference and earlier C++ middle end; the regression exercises the full path.
Output-memory address calculations also move before a live overflow producer,
leaving only register-source stores between the producer and branch. Rule fixtures
cover every overflow opcode with indexed/displaced memory; session tests cover
raw interpretation and optimized JIT writes to displaced stack storage.

The structural-edit execution tests exposed a use-after-free in the original C
`jump_opt`: when O2/O3 emptied an address-taken block, it could delete the label
still referenced by `LADDR`. The stage observer then crashed while dumping that
operand. The C reference now retains every instruction label operand and both
label-data anchors before removing empty blocks. This also covers property branches.
`tests/Reference/empty-addressed-label.mir` and the native structural fixtures run
through the independent C interpreter/JIT at O0-O3. This is an explicit correction
of undefined behavior, not an optimization heuristic copied into the new model.

A single Release run on Apple M1, Apple clang 21, native arm64 (2026-10-14)
produced the following measurements. They are microbenchmarks, not whole-program
JIT speedups; rerun on the deployment platform before making performance claims.

| Operation | Count | Total time | Per rewrite |
| --- | ---: | ---: | ---: |
| Cached CFG query | 2,000 | 11.1 us | - |
| Rebuilt CFG query | 2,000 | 84,893 us | - |
| Operand rewrite | 4,096 | 294 us | 71.8 ns |
| Operand rewrite | 16,384 | 1,085 us | 66.2 ns |
| Operand rewrite | 65,536 | 5,413 us | 82.6 ns |

Cached and rebuilt CFG checksums both equal 510,000. Rewrite checksums equal
42 times the instruction count, and the final functions pass verification.

For the text benchmark with 65,537 instructions, 65,536 anonymous registers and
2,337,145 bytes of input, a single local before/after measurement reduced parse
time from 150.9 ms to 70.6 ms when moving literals out of tokens. Both runs produced
checksum 2,752,512. The after run wrote the text in 60.6 ms; its complete benchmark
process reached 167 MB peak resident memory. No before memory measurement was
recorded. These timings are informational and do not measure total JIT performance.

The binary benchmark's first local Release run used the same 65,537 instructions
and checksum 2,752,512. Its compressed output was 705,301 bytes, with 129.8 ms
writing and 63.2 ms reading. The process measured 240 MB peak resident memory;
it retains the source, text-decoded and binary-decoded Contexts simultaneously.
No binary before/after speedup or memory reduction is claimed from this baseline.

## Native package smoke test

Build the replacement libraries and install to a temporary prefix. Configure
`tests/Consumer` with that prefix in `CMAKE_PREFIX_PATH`. Its project enables only CXX
and tests both static and shared libraries, including text and binary roundtrips,
without source-tree include directories.
The package currently exports the implemented IR/analysis/pass infrastructure, not
an assertion that the execution architecture migration is complete.

The Debug baseline before this change passed all 62 existing tests on native Apple
arm64. Compilation concurrency remains two; Rosetta and C2MIR are outside this work.

## Local validation

This migration increment passed the following checks on native Apple arm64:

- Debug and Release complete suites: 118/118 in each configuration, including
  the owning canonical pipeline, machine preparation and independent C
  stage/execution comparisons.
- ASan with the C reference disabled: 37/37 (`detect_leaks=0` on macOS).
- Thirty-five native linking/helper/session cases cover packed and reference data,
  independent ownership, typed imports, expression initialization, ABI helpers,
  persistent globals, integer/F64 stack arguments, hardware effects, alias and
  signature-aware entry caching, and observer/publication/builtin failure paths.
  The independent session oracle checks 2,901 assertions against C interpretation
  and JIT at O0–O3. The final cache changes also passed seven focused Debug checks;
  the final builtin null-state diagnostics are exercised by the native suite.
- Eleven native machine cases cover independent ownership, stable instruction
  identity, checked rewrites, typed operands and symbols, hardware bindings,
  stack slots, CFG invalidation and every public opcode at O0-O3. Four independent
  C comparison groups check prepared instructions, ordered CFG edges, all target
  register properties, inlining/address fixtures and the original MIR corpus.
- Eight additional native machine-analysis cases cover ordered duplicate uses,
  private instructions, loop weights, compact scans, call/global physical masks,
  invalidation, source destruction, module-wide bindings and deep graph/loop cases.
  The same four independent groups additionally compare graph and liveness facts.
- Eight further native machine cases cover checked instruction navigation,
  AArch64 target lowering and its ABI/offset boundaries, source lifetime,
  repeat invocation, typed builtin prototypes and invalid lowering inputs.
  A fifth independent C group compares AArch64 target-lowering instructions,
  matching decisions, legalization, ordered CFG edges, call-register sets and
  function state at O0-O3. A sixth group records the original C multi-memory
  fallback defect. Seven native legalization cases cover every shared pattern,
  boundaries, reload order, reuse, aliases, switch inputs and phase diagnostics.
- Ten native allocation-preparation cases check coalescing, raw/compressed live
  ranges, call/early-clobber effects, typed compact scans, creation order, hardware
  binding and stale/foreign diagnostics. The fifth independent C group compares
  all four machine stages and raw/compressed range metadata at O0–O3.
- Nine additional native cases cover priority assignment, typed locations, aligned
  and reusable stack storage, hardware/global exclusions, exact splitting profits
  and rejected analysis combinations. The fifth independent C group now includes
  assignment metadata; a seventh group retains branch/loop allocation pressure at
  O0–O3 without middle-end folding.
- Nine allocation-rewrite cases cover stable identity, typed physical/memory
  operands, local addresses, reload ordering, floating temporaries, large offsets,
  repeated call preservation, empty anchors and rejected analysis/phase inputs.
  C comparison groups now include rewrite and preservation metadata.
- Nine edge-preservation cases cover actual assignment gaps, critical splitting,
  label aliases, duplicate switch edges, retained incoming order, immutable range
  ownership and stale/foreign/forged diagnostics. An eighth C group requires actual
  placements and new blocks, preventing an empty comparison from passing coverage.
- Fourteen post-allocation cases cover combine, property stripping and DCE on
  owning nodes. The ninth C group compares surviving instructions, boundary sets
  and deaths at both stages, with mandatory nonempty transformation coverage.
- Ten frame cases cover owning AArch64 layout and insertion, with a tenth C
  comparison group checking identical post-allocation input and frame output.
- Eleven encoding cases cover owned AArch64 artifacts, symbol/label relocation,
  range diagnostics and C-free generated-code execution. An eleventh C group
  compares final bytes, observable label offsets and absolute relocation slots.
- Twelve native pipeline cases cover original level/phase selection, actual
  CFG/SSA/loop views, the pre-pruning ADDR decision, source/observer lifetime,
  repeated optimization and moves, module-only isolation, cross-module inlining,
  diagnostics, cache observation, precise CFG invalidation, closed PHIs, nested
  stack scopes, every public opcode and both binary encodings.
- Seven native jump cases cover stable identities, prohibited threading,
  label/data anchors, historical address roots, artificial entry edges,
  independent/repeated analysis, ownership/phase errors, 4,096 adjacent labels,
  every public opcode and format roundtrips. Independent stage comparisons check
  ordered predecessors/successors and root flags; seeded execution checks returned
  values and global bytes through the interpreter and O0-O3 JIT.
- Eight native SSA combine cases cover comparison truth directions, both target
  address predicates, checked memory edits, stable instruction/use identities,
  hardware bindings, malformed definition cycles, invalid phases and ownership,
  every public opcode and text/binary roundtrips. Forty-three independent SSA/jump
  comparison cases include isolated and combined pipelines, global storage bytes,
  NaN behavior, original regressions and fixed-seed programs.
- Thirteen native SSA cleanup/motion cases cover ordered inverse-use transfers,
  recursive deletion, overflow barriers, wide and cyclic PHIs, conditional and
  property preheaders, repeated LICM, independently consumed analysis caches,
  live-edge indexing, reconstructed dominance, stable identity, ownership, invalid
  phases and text/binary roundtrips.
- Seven copy/memory cases cover rule boundaries, seeded loops, owned metadata
  transfer, phase errors, atomic hardware coalescing, inverse SSA sites, byte
  overlap across the signed displacement boundary and every public opcode.
- Seven native inlining cases cover phase ordering, independent ownership,
  hardware reuse, threshold/frame/cold-code/relocation fixtures, callback errors,
  block movement, frame-overflow errors and checked operand storage transfer.
  Independent C stage comparison also includes every original MIR regression.
- Replacement layers include 15 IR cases and ten text cases per linkage,
  fourteen analysis/pass cases and the architecture dependency guard.
- Thirteen scope/Builder cases per linkage cover conditional exits, loops, repeated
  switch targets, sibling scopes, mixed and homogeneous indirect targets, return
  memory reads, explicit operations, invalid construction, 4,096 nested scopes,
  independent lifetimes and read-only format materialization.
- Fourteen call DSL cases per linkage cover direct/indirect and inline calls,
  multiple scalar results, all block classes, RBLK, variadic extraction, typed imports,
  forward definitions, jump conventions, failure atomicity and bulk handle stability.
- Fourteen variable cases per linkage cover scalar storage widths, ordered items,
  binding identity, readonly and imported permissions, boolean normalization,
  atomic failures, relocations, expression purity and independent clone lifetimes.
- Fourteen expression cases per linkage cover opcode selection and encoded results,
  all scalar conversion pairs, direct branches without temporary registers, literal
  bits, atomic failures, ownership, typed boundaries and independent clone lifetimes.
- Ten binary cases per linkage cover exact opcodes, floating bits, all memory tag
  forms, block ABI, hardware identity, cross-module references, sparse labels,
  malformed wire records, truncation and compression block/checksum boundaries.
- Release build with tests and the C reference disabled.
- Installed CXX-only consumer, static and shared: 2/2. Configuration succeeds with
  an intentionally nonexistent C compiler; no source-tree header path is provided.
  The static consumer links `MIRNext::Transforms` and `MIRNext::Execution` and exercises
  the installed owning optimization API, function lookup, text/binary output,
  machine preparation, verification, CFG, loop analysis, register liveness and
  AArch64 target lowering, operand legalization, coalescing, live ranges, assignment
  allocation operand rewrite, edge preservation and post-allocation optimization
  through their exported dependencies. It also uses the unified compilation entry
  and publishes/executes a newly built constant function on arm64, then calls the
  new owning session twice to check host import resolution and persistent globals.
- The standalone C oracle records O0–O3 stages, checks all public opcodes and 16
  regression files in native text/binary output, and executes loop and layout fixtures
  through interpretation and JIT. It also executes conditional, switch and indirect
  stack-scope fixtures at O0-O3, checking both returned values and stack restoration.
  Eight native DSL call fixtures run at O0-O3 through text/binary input and both C
  execution modes: direct/indirect, block, RBLK, scalar/block varargs, multiple results
  and inline calls. This is an independent execution oracle, not new-model native JIT.
  Nine variable fixtures also execute at O0-O3 through text/binary and interpreter/JIT:
  initialized/BSS/readonly/bool/floating/pointer/expression data and cross-module stores
  observed through an exported getter, plus signed/unsigned narrow loads and stores.
  The multi-module fixture checks both module
  records; source binding metadata is validated natively before serialization.
  Eleven expression fixtures execute at O0-O3 through text/binary and interpreter/JIT,
  checking unsigned high bits, narrow extension, truncation, floating conversions,
  NaN, branching, memory, Boolean raw writes, calls and expression initializers. When the original C
  binary reader rejects raw long-double locals on a double-width long-double target,
  execution uses the C-written canonical binary for that fixture, explicitly logged.
  Native binary construction and decoding, the original rejection and C-written
  binary compatibility remain checked; native text executes the long-double fixture.
  The new reader imports C-written files and
  reproduces their compressed bytes, including a 40,001-instruction multi-block
  compression fixture. C reads the native re-encoding too. Operand snapshots
  compare types, constant bits, memory addresses, hardware bindings, symbol identity,
  register reference relationships and label-anchor positions.
  New-core IR/format libraries have no undefined C MIR
  engine symbols.

The maintained platform workflows include the installed native consumer checks.
Those CI jobs have not been executed locally. These results establish the new
infrastructure's baseline; they do not satisfy the remaining pipeline migration
and final architecture exit conditions.

## Stack-scope materialization

`IRBuilder::begin_stack_scope` opens a scoped block at the current insertion point;
`end_stack_scope` opens its parent continuation, or clears the insertion point after
a function exit. `create_block` on the Builder retains its current scope. Explicit
graph construction can use function-owned `StackScope` objects and assign them to
blocks; entering a scope through any edge initializes every newly entered ancestor.
Scope metadata, ordinary iteration and diagnostic dump never synthesize instructions.
See `examples/native-stack-scope.cpp` for an independently built new-model example.

`materialize_stack_scopes` mutates canonical IR directly and keeps existing node
identities. `materialize` clones a Context first. The pass manager provides the same
operation through `MaterializeStackScopesPass`, with observation and analysis
invalidation. Retired scope handles remain inspectable; subsequent materialization
does nothing. No scope marker enters the text or binary protocols.

Each edge restores scopes from the inside out, then saves newly entered scopes from
the outside in. Same-scope backedges do neither. Conditional fallthrough and taken
edges are handled independently, and repeated switch destinations share cleanup
blocks. Indirect targets retain their original addresses; mixed transitions use
dispatch blocks and evaluate the target once. Homogeneous transitions avoid dispatch
when they match the unknown-target exit cleanup too.
Memory return values and indirect exit targets are loaded before restoring the stack.
Explicit `BSTART`/`BEND` stay in their caller-specified order. Overflow flags cannot
cross a scope transition that inserts stack operations; verification diagnoses this
before mutation, just as it rejects an explicit intervening `BSTART` or `BEND`.

Block insertion uses stable list positions. Scope ancestry collection visits each
ancestor once, and the lowering uses iterative parent walks, including deeply nested
scopes. The Release benchmark reports 4,096/16,384/65,536 alternating scoped blocks,
checking one save and restore per block; timing is not a pass/fail threshold.
One native Apple M1 Release run measured 1.07/3.83/15.83 ms at those sizes
(262/233/242 ns per block). This is a scaling baseline, not a whole-JIT speedup.

## Native call DSL

`call(function, {20, 22})` and `call(typed_import, arguments)` infer the signature
from the callee. `call_indirect(signature_symbol, target, arguments)` supports a register,
memory or symbol target; it always creates `CALL`. `inline_call` selects `INLINE`.
The format layer emits the required prototypes without changing the source module.
Untyped external symbols require an explicit signature symbol. Cross-module calls
refer to a caller-owned import, whose signature is checked against an exported
definition by Context verification.

`CallResult::results()` returns all scalar result registers. `value()` returns a
diagnostic unless there is exactly one result. Result references and the instruction
retain identity through subsequent construction; cloning remaps them to the new
owner. Invalid arguments publish no registers or instructions and retain the
Builder's first diagnostic.

`BlockArgument(address, size, classification, offset)` represents an ordinary block;
`BlockArgument::return_buffer(address, size)` represents the leading RBLK parameter.
`start_varargs`, `next_vararg`, `next_vararg_address`, `next_vararg_block` and
`end_varargs` avoid collisions with C variadic macros. Scalar extraction loads the
value; address extraction exposes the MIR `VA_ARG` result. Block extraction copies
into caller-provided storage and takes a checked block Type. Callers must promote
variadic floats to double, as required by the existing IR validation rules.
`jump_call`/`ret_to` explicitly select the separate void, parameterless jump ABI.
See `examples/native-calls.cpp`; it uses only the replacement libraries and DSL.

## Native module variables

`create_variable(name, type, optional_initial, writable)` immediately creates one
ordered data item, using BSS when the initializer is absent. Scalar widths follow
MIR storage: booleans and pointers use eight bytes, and long double uses the native
platform width. There is no module-slot operand or secondary initializer body.
A pointer initial value may be a typed symbol relocation. The overload taking a
Function creates checked expression data; calls, memory reads and writes in an
expression initializer are rejected by the common initializer validator.

`IRBuilder::load`, `store` and `address_of` accept a `GlobalVariable`. Each multi-
instruction access validates the complete sequence before publishing registers or
instructions. Readonly stores, foreign owners and invalid result names fail atomically.
Boolean values load as 0/1, including values changed through a raw address. Literal
boolean writes materialize at construction time; dynamic writes use an integer test.
Address-taking remains an explicit escape to the low-level memory API; binding
permissions are Builder checks, not operating-system memory protection.

`import_variable` creates a caller-owned data import; Context verification checks its
type and write permission against an in-Context exported definition. Readonly imports
may refer to mutable storage. External host variables may remain unresolved until
the execution layer links them. `find_variable` retrieves the stable binding, and
cloning preserves bindings without retaining source objects.

The existing text/binary protocols encode data, imports, relocations and instructions;
they have no variable-binding or readonly metadata record. Native cloning preserves
this metadata, while reading a file produces ordinary data symbols. `bind_variable`
adds a checked typed binding to a parsed scalar/BSS/reference/expression symbol or
untyped data import without changing its item position or storage. See
`examples/native-variables.cpp` for a new-model-only cross-module example.

## Native expressions

`builder.literal(20) + 22` produces a typed `Expression`; `expression(reg)`,
`expression(memory)` and `expression(variable)` incorporate existing registers or
emit a checked load. Operators emit eagerly into the Builder's current insertion
block. An Expression is a Builder-bound view of a scalar immediate or register,
not an owning AST, and must not outlive its Builder or Function.
Boolean memory loads normalize nonzero raw contents to 1, including writes through
an escaped address. `literal(true)` and `literal(false)` infer the Boolean type.

Arithmetic, bitwise, shift and comparison operators select the MIR family from
the operand types. I8/I16/I32 use 32-bit operations; their registers retain MIR
scalar semantics, not automatic 8/16-bit arithmetic truncation. Unsigned types
select unsigned division, remainder, right shift and ordering. Comparisons and
logical negation produce Boolean values. Integer literals beside a floating
expression adopt its precision; floating literals must already match precision.
Plain integer literals infer I64/U64; narrower types use `literal(type, value)`.
Expressions from different Builders or different types require explicit handling;
`convert(type)` performs checked conversion, including source extension and target
narrowing. Conversions publish their entire sequence atomically.

MIR floating-to-integer conversion truncates to signed 64-bit representation;
conversion to a narrower integer then applies its extension rule. Full-range
floating-to-u64 conversion is unsupported and returns a diagnostic. Pointer
conversions accept I64/U64 or Boolean tests. The low-level opcode API remains
available for exact MIR operations.

`assign`, `store`, `ret`, `branch_if` and call arguments accept expressions with
checked types and ownership. `branch(Comparison, lhs, rhs, label)` emits the direct
comparison branch without a Boolean temporary. There are no overloaded `&&` or
`||` operators implying short-circuit execution; use explicit control flow.
Invalid expressions preserve the module's first diagnostic and are reported by
`value()` and the Builder's Result-returning boundaries. See
`examples/native-expressions.cpp` and the installed CXX-only consumer.

## Text protocol behavior

Text writing is read-only and separate from diagnostic dump. It normalizes register
and label names, generates call prototypes, and moves expression functions before
their initializers to satisfy the original reader. Hoisted metadata and functions
retain data-run boundaries through empty prototype records; data order is preserved.
Symbol and block-address displacement expansion declares its temporaries before labels.
Block ABI sizes remain distinct from address displacements in both formats. Arbitrary IR
attributes and read-only bindings have no record in the existing protocol.
Integer register widths retain MIR's scalar register semantics.
The text grammar represents integer immediates as signed 64-bit bit patterns and
has no unsigned operand tag. Text writing canonicalizes unsigned immediate bits
to that form; binary encoding still preserves the exact signed/unsigned operand
mode. Typed expression literals use the canonical bits, with signedness carried
by the Expression type and selected opcode.

Decimal floating literals use locale-independent parsing. Signed zero and canonical
NaN/infinity roundtrip; noncanonical NaN payloads return a diagnostic instead of
losing bits. Empty strings consistently include a terminator; the old reader's empty
string behavior and inability to read its own nonfinite output are not replicated.
Native text keeps exact public long-double opcodes; on Windows and platforms with
eight-byte long double, the independent C oracle canonicalizes these to double as
its source specifies. The comparison records this mapping rather than skipping them.

The C oracle reads every generated public-opcode fixture and regression file, then
executes loop and data-layout fixtures with interpreter/JIT at O0–O3. On native Apple
arm64, a label-difference fixture involving a removed same-position label makes the
original C O2 JIT abort after encoding; it retains construction, native roundtrip and
C-reader coverage. The executed layout fixture refers to the retained label. This
does not establish a native JIT implementation for the replacement object model.

## Binary protocol behavior

`write_binary` defaults to compressed MIR v1. `BinaryEncoding::Raw` exposes its
uncompressed records for tools and tests. `read_binary` returns an owning Context
and takes limits for bytes, strings and instructions; malformed records return
source/offset, module and function diagnostics before execution. Labels are allocated
per module across all its functions, without dense allocation based on wire IDs.
Symbol and block-address displacements are materialized without modifying source IR.
Integer data is truncated to its declared width as in the original format; pointer
bits serialize as unsigned 64-bit data. Names need not be text identifiers. Metadata
without a protocol representation follows the same normalization as text output.

Binary floats retain signed zero and NaN payloads. Long-double payloads use the
existing host-dependent 16-byte protocol slot with deterministic padding, without
claiming cross-target conversion. The native reader preserves exact public types
and opcodes. On targets where C canonicalizes long double to double, the original
C binary reader leaves raw `ld` locals uncanonicalized and rejects some such files.
The oracle records this specific C diagnostic, retains all native opcode encoding
and roundtrip checks, then checks every C-canonicalized opcode through the native
reader/writer and back through C. This limitation does not suppress native coverage.

Compression preserves the original dictionary selection and checksum algorithm.
Decoding rejects overlapping/uninitialized dictionary references instead of copying
the C decoder's undefined behavior. Whole-buffer byte APIs have C++ ownership and
no untyped callback context. The pre-rewrite runtime's streaming codec remains until
that runtime is replaced; it is not a dependency of these new format entry points.
