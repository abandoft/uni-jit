## 0.1.7

- Added a fixed-worker background compilation scheduler with task-count and estimated-byte queue budgets, nonblocking admission, deadline-bounded backpressure, exact identity-and-generation deduplication, and starvation-bounded urgent, normal, and background priorities.
- Added copyable compilation tickets with queued/running/terminal states, immediate queued cancellation and capacity reclamation, lock-free cooperative running cancellation, exception containment, bounded waits, current/peak resource telemetry, and deterministic drain or cancel shutdown.
- Added production compilation-scheduler stress that coordinates many producers through a deterministic barrier, compiles and publishes native code concurrently, validates cache residency and execution, reconciles lifecycle counters, retains machine-readable records, runs under ThreadSanitizer, and is consumed through the installed CMake package.
- Integrated live QuickJS baseline-to-optimized tiering so accepted straight-line callables publish low-latency baseline code immediately, claim optimization after 64 calls, compile through a bounded runtime-independent worker, reuse an exact-source optimized cache, reject stale generations, and cancel queued work during garbage collection.
- Added QuickJS callable telemetry plus bounded waiting and explicit cancellation through `unijit.stats`, `unijit.wait`, and `unijit.cancel`, with deterministic tests for distinct compilation levels, asynchronous promotion, scheduler completion, non-tierable CFG reporting, and retained executable lifetime.
- Integrated live Lua 5.5 integer and Float64 tiering with immutable numeric prototype snapshots, independent mode-and-tier caches, scalar-loop baselines, eight-way-unrolled optimized loops, atomic expected-generation publication, and runtime-independent background compilation after 64 calls or 10,000 measured loop iterations.
- Added Lua callable and loop telemetry plus bounded waiting, explicit cancellation, and GC cancellation through `unijit.stats`, `unijit.wait`, and `unijit.cancel`, with deterministic tests proving invocation-triggered straight-line promotion, exact parameter-derived backedge accounting, structurally distinct optimized loop IR, and idempotent finalization.
- Hardened the hosted Lua-over-LuaJIT commercial gate with 300,000 measured complete-loop calls and seven samples, replacing millisecond-scale sampling that could invert the ratio under shared-runner scheduling noise.
- Made tier-claim admission atomic with code publication and ordered promotion telemetry before executable visibility, preventing a fast background compiler from triggering a redundant optimization attempt or exposing partially updated statistics.
- Moved PocketPy optimized compilation off the VM thread onto a one-worker scheduler bounded to 64 queued tasks and 8 MiB, with immutable retained-source jobs, exact-cache reuse, cooperative cancellation, expected-generation publication, and GC-safe shared state lifetime.
- Added PocketPy compilation-task and scheduler telemetry plus timeout-bounded waiting and explicit cancellation through `unijit.stats`, `unijit.wait`, and `unijit.cancel`, with deterministic cold, asynchronous promotion, optimized checked-division, timeout validation, and foreign-object rejection coverage.
- Added effectful CFG Float64 nonzero guards with dominance and type verification, exact signed-zero exits in the reference interpreter, immutable deoptimization metadata, managed-context provisioning, and independent native lowering on AArch64, x86-64, and RISC-V 64.
- Enabled checked PocketPy counted-loop `/` and `/=` with exact source-site reconstruction and `ZeroDivisionError` mapping for both signed zeroes while preserving zero-iteration semantics where dormant division is not executed.
- Added ordered Float64 `<` and `<=` to straight-line SSA with Word results, verifier and optimizer support, constant folding, IEEE-754 unordered-false interpreter semantics, register allocation, and independent AArch64, x86-64, and RISC-V 64 lowering.
- Retained complete parameter and return types in compiled functions and cache leases, rejected inconsistent CFG return types, and strengthened tier publication to compare every signature type rather than parameter count alone.
- Enabled one top-level QuickJS `<`, `<=`, `>`, or `>=` Number comparison with strict chained-comparison rejection and true JavaScript Boolean results that remain correctly typed after asynchronous optimized-tier promotion.
- Enabled one top-level PocketPy `<`, `<=`, `>`, or `>=` numeric comparison with strict chained-comparison rejection and true PocketPy `bool` results that preserve NaN and asynchronous optimized-tier semantics.
- Added canonical stack maps for every compiled guard and safepoint, with precise straight-line liveness, fixed-point CFG loop liveness, successor-parameter edge translation, stable typed frame slots, bounded metadata growth, and independent AArch64, x86-64, and RISC-V 64 publication.
- Exposed immutable native offsets, frame sizes, exit kinds, live SSA locations, and map telemetry through compiled functions and cache leases, enforced unique runtime exit sites, pruned maps with eliminated guards, and verified metadata lifetime through the installed package API.
- Added allocation-free diagnosed-exit capture for up to 64 live values per site, with exact Word and Float64 bits copied before native frame restoration, validated reconstruction through compiled functions and generation-stable cache leases, stale-state clearing, and compile-time rejection beyond the fixed capacity.
- Added arbitrary Word and Float64 SSA inputs to deoptimization recovery, with guard-scoped optimizer preservation and Value-ID remapping, straight-line availability and CFG dominance validation, forced allocator and data-flow liveness on all three backends, resolved canonical capture indices, and exact baseline, optimized, cached-lease, and installed-package reconstruction.
- Materialized PocketPy primitive division frames with the current straight-line left operand plus every counted-loop local and induction value while retaining stable parameter and signed-zero divisor slots for existing `ZeroDivisionError` mapping.
- Added site-and-resume-bound transactional object materialization with primitive, constant, forward, backward, and cyclic recipe inputs, bounded graph validation, two-phase shell allocation and field population, opaque frontend handles, exactly-once rollback for every callback failure phase, explicit object-valued logical slots, and compiled-function plus generation-stable cache-lease APIs.
- Made object materialization and logical interpreter-frame installation one atomic frontend transaction, passing exact exit metadata and slot counts at begin, staging typed primitive and object slots before commit, and rolling back both the graph and frame exactly once on installation failure.
- Stabilized the hosted Lua commercial gate with three order-balanced seven-sample trials, median-of-trial comparison without lowering the 1.25x stock Lua or 1.10x LuaJIT floors, retained per-trial timing and execution-order evidence, and always-uploaded structured failure decisions for audit.
- Added bounded site-and-resume-checked on-stack replacement entry with unique typed interpreter slots, exact native-signature plans, allocation-free successful marshalling into fixed argument storage, generation-stable compiled-function and cache-lease transfer, retained exit arguments for exact deoptimization, and installed-package coverage.
- Integrated OSR transfer with atomic tier snapshots, retained the exact attempted baseline or optimized lease under concurrent publication, withdrew optimized assumption exits without invalidating reconstruction, added attempt/entry/exit telemetry to all frontend stats APIs, and stress-tested OSR while generations switch.

## 0.1.6

- Added explicit verified baseline and optimized straight-line compilation levels so latency-sensitive frontends can skip optimizer work initially without bypassing guards, deoptimization metadata, native allocation, or W^X publication.
- Integrated live PocketPy tiering with retained exact source, independent baseline and optimized caches, atomic single-compiler promotion after 64 successful calls, expected-generation publication, delayed failure retry, and direct single-tier CFG loops.
- Added PocketPy tier telemetry for active generation, hotness, compilation outcomes, switches, code size, and IR size, retained the exact attempted code lease for race-safe exit reconstruction, and verified `ZeroDivisionError` recovery after optimized promotion.
- Added public saturating invocation and backedge hotness profiles with configurable thresholds, atomic single-compiler claims, cumulative telemetry, and retry delay after failed compilation to prevent claim storms.
- Added assumption-free baseline and replaceable optimized tier publication through immutable atomically acquired snapshots, opaque generations, signature validation, late-result rejection, safe concurrent promotion, preallocated allocation-free withdrawal, and retained execution leases.
- Added automatic optimized-tier withdrawal on assumption deoptimization plus an explicit restartable baseline-retry policy, with concurrent switching, stale snapshot, fallback, telemetry, ThreadSanitizer, and installed-package coverage.
- Added one-shot concurrent runtime assumptions for straight-line and CFG compilation, with validated dependency sites, typed entry-frame recovery, managed entry and return checks, safepoint wakeups isolated from user interrupts, and blocking quiescence before protected state mutation.
- Integrated assumption validity with the native-code cache so lookup retires stale generations, same-identity publication replaces rather than reuses them, retained leases deoptimize safely, and dedicated telemetry reports automatic assumption invalidations.
- Extended the ThreadSanitizer runtime gate with active native-loop assumption invalidation, quiescence, frame reconstruction, stale-generation replacement, and independent sticky-interrupt coverage.
- Added public immutable deoptimization records with semantic reasons, frontend resume offsets, typed recovery of entry arguments, constants, and guarded exit values, compiler validation and optimization pruning, cached-lease lifetime guarantees, and ABI-boundary frame reconstruction.
- Preserved the exact triggering Float64 value bits for diagnosed guard exits in the reference interpreter and all AArch64, x86-64, and RISC-V 64 native backends, including the sign of zero.
- Integrated PocketPy checked division with validated deoptimization reconstruction so only a confirmed division-by-zero record becomes `ZeroDivisionError`, while unknown runtime exits fail diagnostically instead of being misclassified.
- Added overflow-safe eight-way Lua numeric-loop unrolling with a bounded scalar tail and cooperative polling at most every eight source iterations, exceeding LuaJIT on the validated complete-loop x86-64 workload while preserving `math.maxinteger` boundaries.
- Promoted the complete Lua numeric-loop comparison to a hard commercial CI gate requiring at least 1.25x stock Lua performance and 1.10x LuaJIT performance.
- Added deterministic seed-replay differential fuzzing that generates straight-line Word and Float64 SSA plus typed loop-and-diamond CFG programs, then compares production native execution bit-for-bit with the reference interpreters.
- Added configurable concurrent code-cache stress over lookup, publication, replacement, precise and key-wide invalidation, LRU eviction, retained execution leases, clearing, and synchronized lifecycle telemetry.
- Added a responsibility-specific stress and differential-fuzz workflow with extended committed seed corpora, retained machine-readable records, and ThreadSanitizer coverage for concurrent cache lifecycle operations.
- Added machine-readable commercial performance gates that require complete-loop UniJIT execution to exceed stock Lua, LuaJIT, stock QuickJS, V8 Jitless, stock PocketPy, CPython 3.14.6 interpreter mode, and CPython 3.14.6 JIT mode by explicit CI floors.
- Added a direct UniJIT-over-LuaJIT ratio and measurement-boundary metadata to Lua comparison records, and retained all four hosted Lua baseline artifacts.
- Fixed QuickJS adapter diagnostics to use format widths accepted consistently by Unix, MSVC, and MinGW x64 toolchains under warnings-as-errors builds.
- Added a public thread-safe bounded LRU native-code cache with fingerprinted identities, generation tokens, executable-mapping byte budgets, lifecycle telemetry, precise invalidation, and copyable execution leases that remain safe across replacement, eviction, clearing, and cache destruction.
- Integrated exact-source native-code reuse into the stock QuickJS and PocketPy adapters while retaining compiled mappings through runtime-owned callable leases.
- Added safe Lua 5.5 numeric-prototype caching over the numeric mode, prototype shape, complete instruction stream, and exact numeric constant bits, with isolated integer and Float64 domains and an uncached fallback for unsupported prototype shapes.
- Added execution-context safepoints to Lua 5.5 numeric-loop backedges so cached long-running functions remain cooperatively interruptible.
- Added complete QuickJS numeric counted-loop compilation with Float64 local state, arithmetic assignments, ordered conditional resets, SSA merges, backedge safepoints, GC-owned native invocation, and a bit-exact target benchmark that exceeds V8 Jitless on the validated workload.
- Added complete PocketPy `range(count)` numeric-loop compilation with indentation validation, Float64 local state, conditional updates, SSA merges, backedge safepoints, GC-owned native invocation, and a bit-exact target benchmark that exceeds CPython 3.14.6 interpreter and JIT modes on the validated workload.
- Fixed CFG compilation metadata so functions containing safepoints always receive an execution context through the safe invocation API.
- Fixed CFG parallel edge copies when multiple target parameters share a physical register, preventing nonlocal loop state from being overwritten before its required stack publication.
- Added ordered Float64 CFG comparisons with IEEE-754 unordered-false semantics and native encodings on all three architectures.
- Added typed Float64 CFG parameters, constants, arithmetic, loop-carried edge validation, interpreter execution, and native lowering on AArch64, x86-64, and RISC-V 64.

## 0.1.5

- Added a single-source language-loop benchmark across QuickJS, UniJIT, V8 Jitless, and V8 with pinned Node execution, bit-exact validation, engine metadata, and retained CI records.
- Added a single-source language-loop benchmark across PocketPy, UniJIT, CPython 3.14.6, and CPython 3.14.6 JIT with strict runtime-mode checks, verified official packages, and retained CI records.
- Advanced the no-release LuaJIT reference to the latest upstream v2.1 commit while keeping all generated reference-build state under `build/`.
- Eliminated provably passing Float64 nonzero guards so PocketPy division by a known nonzero constant retains the unchecked native-call fast path.

## 0.1.4

- Added IEEE-754 Float64 division to typed SSA and every native backend, with guarded Lua 5.5 and QuickJS frontend support.
- Added chained Float64 constant folding for addition, subtraction, multiplication, and division.
- Added effectful Float64 nonzero guards with diagnosed exits on every backend and used them to preserve PocketPy `ZeroDivisionError` semantics for native division.
- Split hosted automation into responsibility-specific continuous-validation, core-platform, language-baseline, sanitizer, release-metadata, and publication workflows with no generic `ci.yml` entry point.

## 0.1.3

- Added an isolated PocketPy 2.1.8 build, strict Python numeric-source translation, GC-owned guarded native callables, and a checksum-verified stock-versus-UniJIT call benchmark.
- Added an isolated stock QuickJS build, strict numeric-function-to-Float64-SSA translation, guarded native closures with GC-owned code lifetime, and a checksum-verified stock-versus-UniJIT call benchmark.
- Added an explicit execution-context ABI, lock-free cooperative interruption, diagnosed runtime exits, and effectful native safepoints for straight-line and CFG loop code on AArch64, x86-64, and RISC-V 64.
- Added effect-aware typed runtime-helper calls with flat value-bits arguments, cross-call liveness preservation, and native ABI lowering on all targets.
- Added guarded stock Lua 5.5 Float64 bytecode compilation and a reproducible three-engine floating-point call benchmark.

## 0.1.2

- Added typed Float64 SSA constants and arithmetic, an IEEE-754 interpreter oracle, optimized spill handling, and native encoding on all three targets.
- Added block-local CFG register allocation and cycle-safe parallel register copies on every native backend, removing hot-loop stack traffic.
- Renamed the project-owned frontend, tool, and documentation directories to the singular `frontend/`, `tool/`, and `doc/` layout.
- Added guarded compilation of one structured Lua 5.5 numeric `for` loop with loop-carried CFG values, zero-iteration semantics, and a three-engine test.
- Added range-checked native CFG lowering on AArch64, x86-64, and RISC-V 64, including parallel block-argument copies for loop backedges.
- Added CFG SSA with explicit block parameters, dominance verification, and a budgeted reference interpreter for branches and loops.
- Added a three-engine Lua integer benchmark and a pinned-ABI invocation fast path that outperforms stock Lua on the measured call-boundary workload.
- Added a guarded stock Lua 5.5 bytecode-to-native frontend for straight-line integer functions, with differential and lifecycle tests.
- Added isolated Lua 5.5 and LuaJIT reference builds for language benchmarks.

## 0.1.1

- Added a verified straight-line integer SSA representation and reference interpreter.
- Added independent AArch64, x86-64, and RISC-V 64 native code generators.
- Added spill-capable linear-scan register allocation and W^X code publication.
- Added deterministic differential tests across macOS, Linux, and Windows.
- Added installable CMake package metadata and a cross-platform CI matrix.
- Added canonical SSA optimization passes and a reproducible core benchmark.
- Added decimal version validation and changelog-driven GitHub releases.
