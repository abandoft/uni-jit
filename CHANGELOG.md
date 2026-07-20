## 0.1.6

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
