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

The RISC-V backend requires the `M` extension for native integer multiply,
floor division, and modulo. The
Float64 tier requires the `D` extension. The assembler, ABI selection, register
allocator, and executable-memory publisher are owned by UniJIT and do not link
against SLJIT or another JIT library.

Every compilation retains a validated immutable target profile. Portable
profiles require AArch64 FP64/NEON, x86-64 SSE2, or RV64IMD as appropriate;
optional x86 features are discovered with CPUID plus OS-state validation, and
Linux RISC-V vector availability is discovered from `AT_HWCAP`. Code caches
are profile-scoped and reject mismatched native generations. See
[`TARGET_PROFILES.md`](TARGET_PROFILES.md).

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

On 2026-07-21 the Word-comparison, compact-CFG-frame, and Lua structured
`if/else` change set was rerun natively on Darwin arm64, Darwin x86-64 through
Rosetta, and the Bianbu riscv64 host. Each target passed the core suite, live
baseline-to-optimized Lua suite, and the same 512-program by 64-input extended
differential corpus; the riscv64 core run also executed the greater-than-2,048
node compact-frame regression inside its signed 12-bit stack-addressing limit.

Also on 2026-07-21, bounded Word memory was executed natively on Darwin arm64,
Darwin x86-64 through Rosetta, Ubuntu x86-64, Windows x86-64, and the Bianbu
riscv64 host. The shared matrix checks 8/16/32/64-bit loads and stores,
native/little/big byte order, signed extension, naturally aligned fast paths,
deliberately unaligned paths, exact stored bytes, diagnosed failures, and
stack-map ownership against the reference interpreter. Ubuntu used GCC 13.3,
Windows used MinGW GCC 14.2, and Bianbu used GCC 14.2; each warnings-as-errors
build passed the full eight-test qualification configuration. The Windows host
could not reach GitHub during this run, so the exact `main` Git bundle was
transferred over its registered SSH channel before the native build.

The versioned bounded-memory benchmark was also run at commit `c9d338b` with
1,000 warmup iterations, three samples of 10,000 iterations, exact checksum and
byte comparison, and no release threshold:

| Native host | Aligned native u64 | Unaligned big-endian u64 | Interpreter speedup |
|---|---:|---:|---:|
| Darwin arm64 | 6.021 ns | 5.125 ns | 20.050x / 21.009x |
| Ubuntu x86-64 | 3.772 ns | 3.595 ns | 10.971x / 11.865x |
| Windows x86-64 | 4.510 ns | 4.280 ns | 24.186x / 25.797x |
| Bianbu riscv64 | 53.782 ns | 71.898 ns | 11.045x / 8.369x |

At commit `61f52ad`, bounded Float32/Float64 memory and standalone 16/32/64-bit
byte swap passed the same warnings-as-errors eight-test qualification build on
Darwin arm64, Ubuntu x86-64 with GCC 13.3, Windows x86-64 with MinGW GCC 14.2,
and Bianbu riscv64 with GCC 14.2. Darwin x86-64 through Rosetta also executed
the native unit suite. The matrix compares both IR forms and optimized IR with
the interpreter, checks exact bytes for native/little/big order and aligned or
unaligned access, and reconstructs live Word and Float64 values after a
diagnosed out-of-bounds exit.

The v2 benchmark at commit `8e78b8e` uses deterministic finite binary64 input
bits so FMA contraction cannot change cross-host checksums. With 1,000 warmup
iterations and three 10,000-iteration samples, every host produced
`0x599b3a9cbe610f21` for both u64 paths, `0xd44953875e3e458c` for aligned f64,
and `0xf0fd39bd683c6573` for unaligned big-endian f32:

| Native host | Aligned native u64 | Unaligned big u64 | Aligned native f64 | Unaligned big f32 |
|---|---:|---:|---:|---:|
| Darwin arm64 | 2.800 ns (18.682x) | 2.750 ns (19.059x) | 2.800 ns (19.487x) | 2.542 ns (21.500x) |
| Ubuntu x86-64 | 3.735 ns (10.654x) | 3.538 ns (11.683x) | 3.575 ns (12.604x) | 3.574 ns (12.146x) |
| Windows x86-64 | 4.740 ns (23.909x) | 4.510 ns (25.647x) | 4.730 ns (25.751x) | 4.730 ns (26.789x) |
| Bianbu riscv64 | 55.672 ns (10.784x) | 73.776 ns (8.299x) | 56.901 ns (11.336x) | 71.897 ns (8.900x) |

These values establish replayable architecture baselines rather than a
commercial floor. The RISC-V unaligned big-endian path intentionally uses byte
operations to avoid implementation-dependent misaligned traps, which is
reflected in its latency and larger function.

The native test suite checks full-width constants, all bootstrap arithmetic
operations, forced register spilling, invocation validation, and 5,000 seeded
random comparisons against the interpreter oracle. It also checks helper calls
and interruptible safepoints in straight-line and loop CFG code. CFG helper
coverage includes mixed Word/Float64 register and stack arguments, live values
across repeated calls, effectful dead results, and diagnosed exits after calls.
Machine addresses and credentials are intentionally kept outside version
control.

## Executable-memory policy

Windows uses `VirtualAlloc` as read/write followed by `VirtualProtect` to
read/execute. POSIX systems use `mmap` as read/write followed by instruction
cache synchronization and `mprotect` to read/execute. Published mappings are
never writable and executable at the same time. A zero-filled aligned prefix
keeps Clang's indirect-function sanitizer probe inside the mapping while
identifying generated entries as intentionally uninstrumented dynamic code.
