## 0.2.5

- Added a versioned C17 embedding ABI with opaque ownership, fixed-width records, bounded diagnostics, resource budgets, and exception containment.
- Exposed scalar Word/Float64 construction, guards, safepoints, generation-safe fast calls, patch cells, compilation, invocation, and caching through `unijit_v1_*`.
- Restricted shared libraries to the exact 67-symbol C manifest and added pure-C ABI, export-leakage, and installed-package tests.
- Added dedicated shared-library validation on Ubuntu GCC x86-64, Windows MSVC x86-64, and Apple Clang AArch64 hosts.
- Added portable IR version 1 for deterministic reconstruction of verified straight-line and CFG functions without serializing C++ layouts or process addresses.
- Protected portable IR with fixed little-endian records, SHA-256 payload integrity, allocation-first limits, strict length checks, and post-decode verification.
- Added byte-identical portable IR round trips, corruption and truncation negatives, side-table coverage, native semantic checks, and an installed-package consumer.
- Reworked the project README around open-source onboarding, support status, quick starts, examples, qualification, documentation, contribution, and licensing.

## 0.2.4

- Added generation-safe typed JIT-to-JIT calls for context-free Word/Float64 targets in straight-line and CFG IR.
- Added atomic target-table publication, exact compiled-generation leases, cache-safe invalidation, and concurrent fast-call retargeting.
- Added a strict 128-bit SIMD semantic core with six data vectors, canonical masks, lane operations, comparisons, selection, shuffles, and widening.
- Extended straight-line and CFG SSA, verification, interpretation, optimization, allocation, spill handling, and edge copies for vector values.
- Added complete AArch64 NEON lowering for the delivered SIMD surface, including bounded I64x2 multiplication legalization.
- Added complete x86-64 SSE2 lowering for System V and Windows, including aligned spills and bounded scalar legalization.
- Added bounded RV64IMD scalar SIMD lowering without requiring or claiming RVV.
- Added bounded aligned and unaligned 128-bit vector memory with explicit per-lane byte order and failure-atomic transfers.
- Added target-profile capability preflight and compiled lowering telemetry without code emission.
- Added bounded generated-code atomics with explicit memory orders, target-selected native paths, fallbacks, and litmus qualification.
- Added function-owned non-executable patch cells with release/acquire publication, CAS, fetch-add, and cache-generation lifetime.
- Added retained SIMD, patch-cell, and fast-call performance gates plus sanitizer, stress, package, and real-host qualification.

## 0.2.3

- Added immutable target profiles for architecture, ABI, endianness, scalar/vector features, and vector-width policy.
- Added bounded 8/16/32/64-bit Word memory to both IR forms with explicit regions, alignment, byte order, and diagnosed exits.
- Added Float32/Float64 memory, exact byte reversal, signed-zero preservation, and cross-endian differential tests.
- Added zero-initialized non-addressable Word/Float64 frame locals with typed access and sensitive-slot clearing.
- Added invocation-bound trusted object layouts with semantic identities, fixed fields, permission preflight, and managed entry.
- Documented the independent commercial roadmap after auditing SLJIT only as a capability reference.
- Expanded real-host, sanitizer, package, negative-verifier, and resource-limit qualification for the new provenance model.
- Added the repository-level MIT License.

## 0.2.2

- Added stock Lua 5.5 Float64 numeric-loop compilation with dynamic starts, limits, steps, direction checks, and eight-way unrolling.
- Expanded AArch64 Float64 allocation to all ABI-safe caller-clobbered SIMD/FP registers.
- Expanded System V x86-64 Float64 allocation to fourteen non-scratch XMM registers while preserving the Windows ABI pool.
- Added x86-64 last-use left-operand donation for destructive two-address CFG Float64 arithmetic.
- Fixed canonical cross-block stack-map copies after helper calls on AArch64, x86-64, and RISC-V 64.
- Extended the independent low-level roadmap for SIMD, atomics, patch cells, fast calls, portable IR, and AOT.

## 0.2.1

- Replaced value-ID-sized CFG frames with compact cross-block slots and reusable spill, call, exit, and edge-temporary areas.
- Added signed Word `<`, `<=`, `==`, and `!=` across both IR forms, optimizers, interpreters, and all native backends.
- Added floor division and modulo with deterministic zero-divisor behavior and exact negative-operand semantics on every backend.

## 0.2.0

- Added signed bidirectional Word shifts with overshift-to-zero semantics in both IR forms and every native backend.
- Added exact 64-bit Word AND, OR, XOR, negation, and bitwise-not with matching optimizer and interpreter behavior.
- Added Float64 negation plus equality and inequality with exact NaN, signed-zero, and payload-bit semantics.
- Added optional native safepoint-poll telemetry and exposed call-scoped counts to QuickJS and PocketPy.
- Added baseline and optimized CFG compilation with folding, canonicalization, effect preservation, and dead-code cleanup.
- Added constant-branch folding with recursive unreachable-block removal and exact side-table pruning.
- Expanded real Ubuntu and Windows x86-64 qualification across the core and all three language frontends.
- Added target-independent tests for optimization parity, runtime exits, installed packages, and architecture-specific lowering.

## 0.1.9

- Isolated Lua's `longjmp` error bridge from exception-owning C++ state on Windows x86-64.
- Added one structured integer condition to Lua numeric-loop bodies with exact SSA merges and comparison semantics.
- Added guarded runtime-parameter loop steps with exact zero-step errors and overflow-safe backedge accounting.
- Added guarded runtime-parameter loop starts for ascending and descending loops.
- Added arbitrary nonzero constant integer steps with direction-aware zero-iteration behavior.

## 0.1.8

- Added strided QuickJS and PocketPy counted loops with increment, decrement, and finite literal step updates.
- Added single-level guarded `break` and `continue` with exact loop-state merges.
- Added typed effectful CFG runtime-helper calls with dominance checks, mixed arguments, ABI lowering, and package coverage.
- Split Word and Float64 CFG register classes and kept loop-carried floating-point values resident on all three backends.

## 0.1.7

- Added a bounded fixed-worker compilation scheduler with deduplication, priorities, deadlines, cancellation, telemetry, and deterministic shutdown.
- Added deterministic multi-producer scheduler stress with native compilation, cache publication, installed consumption, and ThreadSanitizer coverage.
- Added asynchronous baseline-to-optimized tiering, cache reuse, cancellation, waiting, and telemetry to Lua, QuickJS, and PocketPy.
- Stabilized the Lua/LuaJIT gate with longer order-balanced sampling while retaining its original performance floors.
- Added CFG Float64 guards, checked PocketPy loop division, ordered comparisons, and exact typed signature retention.
- Added canonical guard and safepoint stack maps with bounded live-value capture and generation-stable reconstruction.
- Added typed deoptimization recovery for arguments, constants, guarded values, optimized value remapping, and cached leases.
- Added transactional object materialization with cyclic graphs, logical-frame installation, commit, and exactly-once rollback.
- Added bounded OSR entry with typed frame plans, generation-stable transfers, concurrent tier switching, and exit reconstruction.
- Added configurable compilation budgets for IR, CFG, metadata, code bytes, and untrusted frontend source.
- Added one-line English/Chinese release-note count parity to release metadata validation.

## 0.1.6

- Added explicit verified baseline and optimized compilation levels for latency-sensitive frontends.
- Added live tier management with hotness profiles, single-compiler claims, immutable generation switching, retry, and withdrawal.
- Added concurrent runtime assumptions, quiescent invalidation, cache retirement, and ThreadSanitizer qualification.
- Added immutable deoptimization records and exact Float64 guard-value recovery on all three native backends.
- Added checked PocketPy division with validated `ZeroDivisionError` reconstruction.
- Added overflow-safe eight-way Lua loop unrolling and promoted the Lua/LuaJIT result to a commercial CI gate.
- Added deterministic differential fuzzing, concurrent code-cache stress, retained records, and dedicated stress workflows.
- Added machine-readable performance gates for Lua, QuickJS, PocketPy, LuaJIT, V8, and CPython comparison targets.
- Added a bounded generation-aware LRU native-code cache and exact-source reuse in all three frontends.
- Added complete QuickJS and PocketPy numeric counted-loop compilation with CFG safepoints and benchmark gates.
- Added typed Float64 CFG values, arithmetic, comparisons, edge validation, interpretation, and three-backend lowering.
- Fixed CFG context metadata and parallel edge copies for shared physical registers.

## 0.1.5

- Added a single-source QuickJS/UniJIT/V8 Jitless/V8 loop benchmark with bit-exact validation and retained metadata.
- Added a single-source PocketPy/UniJIT/CPython/CPython-JIT loop benchmark with verified runtime modes and packages.
- Advanced the rolling LuaJIT reference while keeping generated reference state under `build/`.
- Removed provably passing Float64 guards so known-safe PocketPy division keeps its direct native path.

## 0.1.4

- Added IEEE-754 Float64 division to typed SSA, all native backends, Lua, and QuickJS.
- Added chained Float64 constant folding for arithmetic and division.
- Added effectful Float64 nonzero guards and exact PocketPy division-by-zero exits.
- Split automation into responsibility-specific validation, baseline, sanitizer, metadata, and release workflows.

## 0.1.3

- Added an isolated PocketPy integration with numeric-source translation, GC-owned native callables, and a reproducible benchmark.
- Added an isolated QuickJS integration with Float64 SSA translation, guarded native closures, and a reproducible benchmark.
- Added execution contexts, cooperative interruption, diagnosed exits, and native safepoints on all three architectures.
- Added typed effectful runtime-helper calls with value-bit arguments, liveness preservation, and native ABI lowering.
- Added guarded stock Lua Float64 bytecode compilation and a three-engine call benchmark.

## 0.1.2

- Added typed Float64 SSA, an IEEE-754 interpreter oracle, spill handling, and native encoding on all three targets.
- Added block-local CFG allocation and cycle-safe parallel register copies.
- Renamed project directories to the singular `frontend/`, `tool/`, and `doc/` layout.
- Added one structured stock Lua numeric loop with CFG-carried values and zero-iteration semantics.
- Added range-checked CFG lowering and loop-backedge argument copies on all three architectures.
- Added CFG SSA with explicit block parameters, dominance verification, and a budgeted interpreter.
- Added a three-engine Lua integer benchmark and a pinned-ABI native invocation path.
- Added guarded stock Lua integer bytecode compilation with differential and lifecycle tests.
- Added isolated stock Lua and LuaJIT reference builds.

## 0.1.1

- Added a verified straight-line integer SSA representation and reference interpreter.
- Added independent AArch64, x86-64, and RISC-V 64 native code generators.
- Added spill-capable linear-scan register allocation and W^X code publication.
- Added deterministic differential tests across macOS, Linux, and Windows.
- Added installable CMake package metadata and a cross-platform validation matrix.
- Added canonical SSA optimization passes and a reproducible core benchmark.
- Added decimal version validation and changelog-driven GitHub releases.
