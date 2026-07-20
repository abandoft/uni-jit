# Code-cache lifecycle

`unijit::jit::CodeCache` is the public publication and lifetime boundary for
immutable native code. It is thread-safe, bounded by both entry count and the
actual executable mapping bytes retained by the cache, and independent of any
frontend object model.

## Identity and publication

A cache identity is the pair `(key, fingerprint)`. The caller owns the key
namespace. The fingerprint must change whenever compiler policy, frontend
semantics, assumptions, or serialized input interpretation changes.

`find` returns a `CodeHandle` only when both parts match. `publish` transfers a
successfully compiled function into shared immutable ownership and returns a
handle in every successful case:

- Publishing the same identity reuses the resident generation and discards the
  duplicate compiled function.
- Publishing the same key with a different fingerprint replaces the resident
  entry and assigns a new generation.
- A disabled cache or a function larger than the byte budget returns an owned,
  uncached handle instead of turning compilation success into a failure.
- Allocation failure is reported as `StatusCode::kResourceExhausted`; invalid
  input is reported as `StatusCode::kInvalidArgument`.

Generations are opaque change tokens. They are monotonic within a cache until
the 64-bit counter wraps and must not be treated as globally unique IDs.

## Capacity and eviction

`CodeCacheLimits` controls the maximum resident entry count and executable
mapping bytes. The byte count is the operating-system mapping size retained by
each compiled function, including page rounding and the protected entry
prefix, rather than the emitted instruction length.

Publication evicts least-recently-used resident entries until both limits are
satisfied. A successful lookup refreshes recency. A zero entry limit disables
residency. A zero byte limit makes every nonempty mapping uncached.

`CodeCacheStats` exposes lookup, publication, replacement, invalidation,
eviction, clear, and current-residency counters. The snapshot is internally
synchronized and is suitable for telemetry and capacity tuning; it is not a
transactional view across multiple calls.

## Leases, invalidation, and concurrency

A `CodeHandle` is a copyable execution lease. Invalidation, replacement,
eviction, `clear`, moving the cache, or destroying the cache removes lookup
visibility but does not revoke an existing handle. The executable mapping is
reclaimed only after the last handle and resident cache reference are gone.
Compiled deoptimization records share the same immutable lifetime and remain
queryable through the handle. This guarantees that one thread may execute and
reconstruct an exit from a leased function while another thread replaces or
invalidates its cache entry.

`invalidate(key, fingerprint)` performs assumption-specific invalidation;
`invalidate(key)` removes the currently resident generation for the whole key.
Neither operation interrupts active execution. Frontends that need immediate
semantic withdrawal must pair invalidation with execution-context safepoints
and diagnosed exits.

Compiled runtime assumptions provide that pairing as a first-class contract.
Lookup retires a resident generation whose token is invalid, and publication
replaces rather than reuses an invalid same-identity generation. Both paths
increment `CodeCacheStats::assumption_invalidations`. Existing leases remain
memory-safe but their managed invocation exits through deoptimization. See
[ASSUMPTIONS.md](ASSUMPTIONS.md) for the required invalidate-before-mutate
ordering. An incoming function whose dependency is already invalid receives an
owned uncached handle and is never made lookup-visible.

The cache owns mappings and identity; `TieredCode` separately owns which leased
generation receives new invocations. This separation allows an optimized tier
to be withdrawn atomically while snapshots already executing it retain safe
mapping ownership. See [TIERING.md](TIERING.md).

The current implementation uses shared immutable ownership. Epoch-based
reclamation may later reduce reference-count traffic without changing the
public lease and invalidation contract.

## Frontend use

The stock QuickJS and PocketPy adapters use exact source text as the key and
keep separate frontend-tier fingerprints. Their runtime-owned callable
objects retain `CodeHandle` leases.

The Lua 5.5 adapter cannot use source identity, so it serializes the numeric
mode, prototype shape, complete instruction stream, and exact numeric constant
bits. Integer and Float64 tiers use separate cache domains. Prototypes with
nested functions, unsupported constants, or malformed storage remain safely
uncached.

All three adapters use backedge safepoints for compiled counted loops and safe
managed invocation whenever the compiled function requires an execution
context.
