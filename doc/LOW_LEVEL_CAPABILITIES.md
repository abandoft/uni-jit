# Commercial low-level capability roadmap

## Scope and independence

UniJIT owns its IR, optimizer, register allocator, calling conventions,
encoders, executable-memory manager, and runtime metadata. The feature list of
the pinned SLJIT source is useful as a maturity checklist, but no SLJIT API,
code generator, serialized state, calling convention, or internal data
structure is part of the UniJIT design. Every capability below has an
independent semantic contract, interpreter oracle, resource limit, and native
qualification plan.

The product targets remain AArch64, x86-64, and RISC-V 64. Adding 32-bit or
additional 64-bit architectures is lower priority than completing and
qualifying the shared commercial contract on those three targets.

## Gap assessment

| Capability | Current UniJIT state | Product decision | Priority |
|---|---|---|---|
| Scalar Word and Float64 operations | Implemented in both IR forms and all three backends | Continue expanding through the same typed contract | Delivered |
| SIMD | The strict 128-bit type/operation contract, both IR forms, verifier, interpreters, CFG whole-vector edges, folding, limits, differential generation, bounded vector memory, typed shared-SIMD allocation, aligned two-word vector slots, mixed-bank parallel copies, complete AArch64 Advanced SIMD/NEON and x86-64 SSE2 lowering, bounded stack-only RV64IMD scalar lowering, target-scoped lowering preflight/telemetry, and a retained three-architecture complete-CFG-loop performance gate for the current explicit surface are delivered | Retain this portable floor and add an optional profile-specific RVV fast path before claiming RVV or wider profiles | Delivered portable floor; optional profile follow-on |
| Typed memory, unaligned access, byte reversal | Bounded 8/16/32/64-bit Word memory, Float32/Float64 storage, standalone 16/32/64-bit byte reversal, fixed Word/Float64 frame slots, and preflighted trusted Word/Float64 object layouts are delivered in both IR forms, the interpreter, optimizer, and all three native backends | Use the completed scalar provenance floor for SIMD, atomics, and later FFI lowering | Delivered scalar floor |
| Generated-code atomics | Bounded typed atomic IR, verification, interpretation, optimization, runtime-exit metadata, capability telemetry, and independent x86-64, AArch64 LSE/bounded LL/SC, and RISC-V `A` AMO/bounded LR/SC lowering are delivered with exact-width helper fallback | Retain cross-target litmus, contention, sanitizer, real-host, and installed-package gates | Delivered |
| Fast internal calls | Typed Word/Float64 descriptors, interpreter oracles, immutable generation-leased target snapshots, managed fail-closed binding, independent three-backend lowering, concurrency stress, package consumption, and a retained complete-CFG-loop performance gate are delivered for context-free targets | Retain the generation-safe scalar floor, then add contextual targets and a profile-specific register-prefix convention without weakening external ABI safety | Delivered scalar floor; contextual/register follow-on |
| Tail calls | Not represented | Add verified tail transfers after fast calls and unwind metadata exist | P2 |
| Self-modifying code | Published code remains immutable RX; function-owned atomic RW non-executable patch cells, managed binding, lease lifetime, and three-backend acquire loads are delivered | Retain data-only retargeting and never reopen published code pages | Delivered |
| Direct physical-register access | Intentionally absent from public IR | Keep physical registers private to MIR; expose only verified role constraints | Architectural rule |
| Function-local stack storage | Fixed zero-initialized Word/Float64 slots, optional slot zeroization, resource limits, and three-backend lowering are delivered | Add aligned vector/aggregate slots, lexical lifetime reuse, stack probing, and whole-value secret erasure | P1 partial |
| All-in-one hidden compilation | A versioned opaque C17 scalar embedding floor now owns builders, functions, compilers, contexts, caches, compiled generations, and bounded diagnostics without exporting C++ implementation symbols | Retain exact three-platform shared-library ABI gates, then extend new versioned surfaces only after their runtime contracts are complete | Delivered scalar floor; broader runtime surface follow-on |
| Serialization and AOT | No persistent compilation artifact | Add canonical portable IR packages first, then validated target code objects | P1 |

P0 closes a prerequisite for several language and numerical workloads. P1 is
required before claiming a broadly embeddable commercial backend. P2 improves
call-heavy workloads after the metadata and lifecycle foundations are proven.

## Findings from the SLJIT capability checklist

The short SLJIT README is useful because it groups several facilities that a
commercial low-level JIT eventually needs, but the linked LIR interface also
shows why a feature name alone is not an adequate UniJIT contract:

- SIMD covers register widths, element widths, aligned and unaligned memory,
  splats, lane movement, signed/unsigned widening, lane-sign extraction,
  bitwise operations, shuffles, and a way to test availability without
  emitting code. UniJIT therefore needs explicit typed semantics and a
  preflight legalization report, not a single `supports_simd` Boolean.
- SLJIT's atomic surface exposes a low-level paired load/store transaction with
  natural-alignment obligations. UniJIT will instead expose complete atomic
  operations with verifier-checked memory orders, while retaining natural
  alignment, availability testing, and explicit lowering fallbacks.
- Raw local stack bytes are useful but too permissive for untrusted frontend
  IR. UniJIT's delivered floor uses non-addressable typed slots, per-invocation
  zero initialization, explicit sensitivity metadata, and a fixed resource
  budget. Raw local addresses and unbounded `alloca` remain excluded.
- Rewritable instructions are not adopted. Immutable RX code plus separate
  atomic non-executable patch cells provides retargeting without reopening a
  published code page.
- Fast calls, tail calls, hidden compilation state, and resumable AOT are valid
  product requirements, but they require generation leases, stack maps,
  unwind metadata, a versioned C boundary, and validated artifacts rather than
  exposing a platform-independent assembler directly.
- Direct physical-register access and the long tail of 32-bit or additional
  architectures are deliberately not copied. Typed allocation and deep
  qualification of AArch64, x86-64, and RISC-V 64 take priority.

This assessment is a requirements input only. No SLJIT opcode, register model,
serialization format, generated-code convention, or implementation source is
part of UniJIT.

## Target and feature profiles

Native compilation receives an immutable target profile containing the
architecture, operating-system ABI, endianness, scalar ISA baseline, optional
instruction features, and vector-width policy. The profile is part of the code
cache identity and serialized-artifact compatibility key. A compiled function
may only execute on a host whose discovered feature set contains its required
set.

The public target-profile contract, validation, host discovery, compiled-code
identity, and profile-scoped code-cache enforcement are now implemented; see
[`TARGET_PROFILES.md`](TARGET_PROFILES.md). Cross-architecture object emission
remains deferred to the portable IR/AOT stage and is rejected rather than
inferred from the build host.

Host discovery uses CPUID plus XGETBV on x86-64, operating-system capability
interfaces on AArch64, and the Linux RISC-V hardware-probe or auxiliary-vector
interfaces where available. Cross compilation requires an explicit profile and
must not infer target features from the build host. Unknown features select the
portable baseline rather than optimistic emission.

The profile vocabulary must contain every feature that changes legality or ABI
state. AArch64 LSE discovery and profile isolation are delivered; x86 SSE4.1
remains required before any legalization selects it, and RISC-V `A` discovery,
profile isolation, and atomic lowering are delivered. A profile does not infer
optional atomic support from the architecture name, and RVV execution must be
authorized by both ISA discovery and operating-system vector-state support.

The current minimum profiles remain AArch64 with FP64, x86-64 with SSE2, and
RV64IMD. Optional profiles are monotonic: code compiled for a wider profile is
stored and invalidated independently from baseline code.

## Scalar and vector register foundation

The delivered typed allocator maps Float64 and vector values into one
conflict-free physical SIMD/FP bank while keeping their IR types and operation
constraints distinct. It can instead force vectors onto aligned 16-byte stack
slots for targets without an enabled vector register file. A backend register
description records allocatable,
caller-clobbered, callee-preserved, argument, result, and scratch roles instead
of assuming that instruction support alone makes the full register file safe.

The AArch64 scalar backend exposes caller-clobbered `v0`–`v7` and
`v16`–`v29` to straight-line and CFG allocation, reserves `v30`/`v31` as lowering scratch, and
does not allocate callee-preserved `v8`–`v15` until prologue/epilogue save
selection is implemented. Live Float64 values are saved around runtime-helper
calls, so widening this pool does not weaken the helper ABI. x86-64 currently
allocates XMM1–XMM4 plus XMM6–XMM15 under System V while reserving XMM0/XMM5
for lowering scratch. Windows retains the common volatile XMM1–XMM4 floor;
using XMM6–XMM15 there requires target-specific nonvolatile save tracking.
RV64 uses its existing caller-clobbered floating-point pool for Float64 and the
explicit stack-only policy for vectors until RVV selection is qualified.
The x86-64 backend asks CFG allocation to let a final-use Float64 left operand
donate its physical register to the result, which maps recurrent SSA arithmetic
to destructive two-address SSE2 instructions without an avoidable move.
AArch64 and RISC-V retain their ordinary three-address allocation policy.

Qualification must force more simultaneously live scalar/vector values than
each target's volatile pool, cross CFG edges and helper calls, and verify both
register and stack paths. Performance records report target profile and native
code size so a wider allocator cannot be accepted solely from a source-level
benchmark improvement.

## Typed memory contract

Memory IR is a prerequisite for vectors, atomics, inline caches, and efficient
runtime object access. The delivered bounded slice covers signed and unsigned
8-, 16-, 32-, and 64-bit Word loads and matching stores, Float32 and Float64
storage through the Float64 SSA type, complete 128-bit transfers for every data
vector shape, and standalone 16/32/64-bit Word byte reversal in straight-line
and CFG IR. It includes the interpreter oracle, optimizer preservation,
resource limits, live-value stack maps, diagnosed exits, and AArch64, x86-64,
and RISC-V 64 native or bounded scalar lowering.
[`TYPED_MEMORY.md`](TYPED_MEMORY.md) is the normative delivered contract.
The vector transfer extension is specified in
[`PORTABLE_SIMD.md`](PORTABLE_SIMD.md).
Trusted runtime-object layouts are now a separate delivered provenance mode;
their fixed primitive fields, semantic identity, managed preflight, and direct
lowering are specified in [`TRUSTED_OBJECTS.md`](TRUSTED_OBJECTS.md).
Standalone address calculation and advanced vector/aggregate frame classes
remain follow-on work. The delivered fixed Word/Float64 slot floor is
specified separately in [`FRAME_LOCALS.md`](FRAME_LOCALS.md). Every bounded
region access records:

- a declared bounded-region index;
- byte width and required alignment;
- native, little-endian, or big-endian interpretation;
- volatility and alias class;
- the frontend exit site used when a dynamic bounds or alignment guard fails.

Address addition is checked as an unsigned operation before dereference.
Untrusted public IR cannot manufacture an arbitrary process pointer. Frontends
bind a base pointer and size to an execution-context region. The verifier
rejects malformed descriptors and undeclared regions; generated code and the
interpreter perform the unsigned dynamic bounds, permission, and absolute
alignment checks before every dereference. Trusted runtime-owned object layouts
are a distinct provenance mode rather than an implicit escape hatch: IR sees
only a declared layout slot and fixed field offset, while managed invocation
validates identity, size, alignment, and whole-function write permission before
native entry.

Unaligned scalar loads and stores have byte-exact semantics and never rely on C
or C++ undefined behavior. A backend may use one native unaligned instruction
when its target contract permits it; otherwise it emits aligned pieces and
combines them. Unaligned atomics are never synthesized and are rejected.

Vector transfers preflight the complete 16-byte range, write permission, and
absolute alignment before changing memory. They preserve lane order while
applying byte order independently within each typed lane, reject mask-vector
loads and stores, and keep RISC-V fallback sequences finite and independent of
RVV. Partial stores are not an observable failure mode.

Standalone byte reversal is exposed as one width-verified pure IR operation.
It reverses the low 16, 32, or 64 bits and zero-extends a narrow result. The
optimizer folds it, while native backends select target instructions or a
verified scalar sequence. UniJIT's generated-code format remains little-endian
on the current targets, while the portable serialized format uses a canonical
byte order.

## Portable SIMD design

### Types and strict semantics

The portable floor is 128-bit vectors: `I8x16`, `I16x8`, `I32x4`, `I64x2`,
`F32x4`, and `F64x2`. Masks have the same lane shape and contain either all
zeroes or all ones in each lane. Initial operations are:

- zero, constant, splat, lane extract, and lane insert;
- aligned and explicitly unaligned load/store;
- integer add, subtract, multiply where natively portable, and bitwise logic;
- Float32/Float64 add, subtract, multiply, and divide;
- ordered comparisons, mask logic, and lane select;
- verifier-checked signed and unsigned widening conversions;
- lane-sign extraction to a Word bit mask for text and byte-search kernels;
- a bounded compile-time shuffle with verifier-checked lane indices.

Lane zero is the lowest-addressed logical element. Integer lane extraction
states whether it sign-extends or zero-extends into `Word`; floating extraction
preserves the lane's exact IEEE bits. Vector memory uses the same bounded
region, alias, permission, alignment, and diagnosed-exit model as scalar
memory. Explicit byte order applies independently to each lane and never
reverses the logical lane sequence. A shuffle index is part of immutable IR;
dynamic swizzles are deferred until their out-of-range behavior is specified.

Floating-point vector operations are lane-wise versions of strict scalar
operations. The optimizer cannot contract multiply/add into FMA, reassociate a
reduction, flush subnormals, or select a reciprocal approximation unless a
separate frontend-visible relaxed-math mode authorizes that change. Strict mode
therefore remains reproducible across tiers. NaN-sensitive operations not
covered by the scalar contract, such as target-specific minimum/maximum, are
deferred until their exact semantics are specified.

Vector values retain an independent typed class while sharing the physical
SIMD/FP allocation bank with Float64 where the architecture requires it. Spill
and caller-clobber backup plans use 16-byte-aligned two-word slots, edge copies
remain parallel across mixed Float64/vector cycles, and call liveness identifies
vector caller-clobbers. The first release does not allow GC references inside
vector lanes, so scalar runtime-exit capture excludes vectors and vector
deoptimization remains unsupported.

### Backend mapping

- x86-64 uses the mandatory SSE2 profile for the portable 128-bit floor. The
  delivered System V and Windows lowering uses direct packed instructions for
  the profitable baseline surface and bounded stack/GPR legalization where
  SSE2 lacks the required lane operation. Optional AVX2 code will use VEX
  forms consistently and emit `VZEROUPPER` at required ABI boundaries; CPUID
  and OS XSAVE state must both authorize AVX.
- AArch64 uses Advanced SIMD/NEON 128-bit registers and instructions. The
  delivered lowering consumes the shared Float64/vector allocation plan for
  straight-line and CFG code, full-width spills, mixed parallel copies, and
  helper-call preservation. It implements the current operation surface
  directly except for deliberately legalized I64x2 multiplication and
  lane-sign-mask assembly.
- RISC-V 64 currently uses verified bounded RV64IMD scalar sequences with
  aligned stack-only vector storage, independent of optional vector state.
  A future RVV fast path will require an explicit target profile, set `vl` to
  the exact lane count without assuming a particular hardware VLEN, and retain
  the scalar path as its semantic and compatibility floor.

Wider vectors are target-profile specializations, not new frontend semantics.
They may combine independent 128-bit operations, but cannot change exception,
NaN, or reduction order. This prevents x86 AVX width or RISC-V VLEN from
silently changing a language result.

Some portable operations do not have one baseline instruction, such as all
integer multiply lane shapes under SSE2. The legalizer may use a verified
instruction sequence or scalar lanes, but the compilation result and telemetry
must report `native`, `legalized`, or `scalarized` for every vector operation
class. Automatic vectorization may only choose a scalarized operation when its
target cost model still proves a benefit.

### Capability preflight

An embedder can now ask whether a typed operation set can compile for a
specific immutable target profile without generating code or allocating
executable memory. `CapabilityReport` uses `native`, `legalized`, `scalarized`,
`helper`, and `unsupported`, and exposes required feature bits, vector width,
execution-context need, strategy counts, and per-vector-operation register and
aligned-stack resource masks. Both IR forms use the same fixed-size report.
Compilation rechecks the optimized IR, publishes that exact immutable report
through the compiled function and cache lease, and fails closed on malformed
IR or profiles. This delivered contract replaces per-op probe flags with one
cacheable, target-profile-scoped decision; see
[`TARGET_PROFILES.md`](TARGET_PROFILES.md).

### Vectorization stages

1. Land explicit vector IR, verifier, interpreter, and folding. This semantic
   slice is delivered and specified in
   [`PORTABLE_SIMD.md`](PORTABLE_SIMD.md); the shared/stack-only allocation
   foundation, AArch64 and x86-64 native lowering, and bounded RV64IMD scalar
   lowering are also delivered.
2. Add superword-level parallelism for independent isomorphic scalar nodes.
3. Add a counted-loop vectorizer with dependence and alias proofs, guarded
   alignment/bounds checks, a scalar epilogue, and deoptimization metadata.
4. Expose frontend intrinsics only after stock-engine differential tests prove
   the language-level contract.

Each stage has bit-exact differential fuzzing. Performance gates use complete
loops, report scalar and vector code size, and compare against each target's
baseline profile on real hardware.

## Atomic operations

The normative operation, memory-order, provenance, progress, and qualification
contract is frozen in [`ATOMICS.md`](ATOMICS.md). The shared semantic layer,
independent lowering on all three product architectures, cross-target litmus
qualification, and installed-package consumption are delivered. Data-only
patch cells are independently specified in [`PATCH_CELLS.md`](PATCH_CELLS.md)
and are not part of the bounded-memory atomic operation claim.

Generated-code atomics operate on naturally aligned 8-, 16-, 32-, or 64-bit
integer locations from a verified memory region. The IR supports atomic load,
store, exchange, compare-exchange, fetch add/and/or/xor, and fences with
`relaxed`, `acquire`, `release`, `acq_rel`, or `seq_cst` order where meaningful.
Invalid load/store order combinations are verifier errors. Compare-exchange
returns both the observed value and a success condition, distinguishes weak
from strong form, and carries separate success and failure orders. A failure
order cannot be `release` or `acq_rel` and cannot be stronger than the success
order. The expected value is never modified through an implicit host pointer.

x86-64 lowers through its TSO rules and `LOCK` operations. The delivered
AArch64 backend selects LSE only when the feature profile permits it and
otherwise performs at most 16 LL/SC attempts before invoking a lock-free atomic
progress helper; its capability record reports `native` plus the LSE feature or
`helper` for the baseline profile. RISC-V 64 requires an explicit `A` profile,
uses native 32/64-bit loads, stores, fences, and AMOs, bounds LR/SC
compare-exchange to 16 attempts, and routes exact-width 8/16-bit cells through
the progress helper without touching adjacent bytes. Capability records expose
every possible fallback. GC pointer atomics are deferred until the owning
frontend supplies write barriers and root visibility.

Qualification combines a deterministic memory-model litmus corpus, contended
compare-exchange stress, ThreadSanitizer runs for the runtime integration, and
real-host tests on all three architectures. The default gate covers
release/acquire message passing and sequentially consistent store buffering;
the extended gate retains a versioned JSON record. Hosted Windows x86-64 and
installed-package execution are included, while real RISC-V 64 qualification
adds full-width and subword contention plus UBSan.

## Calls, tail transfers, and frame locals

The existing helper ABI remains the safe external boundary. The delivered first
JIT-to-JIT layer uses typed Word/Float64 descriptors in both IR forms, an
explicit interpreter-oracle table, and the compiled native-entry ABI. Managed
invocation acquires one immutable target-table snapshot, fails closed if any
slot is unbound, and keeps every exact callee generation alive through a cache
lease. Bind and clear operations release-publish complete replacement tables,
so eviction or concurrent retargeting cannot create a dangling or mixed target.
Only context-free callees with exact signatures and target profiles are admitted;
raw caller entries are withheld. The normative contract is in
[`FAST_CALLS.md`](FAST_CALLS.md).

A later private convention may pass a bounded prefix of Word, Float64, and
vector arguments in target registers while reserving the execution context and
publishing a complete stack map at contextual call sites. It must preserve the
existing descriptor, generation-snapshot, and managed-entry contract.

A tail transfer is legal only when signatures and target profiles match, no
caller value remains live, pending materialization and cleanup obligations are
empty, the outgoing argument area fits the verified frame, and unwind/stack
walk metadata can describe the transition. Lowering tears down the caller
frame before the final jump. Otherwise the compiler emits a normal fast call.

The delivered controlled-frame floor provides fixed eight-byte Word or Float64
slots with whole-function lifetime, per-invocation zero initialization, and
optional slot zeroization on every native return path. Slots have no IR-visible
address and do not require an execution context. The exact contract is defined
in [`FRAME_LOCALS.md`](FRAME_LOCALS.md).

Aligned vector/aggregate slots and lexical lifetime reuse remain future work.
Dynamic or unbounded `alloca` is not part of the public contract. Larger frame
classes must account for ABI alignment, Windows shadow space, guard-page stack
probes, spill/call/edge temporary areas, and unwind metadata. Slot zeroization
does not by itself prove that copied register or spill values were erased; a
future sensitive-value data-flow class must carry that stronger obligation.

Physical registers remain a MIR implementation detail. Target-independent IR
may request a role such as call argument, return value, shift-count register,
or pinned execution context; the constraint solver chooses and verifies the
physical register. Arbitrary register numbers are not exposed to frontends.

## Immutable code and patch cells

UniJIT does not make an RX page writable to implement inline caches or direct
call retargeting. The delivered contract in
[`PATCH_CELLS.md`](PATCH_CELLS.md) keeps published instructions immutable and
places mutable state in separately allocated, non-executable, naturally aligned
function-owned cells that hold a value, target, shape, generation, or counter.
Generated code and runtime reads use acquire semantics, while runtime mutation
uses release publication, strong compare-exchange, or fetch-add updates.

Patch-cell ownership follows the compiled-function lease. Cache invalidation
retires lookup visibility while outstanding handles keep both executable code
and its private cell array alive. Managed invocation supplies the internal cell
base, and `native_entry()` is unavailable for a function with cells so the
binding cannot be bypassed. The current patch-cell target kind remains data
only; verified indirect JIT calls now use the separate typed generation-leased
table in [`FAST_CALLS.md`](FAST_CALLS.md), while polymorphic policy and GC target
retention remain later runtime contracts. This provides the useful data part of
code patching without weakening W^X, instruction-cache coherency, or concurrent
reclamation.

## Encapsulation, serialization, and AOT

The first public commercial embedding boundary is now delivered as the opaque
versioned C17 scalar API specified in [`EMBEDDING_C_API.md`](EMBEDDING_C_API.md).
It owns builders, functions, compilers, target profiles, compiled functions,
execution contexts, code caches, generation-stable handles, and errors while
keeping C++ graphs and native encoders hidden. Fixed-width extensible structures,
structured status codes, 1,024-byte diagnostics, exception containment, explicit
ownership, resource budgets, installed C consumption, an exact 67-symbol export
manifest, and real Ubuntu x86-64, Windows x86-64, and Apple AArch64 shared-library
gates define the delivered floor. The current v1 builder intentionally exposes
only scalar straight-line construction, guards, safepoints, context-free fast
calls, and data patch cells. CFG, memory/object bindings, atomics, SIMD, recovery,
tiering, scheduling, allocation callbacks, and serialized artifacts remain
fail-closed until separately versioned contracts are complete.

Persistent compilation is split into two formats:

1. A portable IR package contains canonical types, CFG, constants, frontend
   semantic identity, exits, stack-map requirements, assumptions, resource
   budgets, and the selected pipeline checkpoint.
2. A target code object additionally contains the exact target/ABI/feature
   profile, immutable code bytes, relocation records, read-only constants,
   patch-cell descriptors, stack maps, deoptimization data, unwind data, and
   compiler build identity.

The versioned portable representation and its untrusted-input boundary are
specified in [`PORTABLE_IR.md`](PORTABLE_IR.md). Version 1 uses a canonical
little-endian wire format, a fixed allocation-driving manifest, SHA-256 payload
integrity, explicit decode budgets, complete post-decode IR verification, and
byte-identical rebuild qualification. Runtime helper addresses are forbidden;
typed fast-call slots remain unbound until the reconstructed function is
compiled and explicitly connected.

Neither format serializes host pointers, C++ object layouts, mutex state, cache
leases, or writable executable memory. Portable IR decoding is
length-delimited, overflow-checked, checksummed, resource-limited before
allocation, verified after decoding, and fuzzed as an untrusted input. Native
machine code cannot be made safe through structural decoding alone: a native
object is accepted only from the same trusted compiler identity under the
embedder's required signature or explicit trust policy, and untrusted native
objects are never mapped. It is rejected on any schema, frontend semantic hash,
compiler build, target, ABI, endianness, feature, or security-policy mismatch.

Resumable compilation restarts only at a validated pipeline checkpoint; it
does not restore a raw in-memory compiler instance. Native objects are relocated
into writable non-executable storage, validated, synchronized, and changed to
RX through the existing publication boundary. Deterministic builds must emit
byte-identical packages for identical inputs and target profiles.

## Delivery and qualification order

1. Complete bounded typed memory, byte reversal, unaligned scalar access, frame
   slots, target profiles, and negative verifier tests. Target profiles, Word
   and Float scalar memory, standalone byte reversal, and the fixed
   Word/Float64 frame-slot floor are delivered; trusted layouts and advanced
   aligned/lifetime frame classes remain.
2. Complete strict 128-bit SIMD. The typed IR, interpreter parity, optimizer,
   CFG whole-vector edge copies, resource limits, negative tests, differential
   corpus, target-scoped fail-closed boundary, shared SIMD allocation, aligned
   spill plans, call liveness, mixed-bank edge-cycle planning, complete
   AArch64 lowering, complete x86-64 SSE2 lowering, and bounded stack-only
   RV64IMD scalar lowering, bounded aligned/unaligned vector memory, and
   profile-specific feature preflight/telemetry for the current explicit
   surface are delivered; optional RVV lowering remains a profile enhancement.
3. Retain explicit SIMD and complete-loop performance gates on real AArch64,
   Ubuntu/Windows x86-64, and RISC-V 64 hosts. This gate is delivered with
   vector/scalar/interpreter parity, target lowering identity, fixed workload
   checksum, code-size ceiling, and minimum speedup ratios; emulation remains
   supplemental only.
4. Retain the delivered atomic memory-operation and data-only patch-cell gates,
   including concurrency, invalidation, sanitizer, reclamation, installed
   package, real-host, code-size, and managed-invocation performance evidence.
5. Retain the delivered typed generation-stable scalar JIT-to-JIT call floor,
   then deliver contextual/register-prefix calls, unwind-aware tail transfers,
   and generation-stable direct-call targets.
6. Retain the delivered opaque C17 scalar embedding ABI, then deliver the
   portable IR package, validated native AOT object, and deterministic rebuild
   gate.
7. Consider additional architectures only after all preceding contracts pass
   semantic, security, resource, and performance release gates on the required
   three architectures.

No item is considered delivered by instruction encoding alone. Completion
requires public contract documentation, verifier enforcement, interpreter
oracle coverage, optimized and baseline native parity, resource limits,
negative tests, fuzzing, real-host execution, and retained machine-readable CI
evidence.
