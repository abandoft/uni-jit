# Portability and verification

UniJIT's bootstrap native ABI is:

```cpp
std::int64_t generated(const std::int64_t* arguments,
                       unijit::runtime::ExecutionContext* context);
```

Generated code only uses caller-saved registers and restores its stack before
returning. The optional context carries cooperative interruption and diagnosed
runtime exits; null bypasses polling. Integer operations have modulo-2^64
semantics in both the reference interpreter and native backends.

## Backend baselines

| Backend | ISA baseline | Host ABI support | Constant strategy |
|---|---|---|---|
| AArch64 | little-endian A64 with FP64 | AAPCS64 | `MOVZ`/`MOVK` sequence |
| x86-64 | x86-64 with SSE2 | System V and Windows x64 | 64-bit immediate move |
| RISC-V 64 | little-endian RV64IMD | ELF psABI | PC-relative literal pool |

The RISC-V backend requires the `M` extension for native integer multiply. The
Float64 tier requires the `D` extension. The assembler, ABI selection, register
allocator, and executable-memory publisher are owned by UniJIT and do not link
against SLJIT or another JIT library.

Straight-line and CFG lowering allocate Word and Float64 values from separate
caller-saved register banks. Typed CFG edge moves preserve floating-point
residency, use a dedicated scratch register for cycles, and spill exact bits to
the canonical 64-bit frame only when the target bank is oversubscribed or
runtime-exit capture requires a stable location.

## Verified systems

The following full configure/build/test runs passed on 2026-07-20 with warnings
treated as errors for the library:

| System | Architecture | Compiler | Native execution |
|---|---|---|---|
| Darwin 25.5 | arm64 | AppleClang 21.0 | passed |
| Darwin 25.5 via Rosetta | x86-64 | AppleClang 21.0 | passed |
| Ubuntu 24.04 | x86-64 | GCC 13.3 | passed |
| Windows 11 | x86-64 | MinGW GCC 14.2 | passed |
| Bianbu Linux 6.6 | riscv64 (Spacemit X60) | GCC 14.2 | passed |

The native test suite checks full-width constants, all bootstrap arithmetic
operations, forced register spilling, invocation validation, and 5,000 seeded
random comparisons against the interpreter oracle. It also checks helper calls
and interruptible safepoints in straight-line and loop CFG code. Machine
addresses and credentials are intentionally kept outside version control.

## Executable-memory policy

Windows uses `VirtualAlloc` as read/write followed by `VirtualProtect` to
read/execute. POSIX systems use `mmap` as read/write followed by instruction
cache synchronization and `mprotect` to read/execute. Published mappings are
never writable and executable at the same time. A zero-filled aligned prefix
keeps Clang's indirect-function sanitizer probe inside the mapping while
identifying generated entries as intentionally uninstrumented dynamic code.
