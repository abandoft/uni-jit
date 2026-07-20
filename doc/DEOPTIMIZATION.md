# Deoptimization metadata and frame reconstruction

UniJIT attaches immutable deoptimization metadata to each compiled function.
The current contract reconstructs a frontend frame after generated code has
restored its native frame and returned through the ordinary ABI boundary. It
does not unwind through generated code or claim on-stack continuation.

## Records and recovery operations

Every active runtime guard has a stable site identifier. A
`DeoptimizationRecord` maps that site to a semantic reason, a frontend-owned
resume offset, and an ordered recovery program. The public reason vocabulary
currently covers generic guard failure, division by zero, type mismatch, and
invalidated assumptions.

Recovery operations materialize typed logical slots from one of three sources:

- an original native-entry argument;
- an immutable value-bits constant;
- the exact value bits captured by the failing guard.

Slots are frontend identities rather than physical registers or stack
locations. Duplicate slots and duplicate sites are rejected. Compilation also
rejects argument references outside the function signature and metadata sites
that do not identify a runtime guard. When optimization proves a guard cannot
fail and removes it, its metadata is omitted from the compiled function.

## Exit and reconstruction sequence

The reference interpreter and every native backend preserve the same sequence:

1. A failing guard records `ExitReason::kRuntime`, its stable site, and the
   exact triggering value bits in the caller-owned `ExecutionContext`.
2. Native code restores its complete frame and returns normally.
3. `CompiledFunction::invoke` reports `StatusCode::kRuntimeExit` at that site.
4. The frontend calls `reconstruct_deoptimization` with the same arguments and
   execution context.
5. Reconstruction verifies that the context describes the requested site,
   materializes the typed slots, and returns the semantic reason and resume
   offset.

This ordering rejects stale or mismatched contexts. One execution context is
owned by one invocation at a time, as specified in
[RUNTIME.md](RUNTIME.md).

`CodeHandle` exposes the same record lookup and reconstruction API. Because a
handle is an immutable execution lease, metadata remains available after cache
replacement, invalidation, eviction, clearing, or cache destruction for as
long as that handle is retained.

Runtime assumptions use the same records with
`kAssumptionInvalidated`, default recovery of every typed entry argument, and a
quiescent invalidation protocol described in [ASSUMPTIONS.md](ASSUMPTIONS.md).

## Frontend policy and current boundary

PocketPy numeric division in both straight-line SSA and counted-loop CFG code
records all source parameters plus the guarded divisor. Its adapter raises
`ZeroDivisionError` only after successful frame reconstruction identifies a
division-by-zero record and a zero Float64 trigger; an unknown runtime exit
becomes a diagnostic runtime error instead of being silently misclassified.

The current recovery vocabulary is sufficient for entry-argument
specializations and language exceptions at the ABI boundary. Broader optimized
tiers still need stack maps for arbitrary live SSA values, materialized object
state, interpreter-frame installation, and resumable transfer into the stock
runtime. Those additions can extend recovery sources without exposing target
register layouts to frontends.
