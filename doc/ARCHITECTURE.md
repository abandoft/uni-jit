# UniJIT architecture

## Product constraints

UniJIT owns its production compilation stack. LuaJIT and SLJIT are retained
as implementation references and benchmark inputs only; neither is a runtime
dependency. The architecture must support three language frontends without
embedding language-specific object layouts or semantics in the optimizing
core.

Correctness, predictable latency, and diagnosability are release gates along
with throughput. Every optimized operation must have an interpreter oracle,
and every performance claim must be backed by a reproducible benchmark result.

## Compilation pipeline

```text
language bytecode
      |
      v
frontend graph builder  -- language guards, calls, exits
      |
      v
typed SSA IR            -- target-independent values and control flow
      |
      v
verification            -- dominance, types, effects, deopt state
      |
      v
optimization pipeline   -- folding, GVN, DCE, range/alias analysis
      |
      v
low-level MIR           -- explicit ABI, flags, calls and constraints
      |
      v
register allocation     -- linear scan first; regional allocator later
      |
      v
native encoder          -- AArch64, then x86-64
      |
      v
W^X code cache          -- publication, unwind/stack maps, reclamation
```

The initial vertical slice deliberately uses straight-line integer SSA. It
must still pass through the same verifier, lifetime analysis, register
allocation, native encoder, and executable-memory boundary that later tiers
will use. This makes the bootstrap implementation extensible instead of a
throwaway assembler demo.

The straight-line IR is type checked for 64-bit words and IEEE-754 Float64
values. Floating-point parameters, constants, arithmetic, register spills,
and results use a uniform 64-bit value-bits calling convention suited to
dynamic-language runtimes. Each backend keeps Float64 values in its native
floating-point register bank and only transfers result bits to the shared
return register at the compiled-function boundary.

Effectful runtime helpers use one portable signature: a pointer to a flat
value-bits argument area plus its element count, returning one value-bits word.
Calls are explicit SSA definitions and cannot be removed when their result is
dead. Lowering saves caller-clobbered Word and Float64 values that remain live,
materializes the per-call argument area, satisfies each platform's stack and
shadow-space ABI, and preserves the link register on AArch64 and RISC-V 64.

The optimizing tier adds a separate CFG SSA representation with explicit
basic-block parameters. Predecessor edges supply every block argument, making
phi semantics visible in construction, verification, interpretation, and
lowering. The verifier rejects unreachable blocks, malformed edges, and any
use whose definition does not dominate it. Its reference interpreter applies
edge arguments in parallel and has a configurable block-execution budget so
fuzzed infinite loops fail deterministically.

Native CFG lowering applies block-local lifetime analysis on AArch64, x86-64,
and RISC-V 64. Values stay in registers within a block; only actual spills and
definitions consumed directly by another block need canonical stack storage.
An architecture-independent edge planner resolves parallel block-parameter
moves, breaks register cycles through a reserved scratch register, and falls
back to temporary stack slots when an edge exceeds the target register bank.
Branch fixups are range-checked by each encoder before executable code is
published.

## Stable subsystem boundaries

- `include/unijit/ir`: target-independent SSA values and construction API.
- `include/unijit/jit`: compilation and invocation API.
- `src/ir`: verification and the reference interpreter.
- `src/jit`: lowering orchestration, code ownership, and executable memory.
- `src/jit/backend/<arch>`: MIR constraints and native instruction encoding
  for AArch64, x86-64, and RISC-V 64.
- `frontend/<language>`: bytecode decoding, runtime guards, and deoptimization.
- `benchmark`: pinned workloads, runners, environment manifests, and reports.

Frontend code may depend on the IR and runtime API. The IR cannot include a
frontend header. Architecture-specific code cannot escape its backend
directory except through the internal backend contract.

## Runtime strategy

The planned runtime is tiered:

1. A low-latency baseline compiler records type and branch profiles.
2. Hot regions are promoted to optimized SSA compilation.
3. Guards preserve language semantics and carry reconstruction metadata.
4. Invalidated code exits through deoptimization rather than silently using
   stale assumptions.

Code is created writable, populated, instruction-cache synchronized, and only
then published executable. Published code is never writable. Future concurrent
reclamation will use epochs so executing threads cannot observe freed code.

## Performance policy

Performance changes require warmup-aware distributions rather than a single
best timing. The benchmark harness will record source revision, compiler,
flags, CPU/OS, warmup policy, sample count, median, and tail latency. Comparisons
against LuaJIT, V8 Jitless/V8, and Python JIT use identical workloads where
language semantics permit, with semantic differences listed in the report.

## Delivery gates

1. Bootstrap: integer SSA, verifier, interpreter oracle, AArch64/x86-64/RV64
   native code, spill-capable register allocation, W^X memory, and differential
   tests.
2. Portable baseline: calls, branches, floating point, code-cache metrics,
   sanitizers, mobile targets, and cross-platform CI.
3. Lua tier: Lua bytecode frontend, guards/exits, benchmark corpus, and parity
   testing against the stock interpreter.
4. Optimizing tier: profiles, CFG SSA, core optimization passes, stack maps,
   deoptimization, and on-stack replacement.
5. Additional frontends: QuickJS and PocketPy adapters sharing the same core.
6. Release qualification: sustained stress, fuzzing, security review,
   performance gates, artifacts, changelog, and monitored release pipelines.
