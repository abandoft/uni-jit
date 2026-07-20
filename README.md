# UniJIT

UniJIT is a C17/C++17 project for building a unified, high-performance JIT
runtime for Lua, QuickJS, and PocketPy.

The runtime is built around an independent compilation stack: typed SSA IR,
an optimization pipeline, target-specific instruction selection, register
allocation, native machine-code encoders, and hardened executable-memory
management. Third-party JIT projects under `third/` are references and
benchmark competitors; UniJIT does not link against them as its backend.

Native integer backends currently target AArch64, x86-64, and RISC-V 64.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and delivery
gates and [docs/PORTABILITY.md](docs/PORTABILITY.md) for verified platforms.

## Build

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

All generated files must remain below the repository-level `build/`
directory.
