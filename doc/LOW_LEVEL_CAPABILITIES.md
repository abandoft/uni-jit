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
| SIMD | The strict 128-bit type/operation contract, both IR forms, verifier, interpreters, CFG whole-vector edges, folding, limits, differential generation, and fail-closed native boundary are delivered; vector allocation and native lowering are not | Complete bounded vector memory, spills/calls, NEON, SSE2, RVV or reported scalar fallback, capability telemetry, and real-host gates before wider profiles or loop vectorization | P0 partial |
| Typed memory, unaligned access, byte reversal | Bounded 8/16/32/64-bit Word memory, Float32/Float64 storage, standalone 16/32/64-bit byte reversal, fixed Word/Float64 frame slots, and preflighted trusted Word/Float64 object layouts are delivered in both IR forms, the interpreter, optimizer, and all three native backends | Use the completed scalar provenance floor for SIMD, atomics, and later FFI lowering | Delivered scalar floor |
| Generated-code atomics | Runtime control structures use C++ atomics; generated IR has none | Add typed atomic IR with explicit memory order and natural-alignment rules | P1 |
| Fast internal calls | Calls currently use the portable runtime-helper ABI | Add a private JIT-to-JIT convention without weakening external ABI safety | P1 |
| Tail calls | Not represented | Add verified tail transfers after fast calls and unwind metadata exist | P2 |
| Self-modifying code | Published code is immutable RX | Keep code immutable; use atomic RW non-executable patch cells and versioned replacement | P1 |
| Direct physical-register access | Intentionally absent from public IR | Keep physical registers private to MIR; expose only verified role constraints | Architectural rule |
| Function-local stack storage | Fixed zero-initialized Word/Float64 slots, optional slot zeroization, resource limits, and three-backend lowering are delivered | Add aligned vector/aggregate slots, lexical lifetime reuse, stack probing, and whole-value secret erasure | P1 partial |
| All-in-one hidden compilation | Native encoders are already internal | Add an opaque versioned C17 embedding ABI over the C++ implementation | P1 |
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

Before atomics and wider SIMD land, the profile vocabulary must add every
feature that changes legality or ABI state, including x86 SSE4.1 where used by
vector legalization, AArch64 LSE, and the RISC-V `A` extension. A profile must
not infer atomic support from the architecture name, and RVV execution must be
authorized by both ISA discovery and operating-system vector-state support.

The current minimum profiles remain AArch64 with FP64, x86-64 with SSE2, and
RV64IMD. Optional profiles are monotonic: code compiled for a wider profile is
stored and invalidated independently from baseline code.

## Scalar and vector register foundation

The scalar Float64 allocator and the future vector allocator share physical
SIMD/FP files, but they remain distinct typed allocation classes at the IR and
MIR boundaries. A backend register description records allocatable,
caller-clobbered, callee-preserved, argument, result, and scratch roles instead
of assuming that instruction support alone makes the full register file safe.

The AArch64 scalar backend already exposes caller-clobbered `v0`–`v7` and
`v16`–`v29` to CFG allocation, reserves `v30`/`v31` as lowering scratch, and
does not allocate callee-preserved `v8`–`v15` until prologue/epilogue save
selection is implemented. Live Float64 values are saved around runtime-helper
calls, so widening this pool does not weaken the helper ABI. x86-64 currently
allocates XMM1–XMM4 plus XMM6–XMM15 under System V while reserving XMM0/XMM5
for lowering scratch. Windows retains the common volatile XMM1–XMM4 floor;
using XMM6–XMM15 there requires target-specific nonvolatile save tracking.
RV64 uses its existing caller-clobbered floating-point pool. These scalar
decisions are prerequisites for SIMD but do not count as vector-IR delivery.
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
runtime object access. The delivered scalar slice covers signed and unsigned
8-, 16-, 32-, and 64-bit Word loads and matching stores, Float32 and Float64
storage through the Float64 SSA type, and standalone 16/32/64-bit Word byte
reversal in straight-line and CFG IR. It includes the interpreter oracle,
optimizer preservation, resource limits, live-value stack-map reconstruction,
diagnosed exits, and AArch64, x86-64, and RISC-V 64 native lowering.
[`TYPED_MEMORY.md`](TYPED_MEMORY.md) is the normative delivered contract.
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

Vector values use an independent register class. Spills are 16-byte aligned,
edge copies remain parallel, call liveness identifies vector caller-clobbers,
and stack maps describe the vector as one typed value. The first release does
not allow GC references inside vector lanes.

### Backend mapping

- x86-64 uses the mandatory SSE2 profile for the portable 128-bit floor.
  Optional AVX2 code uses VEX forms consistently and emits `VZEROUPPER` at
  required ABI boundaries. CPUID and OS XSAVE state must both authorize AVX.
- AArch64 uses Advanced SIMD/NEON 128-bit registers and instructions. The
  allocator keeps scalar Float64 and vector values in one correctly
  constrained physical register bank.
- RISC-V 64 uses RVV when the target and operating system expose vector state.
  Fixed 128-bit IR sets `vl` to the exact lane count and does not assume a
  particular hardware VLEN. Hosts without RVV lower the same IR to verified
  scalar lane operations until a profitable scalarization policy is proven.

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

An embedder must be able to ask whether a typed operation set can compile for a
specific immutable target profile without generating code. The planned report
uses four outcomes: `native`, `legalized`, `helper`, and `unsupported`, plus the
required feature bits, vector width, estimated temporary register class, and
whether execution needs an `ExecutionContext`. Compilation rechecks the same
decision so a stale preflight result cannot bypass verification. This replaces
per-op probe flags with one cacheable, target-profile-scoped contract.

### Vectorization stages

1. Land explicit vector IR, verifier, interpreter, and folding. This semantic
   slice is delivered and specified in
   [`PORTABLE_SIMD.md`](PORTABLE_SIMD.md); allocation and three-backend
   lowering remain in progress.
2. Add superword-level parallelism for independent isomorphic scalar nodes.
3. Add a counted-loop vectorizer with dependence and alias proofs, guarded
   alignment/bounds checks, a scalar epilogue, and deoptimization metadata.
4. Expose frontend intrinsics only after stock-engine differential tests prove
   the language-level contract.

Each stage has bit-exact differential fuzzing. Performance gates use complete
loops, report scalar and vector code size, and compare against each target's
baseline profile on real hardware.

## Atomic operations

Generated-code atomics operate on naturally aligned 8-, 16-, 32-, or 64-bit
integer locations from a verified memory region. The IR supports atomic load,
store, exchange, compare-exchange, fetch add/and/or/xor, and fences with
`relaxed`, `acquire`, `release`, `acq_rel`, or `seq_cst` order where meaningful.
Invalid load/store order combinations are verifier errors. Compare-exchange
returns both the observed value and a success condition, distinguishes weak
from strong form, and carries separate success and failure orders. A failure
order cannot be `release` or `acq_rel` and cannot be stronger than the success
order. The expected value is never modified through an implicit host pointer.

x86-64 lowers through its TSO rules and `LOCK` operations, AArch64 selects LSE
only when the feature profile permits it and otherwise uses bounded LL/SC
attempts followed by a progress-preserving helper fallback, and RV64 uses the
optional A extension's AMO or LR/SC operations under the same progress rule.
Runtime fallback helpers are also allowed for widths not lock-free on a target,
but the record and telemetry must disclose the fallback. GC pointer atomics are
deferred until the owning frontend supplies write barriers and root visibility.

Qualification combines a deterministic memory-model litmus corpus, contended
compare-exchange stress, ThreadSanitizer runs for the runtime integration, and
real-host tests on all three architectures.

## Calls, tail transfers, and frame locals

The existing helper ABI remains the safe external boundary. A private
JIT-to-JIT fast convention may pass a bounded prefix of Word, Float64, and
vector arguments in target registers while reserving the execution context and
publishing a complete stack map at the call site. Callee code is held through a
generation-stable cache lease or an atomic patch cell so eviction cannot create
a dangling target.

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

UniJIT will not make an RX page writable to implement inline caches or direct
call retargeting. Published instructions remain immutable. Mutable state lives
in separately allocated, non-executable, naturally aligned patch cells that
hold a target, shape, generation, or counter. Generated code reads a cell with
the required acquire semantics, while runtime publication uses release or
compare-exchange updates.

Patch-cell ownership follows the compiled-function lease. Replacement first
publishes the new data target, then retires old lookup visibility; active code
may continue using the old generation safely. Polymorphic growth is bounded,
telemetry reports misses and transitions, and megamorphic state falls back to a
runtime helper. This provides the useful part of code patching without
weakening W^X, instruction-cache coherency, or concurrent reclamation.

## Encapsulation, serialization, and AOT

The public commercial embedding boundary will be an opaque versioned C17 API.
It owns compiler, target-profile, artifact, compiled-function, error, and lease
handles while keeping the C++ graph builders and native encoders hidden. API
entry points never expose internal structure sizes and return structured status
codes plus bounded diagnostics. Separate compiler instances can be used
concurrently, while each mutable instance has explicit single-owner rules.
The embedder can provide bounded allocation, executable-memory publication,
logging, clock, and entropy callbacks without replacing verifier policy.

Persistent compilation is split into two formats:

1. A portable IR package contains canonical types, CFG, constants, frontend
   semantic identity, exits, stack-map requirements, assumptions, resource
   budgets, and the selected pipeline checkpoint.
2. A target code object additionally contains the exact target/ABI/feature
   profile, immutable code bytes, relocation records, read-only constants,
   patch-cell descriptors, stack maps, deoptimization data, unwind data, and
   compiler build identity.

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
   corpus, and fail-closed native boundary are delivered; vector memory,
   spills, calls, feature preflight, three-backend lowering, and scalar
   fallback where required remain.
3. Add explicit SIMD and complete-loop performance gates on real AArch64,
   Ubuntu/Windows x86-64, and RISC-V 64 hosts; emulation is supplemental only.
4. Deliver atomic memory operations and data-only patch cells with concurrency,
   invalidation, sanitizer, and reclamation stress.
5. Deliver the JIT-to-JIT convention, unwind-aware tail transfers, and
   generation-stable direct-call targets.
6. Deliver the opaque C17 embedding API and portable IR package, then the
   validated native AOT object and deterministic rebuild gate.
7. Consider additional architectures only after all preceding contracts pass
   semantic, security, resource, and performance release gates on the required
   three architectures.

No item is considered delivered by instruction encoding alone. Completion
requires public contract documentation, verifier enforcement, interpreter
oracle coverage, optimized and baseline native parity, resource limits,
negative tests, fuzzing, real-host execution, and retained machine-readable CI
evidence.
