# Hotness profiling and tiered code switching

`unijit::jit::TieredCode` is the runtime-owned publication boundary between an
assumption-free baseline and replaceable optimized code. It does not embed a
language object model or start compiler threads; frontends own scheduling and
feed completed `CodeHandle` leases into the shared switching mechanism.

## Hotness and compilation claims

Each tiered function owns a `HotnessProfile` with independent invocation and
backedge counters. Both counters saturate at the 64-bit maximum. Reaching either
configured threshold makes the function hot, and
`try_begin_optimization()` uses an atomic claim so only one compiler wins a hot
generation.

A successful optimized publication completes the claim. A failed compilation
must call `report_optimization_failure()`, which clears the claim and advances
both eligibility thresholds by the configured retry delay. This prevents every
subsequent invocation from causing a compilation storm. Invocation, backedge,
attempt, success, and failure state is available through machine-readable
statistics.

## Publication and version safety

The baseline must be a valid assumption-free code handle. Optimized code must
have the same parameter count, every parameter type, and return type, and cannot
contain an already invalid assumption.
Every publication or withdrawal creates an immutable state with a new opaque
generation. Readers acquire the current state through C++17 atomic shared
ownership and execute without taking the publication mutex.

Optimized publication preallocates its baseline withdrawal state. An
assumption-deoptimization path can therefore switch future readers back to the
baseline without allocating memory at the failure boundary.

An optimizing compiler should capture the baseline generation before it starts
and pass that value to `publish_optimized`. Publication rejects the result when
another baseline, optimized tier, or withdrawal has changed the generation in
the meantime. Active calls and explicit snapshots retain their original code
lease safely across every switch.

`TieredInvocationResult::attempted_handle` retains that exact execution lease
for the frontend. Diagnostics and state reconstruction must use this handle,
not a fresh snapshot that may already identify another generation.

Replacing the baseline drops the active optimized tier and restarts sampling.
Withdrawing optimized code immediately directs future calls to the baseline,
while already acquired optimized snapshots remain memory-safe.

## Deoptimization policy

An optimized invocation whose metadata identifies
`kAssumptionInvalidated` atomically withdraws that generation. The default
`kReturnExit` policy returns the diagnosed runtime exit to the frontend without
re-executing code.

`kRetryBaseline` is an explicit opt-in for computations the frontend knows are
restartable, such as pure numeric functions with only frame-local state. It
retries the baseline from the original entry arguments after withdrawal and
reports both the deoptimization and retry in the invocation result and runtime
statistics. Frontends must not select this policy for code whose partial
optimized execution can expose external side effects.

## Current boundary

The compiler exposes explicit `kBaseline` and `kOptimized` levels for verified
straight-line SSA. Baseline compilation skips the optimization pipeline while
preserving verification, guards, deoptimization metadata, allocation, native
lowering, and W^X publication; optimized compilation remains the default API.

PocketPy retains exact accepted source, publishes a baseline immediately, and
submits optimization after 64 successful invocations to a one-worker scheduler
with bounded task and byte queues. The background job never accesses PocketPy
VM state, reuses an independent optimized cache, checks cancellation, and
switches with an expected-generation check. PocketPy userdata cancels queued
work during finalization while shared immutable state remains alive until the
job terminates. Its complete counted-loop CFG path remains a single native tier
because repeating the same compilation would add latency without improving
code; its CFG safepoints and checked-division exits remain active in that tier.

QuickJS applies the same baseline and optimized split to accepted straight-line
callables, but submits optimization to a bounded frontend-owned scheduler after
64 calls. The worker never accesses `JSRuntime` or `JSContext`: it only
translates retained immutable source, publishes through the thread-safe cache,
checks cancellation, and installs against the captured tier generation. The
QuickJS class finalizer cancels outstanding work while the job's shared state
keeps all native publication inputs alive through terminal completion.

Lua 5.5 applies invocation profiling to straight-line integer and Float64
specializations and derives loop-latch counts from the accepted numeric-loop
start and guarded limit. A callable claims optimization after 64 calls or
10,000 loop iterations. Straight-line baseline compilation skips the optimizer;
numeric-loop baseline compilation emits a scalar body, while the optimized tier
emits the production eight-way-unrolled body. Background jobs compile only an
immutable snapshot of numeric bytecode and constants and never access Lua GC
objects. Broader language regions still need additional tier-specific lowering.

The core OSR contract can now marshal a bounded typed interpreter frame into a
generation-stable compiled region, atomically select the active tier, preserve
the attempted lease and exact arguments across a diagnosed exit, withdraw an
optimized assumption exit, and report OSR attempts, entries, and exits; see
[ON_STACK_REPLACEMENT.md](ON_STACK_REPLACEMENT.md). Current language adapters
still switch already-compiled callables between invocations. They do not yet
trigger OSR from a stock interpreter backedge.

The shared background execution primitive is now
`CompilationScheduler`. Frontends can deduplicate an expected generation,
bound queued work by task and byte budgets, select urgency, and retain a ticket
for cancellation and completion. The scheduler never owns tier semantics:
every job must recheck its cancellation token and call
`publish_optimized(expected_generation)` so a late result cannot overwrite a
newer baseline or optimized version. The complete contract is specified in
`doc/COMPILATION_SCHEDULER.md`.
