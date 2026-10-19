# Native MIR commands

These commands use the owning C++ IR, serialization and execution libraries.
They build by default; `MIRNEXT_BUILD_UTILITIES=OFF` omits them. The independent
C oracle and C2MIR tool are not dependencies. All diagnostics go to stderr and
return a nonzero exit status. Binary stdin/stdout also work on Windows.

```sh
build/m2b < program.mir > program.bmir
build/b2m < program.bmir > canonical.mir
build/b2ctab < program.bmir > program.inc
build/mir-bin-run program.bmir program arg1 arg2
```

`mir-bin-run` keeps the original binfmt-style argument layout: its first
argument is the binary path; the second becomes the MIR program's `argv[0]`.
The program receives the remaining arguments and the host environment.
There must be exactly one `main` definition, with an integer result and one of
`()`, `(argc, argv)` or `(argc, argv, envp)`. Use `i32`/`i64` for `argc` and
`p`/`i64`/`u64` for the pointer parameters. The result becomes the process exit
status. Existing OS exit-status width rules apply.

`MIR_TYPE` selects `interp`, `gen`, `lazy` (the default), or `bbv`. Unknown modes
return a diagnostic. Interpretation publishes native entries for callbacks.
JIT modes use the native C++ O2 pipeline; lazy and BBV retain owning entries and
compile reached functions/blocks through the same execution services.

Unresolved functions load from system libraries or `MIR_LIBS`. Entries in
`MIR_LIBS` can be full paths, filenames, or bare library names. Bare names use
the platform prefix/suffix, for example `libexample.dylib` on macOS,
`libexample.so` on Linux, and `example.dll` on Windows. `MIR_LIB_DIRS` supplies
search directories before the system loader search; `LD_LIBRARY_PATH` entries
are also considered. Lists use `:` on Unix and `;` on Windows. Libraries stay
open until the execution session and its generated code have been destroyed.

The runner derives a host function's MIR signature from its typed import or
direct call prototypes. Conflicting prototypes are rejected before execution.
An address-only, untyped import has no host type contract and returns a
diagnostic. Applications needing arbitrary host bindings use
`ExecutionOptions::link.resolve` with explicit `ExternalSymbol` function/data
contracts. Import kind and ABI are not guessed from a host symbol's name.

The format commands retain the existing MIR protocol. Text normalizes signed
integer spellings; binary preserves operand modes and exact bits. Unrepresentable
text payloads return diagnostics. Input streams are buffered and limited to
256 MiB before parsing; format-layer token/instruction/resource limits also
apply. `b2ctab` validates and reencodes its input before writing a `uint8_t`
array, using bounded output buffers and no shared temporary file.

## Embed a program

Generate `program.inc` with `b2ctab`, then configure its absolute path:

```sh
cmake -S . -B build-embedded -DBUILD_TESTING=OFF \
  -DMIRNEXT_BUILD_EXAMPLES=OFF -DMIRNEXT_BUILD_C_REFERENCE=OFF \
  -DMIRNEXT_EMBEDDED_HEADER=/absolute/path/program.inc
cmake --build build-embedded --target mir-embedded-run --parallel 2
MIR_TYPE=lazy build-embedded/mir-embedded-run arg1 arg2
```

The embedded runner's own path is `argv[0]`. The selected binary remains in its
read-only array; decoding, execution and code ownership use the same C++ services
as `mir-bin-run`. No C IR reader, intrusive list, generator context or callback
bridge is involved. Tests generate an array during the build, compile that
actual array and execute it in all four modes.
