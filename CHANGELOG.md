## 0.1.2

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
