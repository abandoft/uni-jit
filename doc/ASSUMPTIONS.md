# Runtime assumptions and safe invalidation

`unijit::runtime::Assumption` is a one-shot concurrency token for optimized
code that depends on mutable frontend or runtime facts. Examples include a
stable object shape, unchanged global binding, or a versioned dispatch target.
An invalidated token is never made valid again; replacement code binds a new
token.

## Compilation contract

An `AssumptionSet` associates each token with a stable exit site and a
frontend-owned resume offset. Compilation rejects null or duplicate tokens,
duplicate sites, sites that collide with runtime guards, and tokens that are
already invalid. Both straight-line SSA and CFG compilation accept assumption
sets.

Every dependency creates an immutable deoptimization record with reason
`kAssumptionInvalidated`. By default the recovery program captures every typed
native-entry argument. A frontend may provide a matching custom record when it
needs a different logical frame. Functions with dependencies report
`requires_context() == true`; bypassing `invoke` through the raw entry pointer
is outside the supported contract.

## Concurrent invalidation

Managed invocation registers its execution context with every dependency before
entering native code and rechecks all tokens after native return. Invalidation
atomically closes the token, wakes active contexts through a runtime-only poll
bit, and waits until every registered invocation has left. Generated safepoints
already poll that bit, so long-running CFG loops exit promptly. Bounded
straight-line code may finish normally, but the managed return boundary changes
its result to the assumption exit before releasing the registration.

The runtime-only wake bit is independent of the public sticky interrupt bit.
An assumption exit therefore does not consume a concurrent user interruption.
After `invalidate()` returns, no managed invocation using that token remains in
native code, and no new invocation can enter it.

The caller must invalidate the token before mutating the state protected by the
assumption. `invalidate()` can block until active code reaches a safepoint or
returns, and must not be called reentrantly from code that depends on the same
token.

## Cache behavior

Existing `CodeHandle` leases remain memory-safe after invalidation but their
managed invocation immediately deoptimizes. Cache lookup retires an invalid
resident generation as a miss, while publication replaces an invalid
same-identity generation instead of reusing it. The
`assumption_invalidations` counter exposes both retirement paths. A fresh
compiled generation must bind fresh assumption tokens; publication keeps an
already invalid incoming generation uncached so it cannot become lookup-visible.
