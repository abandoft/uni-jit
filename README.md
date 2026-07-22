# UniJIT

UniJIT is a C17/C++17 project for building a unified, high-performance JIT
runtime for Lua, QuickJS, and PocketPy.

The runtime is built around an independent compilation stack: typed SSA IR,
an optimization pipeline, target-specific instruction selection, register
allocation, native machine-code encoders, and hardened executable-memory
management. Third-party JIT projects under `third/` are references and
benchmark competitors; UniJIT does not link against them as its backend.

Native Word and Float64 backends currently target AArch64, x86-64, and
RISC-V 64. The complete current explicit strict 128-bit SIMD surface has
native AArch64 Advanced SIMD/NEON and x86-64 SSE2 lowering, plus bounded
RV64IMD scalar lowering that does not require or claim RVV. Bounded Word
and Float memory supports byte-exact scalar loads and stores, while bounded
128-bit memory transfers cover all six data-vector types. Both use explicit
byte order, alignment and permission checks, diagnosed exits, and native or
bounded scalar lowering on all three backends. A retained complete-CFG-loop
SIMD gate requires speedup over both equivalent scalar generated code and the
reference interpreter on every product architecture. Public cross-target
capability preflight classifies verified operation sets without emitting code,
and compiled functions retain the post-optimization lowering decision for
capacity planning and production telemetry.
Bounded 8/16/32/64-bit generated-code atomics are also delivered in both IR
forms on all three targets, with profile-selected native instructions, bounded
progress fallback, explicit capability telemetry, cross-architecture
memory-model litmus tests, and installed-package execution.
Function-owned non-executable data patch cells provide acquire-loaded mutable
values, targets, shapes, generations, and counters without rewriting immutable
RX code, with release/CAS/fetch-add publication, generation-stable cache leases,
three-backend lowering, concurrency stress, and a retained managed-call
performance ceiling.
Generation-safe JIT internal calls now provide typed Word/Float64 descriptors
in both IR forms, explicitly bound interpreter oracles, immutable native target
table snapshots, exact target-generation leases, fail-closed managed entry,
three-backend indirect lowering, concurrent retargeting stress, installed-package
coverage, and a retained complete-CFG-loop performance ceiling.
The versioned C17 embedding ABI now exposes opaque builders, compilers,
execution contexts, compiled generations, caches, fast-call bindings, patch
cells, fixed-width target/limit/statistics records, structured status codes,
and bounded diagnostics without exporting a C++ implementation symbol. Pure C
installed-package and shared-library consumers are qualified on real Ubuntu
and Windows x86-64 plus Apple AArch64 hosts.
See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the design and delivery
gates, [doc/PORTABILITY.md](doc/PORTABILITY.md) for verified platforms, and
[doc/RUNTIME.md](doc/RUNTIME.md) for execution contexts, exits, and safepoints.
[doc/CODE_CACHE.md](doc/CODE_CACHE.md) defines native-code publication,
capacity, invalidation, telemetry, and concurrent lifetime guarantees.
[doc/DEOPTIMIZATION.md](doc/DEOPTIMIZATION.md) defines diagnosed exits,
immutable recovery metadata, and ABI-boundary frame reconstruction.
[doc/STACK_MAPS.md](doc/STACK_MAPS.md) defines canonical live SSA locations,
native site metadata, and compiled-generation ownership.
[doc/MATERIALIZATION.md](doc/MATERIALIZATION.md) defines bounded atomic object
graph and logical-frame recovery, cyclic references, rollback, and opaque
frontend handles.
[doc/ON_STACK_REPLACEMENT.md](doc/ON_STACK_REPLACEMENT.md) defines bounded
typed interpreter-frame transfer into generation-stable native regions and
exact deoptimization from an OSR entry.
[doc/COMPILATION_LIMITS.md](doc/COMPILATION_LIMITS.md) defines per-compilation
IR, CFG, metadata, and native-code budgets enforced before expensive analysis
and W^X publication.
[doc/ASSUMPTIONS.md](doc/ASSUMPTIONS.md) defines one-shot runtime dependencies,
quiescent invalidation, and stale cache-generation retirement.
[doc/TIERING.md](doc/TIERING.md) defines saturating hotness profiles, single
compiler claims, immutable version switching, and explicit baseline retry.
[doc/COMPILATION_SCHEDULER.md](doc/COMPILATION_SCHEDULER.md) defines bounded
background admission, global version deduplication, cooperative cancellation,
weighted priorities, deterministic shutdown, and scheduler telemetry.
[doc/LOW_LEVEL_CAPABILITIES.md](doc/LOW_LEVEL_CAPABILITIES.md) defines the
independent commercial roadmap for typed memory, strict SIMD, generated-code
atomics, immutable patch cells, fast and tail calls, bounded frame locals,
target profiles, and validated serialization/AOT.
[doc/PORTABLE_SIMD.md](doc/PORTABLE_SIMD.md) defines the delivered strict
128-bit semantic core, lane and mask rules, verifier and optimizer guarantees,
bounded vector memory, three-backend lowering, qualification, and remaining
optional RVV gate.
[doc/TARGET_PROFILES.md](doc/TARGET_PROFILES.md) defines host feature discovery,
portable baselines, immutable compilation identity, cross-target lowering
preflight, compiled telemetry, and profile-scoped caches.
[doc/TYPED_MEMORY.md](doc/TYPED_MEMORY.md) defines bounded regions, Word and
Float32/Float64 storage, standalone byte reversal, byte-exact endian and
unaligned semantics, diagnosed failures, live-value stack maps, native
lowering, resource limits, and the remaining memory roadmap.
[doc/ATOMICS.md](doc/ATOMICS.md) defines the independent generated-code atomic
operation, memory-order, bounded provenance, progress, target-lowering, and
concurrency qualification contract.
[doc/PATCH_CELLS.md](doc/PATCH_CELLS.md) defines immutable-code data patching,
publication order, managed-entry binding, cache-generation lifetime,
three-backend lowering, concurrency qualification, and performance limits.
[doc/FAST_CALLS.md](doc/FAST_CALLS.md) defines typed JIT-internal dispatch,
generation-safe target publication, managed-entry enforcement, target lifetime,
three-backend lowering, qualification, and deferred contextual/tail-call work.
[doc/EMBEDDING_C_API.md](doc/EMBEDDING_C_API.md) defines the versioned C17 ABI,
opaque ownership, error, concurrency, shared-library export, cache-generation,
and deliberate fail-closed capability boundaries.
[doc/FRAME_LOCALS.md](doc/FRAME_LOCALS.md) defines fixed typed invocation
storage, zero initialization, sensitive-slot clearing, optimizer rules,
resource limits, and the boundary for future vector and aggregate frames.
[doc/TRUSTED_OBJECTS.md](doc/TRUSTED_OBJECTS.md) defines invocation-bound
runtime layouts, semantic layout identity, fixed typed fields, managed
preflight, direct three-backend lowering, and the no-raw-pointer boundary.
[doc/QUALIFICATION.md](doc/QUALIFICATION.md) defines deterministic fuzzing,
concurrency stress, sanitizers, and commercial performance floors.
[doc/LUA_FRONTEND.md](doc/LUA_FRONTEND.md) describes the stock Lua 5.5
integration contract, invocation/backedge tiering, and lifecycle telemetry;
[doc/QUICKJS_FRONTEND.md](doc/QUICKJS_FRONTEND.md)
defines the stock QuickJS specialization boundary, asynchronous live tiering,
typed Number/Boolean results, counted-loop baseline-to-optimized promotion,
callable telemetry, bounded waiting, and
cancellation.
[doc/POCKETPY_FRONTEND.md](doc/POCKETPY_FRONTEND.md) documents the PocketPy
2.1.8 embedding API, ownership rules, strict numeric tier, live baseline-to-
optimized background promotion for straight-line and counted-loop code,
script-visible tier telemetry, bounded waits,
typed numeric/Boolean results, checked straight-line and counted-loop division,
and cancellation.

## Build

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

All generated files must remain below the repository-level `build/`
directory.

To build the pinned stock Lua runtime and the UniJIT Lua frontend tests:

```sh
cmake -S . -B build/lua -G Ninja \
  -DUNIJIT_BUILD_LUA_REFERENCE=ON -DUNIJIT_BUILD_TESTS=ON
cmake --build build/lua --target unijit_lua55_tests
ctest --test-dir build/lua -R unijit.lua55_frontend --output-on-failure
```

To build the pinned PocketPy 2.1.8 runtime and its UniJIT frontend:

```sh
cmake -S . -B build/pocketpy -G Ninja \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON -DUNIJIT_BUILD_TESTS=ON
cmake --build build/pocketpy --target unijit_pocketpy_translator_tests
ctest --test-dir build/pocketpy -R unijit.pocketpy_ --output-on-failure
```
