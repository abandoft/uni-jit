## 0.1.3

- Added an isolated PocketPy 2.1.8 build, strict Python numeric-source
  translation, GC-owned guarded native callables, and a checksum-verified
  stock-versus-UniJIT call benchmark.
- Added an isolated stock QuickJS build, strict numeric-function-to-Float64-SSA
  translation, guarded native closures with GC-owned code lifetime, and a
  checksum-verified stock-versus-UniJIT call benchmark.
- Added an explicit execution-context ABI, lock-free cooperative interruption,
  diagnosed runtime exits, and effectful native safepoints for straight-line
  and CFG loop code on AArch64, x86-64, and RISC-V 64.
- Added effect-aware typed runtime-helper calls with flat value-bits arguments,
  cross-call liveness preservation, and native ABI lowering on all targets.
- Added guarded stock Lua 5.5 Float64 bytecode compilation and a reproducible
  three-engine floating-point call benchmark.

## 0.1.2

- Added typed Float64 SSA constants and arithmetic, an IEEE-754 interpreter
  oracle, optimized spill handling, and native encoding on all three targets.
- Added block-local CFG register allocation and cycle-safe parallel register
  copies on every native backend, removing hot-loop stack traffic.
- Renamed the project-owned frontend, tool, and documentation directories to
  the singular `frontend/`, `tool/`, and `doc/` layout.
- Added guarded compilation of one structured Lua 5.5 numeric `for` loop with
  loop-carried CFG values, zero-iteration semantics, and a three-engine test.
- Added range-checked native CFG lowering on AArch64, x86-64, and RISC-V 64,
  including parallel block-argument copies for loop backedges.
- Added CFG SSA with explicit block parameters, dominance verification, and a
  budgeted reference interpreter for branches and loops.
- Added a three-engine Lua integer benchmark and a pinned-ABI invocation fast
  path that outperforms stock Lua on the measured call-boundary workload.
- Added a guarded stock Lua 5.5 bytecode-to-native frontend for straight-line
  integer functions, with differential and lifecycle tests.
- Added isolated Lua 5.5 and LuaJIT reference builds for language benchmarks.

## 0.1.1

- Added a verified straight-line integer SSA representation and reference
  interpreter.
- Added independent AArch64, x86-64, and RISC-V 64 native code generators.
- Added spill-capable linear-scan register allocation and W^X code publication.
- Added deterministic differential tests across macOS, Linux, and Windows.
- Added installable CMake package metadata and a cross-platform CI matrix.
- Added canonical SSA optimization passes and a reproducible core benchmark.
- Added decimal version validation and changelog-driven GitHub releases.
