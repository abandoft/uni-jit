# Immutable data patch cells

## Contract

UniJIT patch cells are compiled-function-owned mutable `Word` values stored in
naturally aligned, non-executable memory separate from published machine code.
They provide generation, shape, counter, value, and future target metadata
without making an RX page writable. No patch-cell operation rewrites an
instruction, flushes an instruction cache, or embeds a host pointer in portable
IR.

Both straight-line and CFG builders create descriptors with an initial value
and a `PatchCellKind`, then emit `load_patch_cell`. The kind is retained as
immutable metadata for runtime policy and telemetry; every current cell has
the same `Word` representation. The verifier rejects unknown kinds, undeclared
indices, non-Word loads, operands on a load, and malformed CFG nodes. An
independent `maximum_patch_cells` compilation budget defaults to 256 and is
checked before expensive lowering or executable publication.

The reference interpreters evaluate a verified patch load from its descriptor's
initial snapshot. Optimized IR retains every load as an ordered effect: dead
loads are not deleted, loads are not commoned, and a load is a value-numbering
boundary because its acquire operation may synchronize unrelated data. Runtime
mutation begins only after a `CompiledFunction` owns the cell storage.

## Publication and memory order

Generated code and `read_patch_cell` use acquire loads. Runtime mutation is
available through both `CompiledFunction` and generation-stable `CodeHandle`
leases:

- `publish_patch_cell` performs a release store;
- `compare_exchange_patch_cell` is a strong compare-exchange with
  acquire-release success and acquire failure order, and returns the observed
  value plus an explicit exchanged flag;
- `fetch_add_patch_cell` returns the previous value and uses acquire-release
  ordering.

These rules allow a runtime to initialize ordinary payload state, publish a
new generation or shape with release ordering, and have generated code observe
that payload after its acquire load. They do not make arbitrary non-atomic
concurrent mutation safe. Index errors fail without touching any cell.

`PatchCellKind::kTarget` is currently data only. It may identify or version a
future call target, but generated code cannot interpret it as an executable
address or perform an indirect call. JIT-to-JIT calls remain a separate
verified feature with signature, target-profile, lease, stack-map, and unwind
requirements.

## Native ABI and W^X boundary

Cell storage is one lock-free native `Word`. A managed invocation temporarily
binds the owning array through a private `ExecutionContext` field, calls the
immutable native entry, and restores any previous nested binding on every
normal or diagnosed return path. A function with patch cells reports that it
requires an execution context. Its public `native_entry()` is null so an
embedder cannot bypass ownership or substitute an unverified cell base.

Lowering uses a verified constant descriptor index and has no generated bounds
branch:

- x86-64 performs the naturally aligned load under its TSO acquire mapping;
- AArch64 emits an `LDAR` acquire load;
- RISC-V 64 emits a native-width load followed by an acquire-strength fence and
  does not require the optional `A` extension for generated reads.

The C++ runtime owns release, compare-exchange, and fetch-add updates through a
lock-free `std::atomic<Word>`. Cell allocation completes before executable
publication, and cell destruction occurs only with the compiled generation.
Executable memory therefore follows the existing W^X publication path and
never aliases mutable patch storage.

## Lifetime and cache replacement

A `CodeHandle` retains the same shared compiled generation as invocation and
cell mutation. Cache invalidation removes lookup visibility but does not
invalidate outstanding handles; a retained lease may continue to invoke and
update its retired generation safely. Replacement publishes a separate
compiled function and separate cell array, so new and old generations cannot
race through accidentally shared storage.

Frontends must bound polymorphic state growth outside this primitive. A shape
or target policy can replace one cell value atomically, but megamorphic lookup,
miss accounting, garbage-collector barriers, and executable target retention
remain the responsibility of the owning runtime contract.

## Qualification and performance gate

Core tests cover both IR forms, baseline and optimized compilation, reference
interpretation, verifier failures, the independent compilation limit, every
mutation API, metadata, raw-entry denial, and cache-lease retention after
invalidation. The installed-package consumer repeats the public contract using
only exported headers and CMake targets.

`unijit_patch_cell_stress` checks release/acquire payload publication and
contended eight-writer fetch-add while four generated-code readers require
monotonic observations. Its versioned JSON record also proves that an
invalidated cache generation remains safe through a retained lease. The
ordinary gate uses 512 publications and 1,000 increments per writer; extended
qualification uses 10,000 of each and repeats the integration under
ThreadSanitizer.

`unijit_patch_cell_benchmark` measures the complete managed invocation
boundary against an otherwise equivalent constant-returning compiled function,
plus release-store/acquire-load mutation round trips. Each product platform
retains seven samples of 200,000 invocations. The commercial gate rejects a
patch path above 2.5 times the constant managed-call latency or more than 128
native code bytes; emulation is supplemental to real AArch64, Ubuntu/Windows
x86-64, and RISC-V 64 evidence.
