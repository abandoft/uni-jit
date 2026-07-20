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
[doc/LUA_FRONTEND.md](doc/LUA_FRONTEND.md) describes the first stock Lua 5.5
integration contract, and [doc/QUICKJS_FRONTEND.md](doc/QUICKJS_FRONTEND.md)
defines the current stock QuickJS specialization boundary.
[doc/POCKETPY_FRONTEND.md](doc/POCKETPY_FRONTEND.md) documents the PocketPy
2.1.8 embedding API, ownership rules, and strict numeric tier.

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
