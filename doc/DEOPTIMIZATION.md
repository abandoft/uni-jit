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

Recovery operations materialize typed logical slots from one of four sources:

- an original native-entry argument;
- an immutable value-bits constant;
- the exact value bits captured by the failing guard.
- an arbitrary Word or Float64 SSA value captured from the guard's canonical
  stack map.

Slots are frontend identities rather than physical registers or stack
locations. Duplicate slots and duplicate sites are rejected. Compilation also
rejects argument references outside the function signature, metadata sites
that do not identify a runtime guard, capture values that are unavailable or
have the wrong type, and CFG captures whose definitions do not dominate the
guard. Frame-state values are explicit optimizer roots while their guard is
live, are remapped across canonicalization, and extend register lifetimes and
CFG data-flow liveness through native lowering. The compiler resolves each
logical capture to a checked canonical-map index. When optimization proves a
guard cannot fail and removes it, its frame-state roots and metadata are both
omitted from the compiled function.

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

PocketPy straight-line numeric division records all source parameters, the
guarded divisor, and the current left-hand expression value that represents
its primitive operand stack. Counted-loop division additionally records every
current loop local, including the induction variable, in stable declaration
order. Its adapter raises `ZeroDivisionError` only after successful frame
reconstruction identifies a division-by-zero record and a zero Float64
trigger; an unknown runtime exit becomes a diagnostic runtime error instead of
being silently misclassified.

The current recovery vocabulary materializes complete primitive Word and
Float64 logical frame state at the ABI boundary. Compiled guards and
safepoints retain canonical stack maps and copy their bounded values into the
execution context before ABI return. The exact compiled generation can
reconstruct that typed state after its native frame has been restored. A
site-bound two-phase plan can now turn those slots into cyclic frontend-owned
object graphs with transactional rollback as defined in
[MATERIALIZATION.md](MATERIALIZATION.md). Broader optimized tiers still need
frontend-specific recipes, installation into each stock interpreter's concrete
frame representation, unwind registration, and resumable transfer. Those
additions build on this target-independent recovery program without exposing
physical register layouts to frontends; see [STACK_MAPS.md](STACK_MAPS.md).
