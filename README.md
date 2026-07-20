# UniJIT

UniJIT is a C17/C++17 project for building a unified, high-performance JIT
runtime for Lua, QuickJS, and PocketPy.

The runtime is built around an independent compilation stack: typed SSA IR,
an optimization pipeline, target-specific instruction selection, register
allocation, native machine-code encoders, and hardened executable-memory
management. Third-party JIT projects under `third/` are references and
benchmark competitors; UniJIT does not link against them as its backend.

Native Word and Float64 backends currently target AArch64, x86-64, and
RISC-V 64.
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
[doc/QUALIFICATION.md](doc/QUALIFICATION.md) defines deterministic fuzzing,
concurrency stress, sanitizers, and commercial performance floors.
[doc/LUA_FRONTEND.md](doc/LUA_FRONTEND.md) describes the stock Lua 5.5
integration contract, invocation/backedge tiering, and lifecycle telemetry;
[doc/QUICKJS_FRONTEND.md](doc/QUICKJS_FRONTEND.md)
defines the stock QuickJS specialization boundary, asynchronous live tiering,
typed Number/Boolean results, callable telemetry, bounded waiting, and
cancellation.
[doc/POCKETPY_FRONTEND.md](doc/POCKETPY_FRONTEND.md) documents the PocketPy
2.1.8 embedding API, ownership rules, strict numeric tier, live baseline-to-
optimized background promotion, script-visible tier telemetry, bounded waits,
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
