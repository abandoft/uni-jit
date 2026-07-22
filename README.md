# UniJIT

[![Continuous validation](https://github.com/abandoft/uni-jit/actions/workflows/continuous-validation.yml/badge.svg)](https://github.com/abandoft/uni-jit/actions/workflows/continuous-validation.yml)
[![Latest release](https://img.shields.io/github/v/release/abandoft/uni-jit)](https://github.com/abandoft/uni-jit/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C17 / C++17](https://img.shields.io/badge/C17%20%2F%20C%2B%2B17-supported-00599C.svg)](#building)

UniJIT is an independent, typed, multi-architecture JIT runtime for embedders and language implementers. It provides a C++17 compilation core, a versioned C17 embedding ABI, native AArch64, x86-64, and RISC-V 64 backends, and JIT integrations for stock Lua, QuickJS, and PocketPy.

UniJIT owns its complete production path—from SSA verification and optimization through register allocation, native instruction encoding, W^X publication, and generation-safe reclamation.

> **Project status:** UniJIT is under active `0.x` development. The repository already has executable multi-platform qualification and a versioned `unijit_v1_*` C ABI, but the public feature surface and artifact formats are still expanding. Pin an exact release and run the qualification suite before production adoption.

## Why UniJIT?

- **One typed core for several runtimes.** Straight-line and control-flow SSA support Word, Float64, bounded memory, atomics, strict 128-bit SIMD, guards, safepoints, frame locals, trusted layouts, patch cells, and generation-safe JIT calls.
- **Independent native backends.** AArch64, x86-64 System V/Windows, and RISC-V 64 instruction encoders are implemented in this repository without delegating code generation to another JIT library.
- **Fail-closed runtime boundaries.** Resource budgets, target profiles, diagnosed exits, immutable executable pages, bounded metadata, opaque ownership, and explicit generation leases are part of the API contract.
- **Embeddable from C or C++.** The shared-library C17 ABI exports only versioned symbols and hides implementation layout. The C++ API exposes the complete typed construction and runtime surface.
- **Deterministic artifacts and tests.** Portable IR version 1 uses a canonical length-delimited wire format with SHA-256 integrity, bounded decoding, post-decode verification, and byte-identical reconstruction.
- **Performance claims are executable gates.** Interpreter/native differential tests, sanitizers, concurrency stress, installed consumers, real-host architecture tests, and machine-readable benchmark thresholds run in responsibility-specific workflows.

## Supported targets

| Architecture | ABI / operating systems | Scalar | Strict SIMD | Generated-code atomics |
| --- | --- | :---: | :---: | :---: |
| AArch64 | AAPCS64; macOS and Linux | Native | NEON / Advanced SIMD | Native or bounded fallback by target profile |
| x86-64 | System V; Linux and macOS | Native | SSE2 | Native |
| x86-64 | Windows x64 | Native | SSE2 | Native |
| RISC-V 64 | ELF LP64D; Linux | Native | Bounded RV64IMD scalar lowering | Native `A` extension or bounded fallback |

The committed validation graph exercises real Ubuntu and Windows x86-64 hosts, Apple AArch64, sanitizers, deterministic fuzzing, and external installed-package consumers. RISC-V qualification is also replayed on real 64-bit hardware. Optional RVV lowering remains on the roadmap; the current RISC-V SIMD path does not claim RVV.

## Building

Requirements:

- CMake 3.20 or newer
- a C17 and C++17 compiler
- Ninja (recommended, but not required)
- Git submodules only when building a language frontend or reference benchmark

Clone the repository with its pinned reference runtimes:

```sh
git clone --recurse-submodules https://github.com/abandoft/uni-jit.git
cd uni-jit
```

Build and run the core suite:

```sh
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_WARNINGS_AS_ERRORS=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
```

All generated files must stay below the repository-level `build/` directory.

Install UniJIT for another CMake project:

```sh
cmake --install build/debug --prefix build/install
```

Then consume it with:

```cmake
find_package(UniJIT CONFIG REQUIRED)
target_link_libraries(your_runtime PRIVATE UniJIT::unijit)
```

## A minimal C++ example

This example builds `(a + b) * 2`, serializes it as portable IR, reconstructs it through the untrusted-input boundary, compiles it, and invokes the native function:

```cpp
#include <array>
#include <utility>

#include <unijit/ir/function.h>
#include <unijit/ir/package.h>
#include <unijit/jit/compiler.h>

int main() {
  unijit::ir::FunctionBuilder builder(2);
  const auto sum = builder.add(builder.parameter(0), builder.parameter(1));
  const auto result = builder.multiply(sum, builder.constant(2));
  if (!builder.set_return(result).ok()) return 1;

  const auto package =
      unijit::ir::encode_portable_ir(std::move(builder).build());
  if (!package.ok()) return 2;

  auto decoded = unijit::ir::decode_portable_function(package.bytes);
  if (!decoded.ok()) return 3;

  auto compiled = unijit::jit::Compiler::compile(decoded.function);
  if (!compiled.ok()) return 4;

  const std::array<unijit::ir::Word, 2> arguments = {8, 13};
  const auto invocation = compiled.function->invoke(arguments.data(), 2);
  return invocation.ok() && invocation.value == 42 ? 0 : 5;
}
```

For stable shared-library integration from C, start with the ownership model and complete example in [the C17 embedding guide](doc/EMBEDDING_C_API.md). The ABI uses opaque handles, fixed-width extensible structures, bounded diagnostics, and explicitly versioned `unijit_v1_*` entry points.

## Language frontends

The language integrations specialize verified numeric paths and fall back to their stock runtimes outside the admitted subset. They do not replace the upstream parser, object model, garbage collector, or exception semantics.

| Frontend | Current integration | Configure option |
| --- | --- | --- |
| Stock Lua | Word/Float64 calls and counted loops, guards, backedge tiering, semantic and LuaJIT comparison gates | `UNIJIT_BUILD_LUA_REFERENCE=ON` |
| QuickJS | Number/Boolean specialization, counted loops, asynchronous tiering, V8 Jitless/V8 comparison harness | `UNIJIT_BUILD_QUICKJS_REFERENCE=ON` |
| PocketPy | Numeric/Boolean specialization, checked arithmetic, counted loops, background tiering, Python 3.14 JIT comparison harness | `UNIJIT_BUILD_POCKETPY_REFERENCE=ON` |

Build all three frontend test suites:

```sh
cmake -S . -B build/frontends -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_BUILD_LUA_REFERENCE=ON \
  -DUNIJIT_BUILD_QUICKJS_REFERENCE=ON \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON
cmake --build build/frontends --parallel
ctest --test-dir build/frontends --output-on-failure \
  -R 'unijit\.(lua55|quickjs|pocketpy)'
```

The upstream sources in `third/` are pinned Git submodules so results can be reproduced. Their presence does not change UniJIT's MIT license; each third-party project remains governed by its own license.

## Benchmarks and qualification

Build the core benchmark executables with:

```sh
cmake -S . -B build/benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/benchmark --parallel
```

The benchmark suite covers scalar arithmetic, CFG Float64 residency, strict SIMD loops, bounded memory, immutable data patch cells, and generation-safe internal calls. Language workflows additionally compare the pinned stock runtimes against LuaJIT, V8 Jitless/V8, and CPython's JIT where applicable.

Extended deterministic fuzzing and concurrency stress are opt-in locally:

```sh
cmake -S . -B build/qualification -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_BUILD_QUALIFICATION_TESTS=ON
cmake --build build/qualification --parallel
ctest --test-dir build/qualification --output-on-failure -L qualification
```

See [release qualification](doc/QUALIFICATION.md) for the exact workloads, seeds, thresholds, retained JSON records, and real-host expectations. A performance result is treated as evidence only when the matching semantic oracle and target identity also pass.

## Architecture

```text
Lua / QuickJS / PocketPy
          │
          ▼
  frontend specialization
          │
          ▼
 typed SSA + CFG IR ──► verifier ──► optimizer
          │                            │
          └──── reference oracle       ▼
                              target lowering + register allocation
                                           │
                                           ▼
                              native encoder + W^X code cache
                              AArch64 │ x86-64 │ RISC-V 64
```

Portable IR remains architecture-neutral and is always verified and locally compiled. Target-specific AOT objects are intentionally not accepted yet; their compiler identity, relocation, target-profile, mapping, and authenticity contract must be complete before serialized bytes can become executable.

## Repository layout

| Path | Purpose |
| --- | --- |
| `include/unijit/` | Installed C17 and C++17 public APIs |
| `src/ir/` | IR construction support, verification, interpretation, optimization, and portable packages |
| `src/jit/` | Compilation orchestration, allocation, code ownership, and executable memory |
| `src/jit/backend/` | Independent AArch64, x86-64, and RISC-V 64 lowering and encoders |
| `src/runtime/` | Execution contexts, assumptions, recovery, OSR, and materialization |
| `frontend/` | Stock Lua, QuickJS, and PocketPy integrations |
| `benchmark/` | Reproducible workloads and machine-readable performance gates |
| `test/` | Unit, ABI, package-consumer, differential, and stress qualification |
| `third/` | Pinned upstream references and benchmark competitors |
| `doc/` | Versioned design and delivery contracts |
| `tool/` | Release, export-surface, and performance-gate tooling |

## Documentation

| Topic | Documents |
| --- | --- |
| Design and platform model | [Architecture](doc/ARCHITECTURE.md), [portability](doc/PORTABILITY.md), [target profiles](doc/TARGET_PROFILES.md), [low-level roadmap](doc/LOW_LEVEL_CAPABILITIES.md) |
| Embedding and artifacts | [C17 embedding ABI](doc/EMBEDDING_C_API.md), [portable IR](doc/PORTABLE_IR.md), [code cache](doc/CODE_CACHE.md) |
| Runtime safety and recovery | [Runtime](doc/RUNTIME.md), [stack maps](doc/STACK_MAPS.md), [deoptimization](doc/DEOPTIMIZATION.md), [materialization](doc/MATERIALIZATION.md), [OSR](doc/ON_STACK_REPLACEMENT.md), [assumptions](doc/ASSUMPTIONS.md) |
| Low-level capabilities | [Typed memory](doc/TYPED_MEMORY.md), [portable SIMD](doc/PORTABLE_SIMD.md), [atomics](doc/ATOMICS.md), [patch cells](doc/PATCH_CELLS.md), [fast calls](doc/FAST_CALLS.md), [frame locals](doc/FRAME_LOCALS.md), [trusted objects](doc/TRUSTED_OBJECTS.md) |
| Runtime policy | [Compilation limits](doc/COMPILATION_LIMITS.md), [scheduler](doc/COMPILATION_SCHEDULER.md), [tiering](doc/TIERING.md) |
| Frontends | [Lua](doc/LUA_FRONTEND.md), [QuickJS](doc/QUICKJS_FRONTEND.md), [PocketPy](doc/POCKETPY_FRONTEND.md) |
| Project process | [Qualification](doc/QUALIFICATION.md), [release policy](doc/RELEASING.md), [changelog](CHANGELOG.md) |

## Contributing

Issues and focused pull requests are welcome. Before opening a change:

1. Keep generated files and downloaded build dependencies under `build/`.
2. Add an interpreter or otherwise independent semantic oracle for new IR behavior.
3. Add verifier negatives and explicit resource limits for new untrusted inputs.
4. Keep architecture-specific implementation inside its backend boundary.
5. Run the core test command above; use the qualification build for compiler, cache, concurrency, or serialization changes.
6. Update the relevant design contract and prepend a single-line changelog bullet when the change belongs to the active release.

## License

UniJIT is available under the [MIT License](LICENSE). Third-party submodules retain their respective upstream licenses.
