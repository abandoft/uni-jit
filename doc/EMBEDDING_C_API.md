# C17 embedding API

`<unijit/embedding.h>` is the stable commercial shared-library boundary for
embedding UniJIT without exposing a C++ object layout, standard-library type,
exception, compiler-specific name, process-local callback, or native entry
address. The first ABI is identified by `UNIJIT_C_ABI_VERSION == 0x00010000`
and every exported entry point is prefixed `unijit_v1_`.

This first delivery exposes the scalar straight-line compilation floor. It is
independent of SLJIT and calls UniJIT's own verified IR, optimizer, target
preflight, native encoders, W^X publisher, execution context, code cache,
generation-safe fast calls, and data-only patch cells.

## Compatibility contract

- The ABI requires 64-bit pointers, signed 64-bit value-bits words, and a
  supported little-endian AArch64, x86-64, or RISC-V 64 host.
- Public scalar types have fixed widths. Public structures use only fixed-width
  integer fields, begin with `struct_size`, and reserve zeroed space for
  compatible extension. They contain no `size_t`, C `long`, enum-typed storage,
  pointer-sized integer, bitfield, or compiler-dependent aggregate.
- Callers initialize input structures with the matching `*_init` routine and
  leave every reserved field zero. Output structures that have no initializer
  are zeroed by the caller and receive `struct_size = sizeof(structure)` before
  the call.
- A v1 runtime reads and writes only the v1 prefix. A too-small structure,
  unknown discriminator, nonzero reserved field, unsupported target profile,
  or out-of-range count fails before compilation or publication.
- `Word` values use `unijit_word_v1`. `Float64` values cross invocation as the
  exact 64 value bits in `unijit_word_v1`; portable C code transfers those bits
  with `memcpy`, not pointer aliasing or a numeric cast.
- The shared library exports exactly the symbols committed in
  `tool/c_api_v1_symbols.txt`. The C++ API remains a source/static integration
  surface and is not a stable shared-library ABI.

Changing the meaning or layout of a v1 type, removing a v1 function, changing
ownership, or weakening a failure rule is an ABI break. A future incompatible
surface must use new opaque types and a new symbol prefix.

## Status and diagnostics

Operations return `unijit_status_code_v1`. A call may receive a null error
output when the caller needs only the code. Otherwise the error output must
point to null on entry. On failure, a non-null `unijit_error_v1` owns a status,
an operation-specific unsigned detail value, and a UTF-8 diagnostic limited to
1,024 bytes. The caller destroys it with `unijit_v1_error_destroy` before
reusing the output variable.

All C entry points catch allocation and implementation exceptions. Exceptions
never cross the language boundary: allocation failure becomes
`RESOURCE_EXHAUSTED`, expected engine failures retain their typed status, and
an unexpected exception becomes `INTERNAL` with a bounded generic diagnostic.
No routine uses a global last-error slot.

## Ownership and state transitions

Every opaque owning handle accepts null in its destroy function. Destruction is
idempotent only with respect to a null pointer; destroying the same non-null
pointer twice is invalid C ownership.

| Object | Creation | Terminal transfer or release |
|---|---|---|
| error | returned by a failed call | `unijit_v1_error_destroy` |
| builder | `unijit_v1_builder_create` | `builder_finish` consumes its mutable implementation; destroy the shell afterward |
| function | successful `builder_finish` | `unijit_v1_function_destroy` |
| compiler | `unijit_v1_compiler_create` | `unijit_v1_compiler_destroy` |
| compiled function | successful compile | direct destroy, or cache publication consumes `*compiled` and sets it to null after argument validation |
| execution context | `unijit_v1_execution_context_create` | `unijit_v1_execution_context_destroy` |
| code cache | `unijit_v1_code_cache_create` | `unijit_v1_code_cache_destroy` retires lookup ownership but not retained handles |
| code handle | publication or lookup | `unijit_v1_code_handle_destroy` |

A builder is single-use. After `builder_finish`, or after an internal exception
invalidates its implementation, further mutation fails. Value, fast-call, and
patch-cell tokens are builder-local identifiers and cannot be mixed across
builders.

Cache publication deliberately takes a pointer to the compiled-function
pointer. Once argument validation succeeds, publication consumes the compiled
function whether publication succeeds or fails and stores null in the caller's
variable. On success it returns a generation-stable code handle. Invalidating
or clearing a cache removes future lookup visibility while retained handles,
patch-cell access, and fast-call bindings keep the exact generation alive.

## Concurrency

- A builder is confined to one thread and must not overlap mutation, finish,
  or destruction.
- Functions, compilers, compiled functions, and code handles are immutable
  after publication and may be used concurrently when their owning lifetime
  covers every call.
- A code cache supports concurrent lookup, publication, invalidation, and
  statistics through the underlying bounded cache contract.
- An execution context belongs to one active invocation at a time. Another
  thread may request or clear its cooperative interrupt flag, but may not read
  or mutate exit metadata concurrently with invocation completion.
- An error object is immutable but normally stays with the calling thread.
  The pointer passed as an error output is never shared between concurrent
  calls.
- Destruction never races an operation on the same object. External ownership
  synchronization is required before the final destroy.

## Scalar builder and execution floor

The v1 builder supports typed Word and Float64 parameters and constants, scalar
unary and binary arithmetic/comparisons, nonzero guards, safepoints, typed
context-free fast-call slots, data-only patch cells, and one scalar return.
Compilation inherits explicit resource budgets and a validated target profile.
Invocation accepts a flat value-bits array and an optional execution context.

Guards and safepoints require a context for diagnosed exits. Interrupts return
`EXECUTION_INTERRUPTED`; other managed exits return `RUNTIME_EXIT`. The context
retains the latest reason, site, value, and optional poll count after the native
frame has returned through its ordinary ABI boundary.

Fast-call slots accept only exact Word/Float64 signatures and currently bind
context-free targets. Unbound slots, incompatible signatures, incompatible
target profiles, or targets that require a context fail closed. Bindings retain
the exact target generation even after its cache entry is invalidated.

Patch-cell loads use the function-owned non-executable cell array. Publication
changes data with the underlying acquire/release contract and never rewrites an
RX code page. The current C surface exposes read and release publication; the
broader C++ compare-exchange/fetch-add surface remains outside this ABI version.

## Qualification and deliberate exclusions

`test/embedding_test.c` is compiled as C17 and freezes public aggregate sizes
and offsets. It covers ABI negotiation, malformed structures, bounded
diagnostics, resource exhaustion, Word and Float64 execution, signature errors,
cooperative interruption, patch-cell publication, cache lookup/invalidation,
retained generations, fast-call binding, and ownership transitions.
`test/package/embedding.c` repeats scalar execution using only the installed
header, library, and CMake package.

The dedicated shared-library workflow builds and executes the C consumer on
real Ubuntu x86-64, Windows x86-64, and Apple AArch64 hosts, then rejects a
missing or unexpected v1 export and any exported C++ implementation symbol.
The ordinary sanitizer and platform matrices also compile and run the C17
qualification target.

This delivery does not claim the complete embedding roadmap. CFG construction,
typed memory regions, trusted objects, atomics, SIMD values, assumptions,
deoptimization/materialization, tiering, scheduling, portable IR packages, and
trusted AOT objects remain unavailable through v1 and therefore cannot be
smuggled through an unchecked pointer or internal C++ layout. The next
serialization milestone adds canonical, length-delimited, resource-limited
portable IR packages without expanding this ABI through raw in-memory graphs.
