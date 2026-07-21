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
values. Floating-point parameters, constants, addition, subtraction,
multiplication, division, register spills, and results use a uniform 64-bit
value-bits calling convention suited to dynamic-language runtimes. Each backend
keeps Float64 values in its native floating-point register bank and only
transfers result bits to the shared return register at the compiled-function
boundary. Ordered Float64 `<` and `<=` comparisons produce Word results with
IEEE-754 unordered-false behavior, allowing frontends to materialize native
language Boolean values without routing through a runtime helper.
Float64 equality and inequality additionally preserve numeric signed-zero
equality and make every NaN unequal, matching the specialized Number/float
contracts of QuickJS and PocketPy without comparing raw value bits.
Float64 negation is a typed unary operation that toggles only the IEEE-754 sign
bit. It therefore reverses both signed zeroes and infinities while retaining
every NaN payload bit; the optimizer folds the same bit operation, and each
native backend uses an architecture-specific sign operation rather than
lowering negation as subtraction from positive zero.

Effectful runtime helpers use one portable signature: a pointer to a flat
value-bits argument area plus its element count, returning one value-bits word.
Calls are explicit effectful SSA definitions in both straight-line and CFG IR
and cannot be removed when their result is dead. Lowering saves caller-clobbered
Word and Float64 values that remain live, materializes ordered mixed-type
arguments from registers or spills, satisfies each platform's stack and
shadow-space ABI, and preserves the link register on AArch64 and RISC-V 64.

Native entries also accept an optional execution context. Explicit effectful
safepoints poll its lock-free sticky interrupt flag, publish a stable exit site,
restore the complete native frame, and return through the normal ABI boundary.
Effectful Float64 nonzero guards distinguish both signed zeroes using value bits
without misclassifying NaNs, then publish a diagnosed runtime exit for frontend
exception reconstruction. Immutable site metadata describes the semantic exit,
frontend resume offset, and typed recovery of entry arguments, constants, and
the exact guarded value. The same exit semantics are implemented by the
reference interpreters and native lowering on all three architectures. Null
contexts bypass polling for bounded trusted code; guarded functions use managed
invocation as described in `doc/RUNTIME.md`, with the reconstruction boundary
defined in `doc/DEOPTIMIZATION.md`.

Every guard and safepoint also receives an immutable canonical stack map.
Architecture-independent backward dataflow computes the typed SSA values live
before the effect, including fixed-point loop liveness and block-parameter to
edge-argument translation. AArch64, x86-64, and RISC-V 64 lowering flush live
register values to stable frame offsets before the effect, then records the
native instruction offset and complete frame size. Compiled functions and
cache leases expose this metadata without exposing physical register layouts;
diagnosed exits copy up to the enforced per-site bound into an allocation-free
execution-context area before restoring the frame, and the exact attempted
generation reconstructs the typed bits after ABI return. The contract and
remaining frame-installation boundary are specified in `doc/STACK_MAPS.md`.

Reconstructed primitive slots can feed a site-bound object materialization
plan. UniJIT validates all recovered inputs and graph references, allocates
every frontend-owned object shell before populating fields so cycles retain
identity, stages every primitive and object logical-frame slot, and commits the
graph and frame through one explicit transaction with exactly-once rollback.
Compiled functions and retained cache leases expose the same operation without
embedding runtime layouts in target backends. The contract is specified in
`doc/MATERIALIZATION.md`.

Running interpreters can cross the opposite boundary through a site-and-resume
bound OSR frame and entry plan. The plan maps at most 64 typed logical slots to
the exact native signature without allocation on a successful transfer, while
the selected compiled function or cache lease retains both the native mapping
and deoptimization metadata. Diagnosed exits keep the marshalled arguments for
generation-correct reconstruction. The contract and frontend integration
boundary are specified in `doc/ON_STACK_REPLACEMENT.md`.

The control-flow path adds a separate CFG SSA representation with explicit
basic-block parameters. Predecessor edges supply every block argument, making
phi semantics visible in construction, verification, interpretation, and
lowering. The verifier rejects unreachable blocks, malformed edges, and any
use whose definition does not dominate it. Its reference interpreter applies
edge arguments in parallel and has a configurable block-execution budget so
fuzzed infinite loops fail deterministically.

CFG signatures, constants, instructions, and block parameters carry the same
Word/Float64 types as straight-line SSA. Edges must preserve each destination
parameter's type, and Float64 addition, subtraction, negation, multiplication,
and division retain exact value bits through loop backedges. Ordered Float64
comparisons return false for NaN inputs and produce Word conditions suitable
for CFG branches. Equality is false and inequality is true for unordered
operands, while both signed zeroes compare equal. Effectful CFG Float64
nonzero guards consume one dominated
Float64 value, retain their source site, and use the same diagnosed runtime-exit
ABI and immutable frame-reconstruction metadata as straight-line guards.
Effectful CFG helper calls likewise require every argument to dominate the call,
can return either Word or Float64 bits, and remain valid inside branches and
loops. Per-call liveness preserves only values needed after the call, while one
bounded frame area handles any mixture of register-resident and spilled
arguments.
Block-local allocation uses independent Word and Float64 register banks, so
loop-carried floating-point values remain in native floating-point registers
across arithmetic, comparisons, and CFG backedges. Bit-level transfers are
limited to the value-bits ABI, diagnosed guards, canonical stack-map capture,
and real spills.

Native CFG lowering applies block-local lifetime analysis on AArch64, x86-64,
and RISC-V 64. Values stay in registers within a block; only actual spills and
definitions consumed directly by another block need canonical stack storage.
An architecture-independent edge planner resolves parallel block-parameter
moves independently for each register class, breaks Word and Float64 cycles
through their respective reserved scratch registers, and falls back to typed
temporary stack slots when an edge exceeds either target register bank. Branch
fixups are range-checked by each encoder before executable code is published.

## Stable subsystem boundaries

- `include/unijit/ir`: target-independent SSA values and construction API.
- `include/unijit/jit`: compilation and invocation API.
- `include/unijit/runtime`: execution contexts and deoptimization metadata.
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

The first assumption tier implements this as one-shot shared tokens. Managed
entries register with their dependencies, invalidation wakes existing
safepoints and waits for quiescence, and both entry and return boundaries reject
invalid code. Cache lookup and publication retire stale generations without
revoking memory-safe execution leases. The contract is specified in
`doc/ASSUMPTIONS.md`.

Tiered publication builds on that boundary with saturating invocation and
backedge profiles, atomic single-compiler claims, retry delay after compilation
failure, and immutable baseline/optimized state snapshots. Expected-generation
checks reject late compiler results, while readers switch without taking the
publication mutex. Assumption exits withdraw the affected optimized generation;
only frontends that explicitly declare a computation restartable may request
automatic baseline retry. The contract is specified in `doc/TIERING.md`.

Straight-line and CFG compilation have explicit baseline and optimized modes. The
baseline preserves verification and all exit metadata but skips optimizer
latency; the optimized CFG mode folds constants, canonicalizes safe Word
identities, removes dead pure nodes, folds constant branches and recursively
prunes unreachable blocks, and performs block-local value numbering without
crossing calls, guards, or safepoints. Effects remain ordered, unreachable exit
metadata is pruned, and live guard-scoped captures are rooted and remapped.
Optimized mode remains the default for compatibility.
PocketPy uses these modes in production callables,
retains exact source for recompilation, and publishes an optimized generation
after a single hotness claimant wins. Runtime exits carry the exact attempted
code lease so a concurrent switch cannot redirect frame reconstruction to the
wrong metadata generation.

Background optimization runs through a fixed-worker bounded scheduler rather
than detached frontend threads. Admission is constrained by queued task count
and estimated bytes, identical identity/generation work is deduplicated, and
weighted priorities preserve urgent latency without starving background work.
Copyable tickets retain completion and cooperative cancellation state, while
drain or cancel shutdown closes admission and joins the worker set. Frontends
still own immutable input lifetime and generation-checked tier publication as
defined in `doc/COMPILATION_SCHEDULER.md`.

Code is created writable, populated, instruction-cache synchronized, and only
then published executable. Published code is never writable. A bounded,
thread-safe LRU cache publishes immutable functions behind copyable execution
leases. Replacement, invalidation, eviction, and cache destruction remove
lookup visibility while active leases keep mappings and reconstruction metadata
alive. The current shared ownership implementation can later move to epoch-based
reclamation without changing the public contract in `doc/CODE_CACHE.md`.

## Resource governance

Public compilation applies configurable positive budgets to parameters, IR
nodes and arguments, CFG blocks and edges, exit and stack-map metadata, and
final native code. Shape and metadata limits run before verification so
quadratic control-flow analysis cannot be triggered by an unbounded public
input; emitted metadata and code bytes are checked again before executable
memory publication. QuickJS and PocketPy also cap retained source at 1 MiB.
The complete contract is specified in `doc/COMPILATION_LIMITS.md`.

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
4. Optimizing tier: profiles, CFG SSA, core optimization passes, canonical
   stack maps, deoptimization, complete frame installation, and on-stack
   replacement.
5. Additional frontends: QuickJS and PocketPy adapters sharing the same core.
6. Release qualification: sustained stress, fuzzing, security review,
   performance gates, artifacts, changelog, and monitored release pipelines as
   specified in `doc/QUALIFICATION.md`.
