# C++ memory ownership and executable-code services

IR, analyses, optimized programs and loaded state use owning C++ storage.
`Context` does not accept a per-context general-purpose allocator. The public
customization point for native code mapping and publication is
[`execution::CodeMemoryProvider`](include/mirnext/Execution/CodeMemory.hpp).
Replacing that service does not replace allocations made by standard containers.

## Providing code memory

Derive from `CodeMemoryProvider` and implement these operations:

| Operation | Contract |
| --- | --- |
| `page_size() const` | Return the mapping/protection page size, nonzero and divisible by 16 |
| `allocate(size)` | Return `Result<MappedCodeMemory>` with a non-null, page-aligned region of exactly the requested size |
| `release(mapping)` | Release the entire original mapping; return `Result<void>` |
| `make_writable(range)` | Permit writes to this page-aligned allocation or subrange |
| `make_executable(range)` | Complete executable protection for the range |
| `flush(range)` | Perform the target/OS instruction-cache synchronization required before execution |

The provider must support independent concurrent mappings. Protection and flush
can receive page-aligned subranges; release always receives the original whole
mapping. Implementations report failures through `Result`, without exceptions.
Keep architecture/OS details in the provider rather than exposing them through
IR or code-generator object fields.

Use `native_code_memory_provider()` for the normal OS service, or delegate to
it when adding accounting or policy. It handles platform mapping, protection
and cache operations. A provider should establish executable protection even
when a caller is attempting to close a failed write interval.

## Session integration and lifetime

Pass a `shared_ptr<CodeMemoryProvider>` in `LinkOptions::memory_provider` before
creating a session. For example, in a function that has `context` and `provider`:

```cpp
mirnext::execution::ExecutionOptions options;
options.link.memory_provider = provider;
options.link.compilation = mirnext::execution::JITCompilation::Lazy;
auto session = mirnext::execution::ExecutionSession::create(context, std::move(options));
```

The complete [metered-provider example](examples/custom-code-memory.cpp) builds
and executes with this API. It delegates OS operations, tracks mappings with
atomic counters and demonstrates first-call compilation after the source Context
and session have been destroyed. The retained FunctionHandle owns the loaded
program and provider; destroying the last handle releases its mappings. The
example checks both the returned value and that mapped bytes return to zero.

Native interpreter ABI entries, host-call adapters and JIT functions use code
memory too. A session's execution mode alone does not imply that no executable
memory is needed. A callback-only interpreted program can avoid publishing native
entries when its selected operations do not require native calls.

## Lower-level publication

`CodeMemory(provider).reserve(sizes)` creates a move-only `CodeReservation`.
The service packs 16-byte-aligned slots into an immutable batch, rounds the
mapping to whole pages and exposes each final address before publication.
This permits relocations to use the actual code base.

`CodeReservation::publish(buffers)` checks all lengths before writes, opens
write access, copies bytes, establishes executable protection and flushes the
instruction cache. Result bookkeeping is allocated before opening write access.
Success consumes the reservation and returns owning `ExecutableCode` handles.
Failure returns no executable handles; the reservation retains the mapping and
can retry. A failed attempt can have copied bytes, so it does not promise byte
rollback. Keep buffers valid for the call and execute only published handles.

`CodeMemory::publish(CompiledFunction, resolver)` performs verification,
reservation, relocation and publication for one owning artifact. Its
`ExecutableFunction` retains both artifact and mapping independently of the source
IR and machine-function lifetime. See [the compiled-function example](examples/native-compile.cpp).

`CodeArena` reserves a bounded address range for incremental publication. Each
reservation owns complete pages, so later batches do not reopen previously
published pages. Published handles retain the mapping after arena destruction.
Abandoned reservations do not reclaim capacity. Arena reservation, publication
and execution require external synchronization.

## Service implementation checks

[Code-memory tests](tests/Execution/CodeMemory.cpp) exercise page rounding,
alignment, malformed provider responses, publication failures, retries, subrange
isolation, independent mappings and ownership. Session, lazy and BBV tests check
provider failure propagation and retry without falling back to another engine.
The public interface is a C++ service with shared ownership; it contains no
source IR context pointer or allocator callback table.
