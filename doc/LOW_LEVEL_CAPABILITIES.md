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
| SIMD | No vector IR or vector allocation | Add strict portable SIMD, then guarded wider target profiles and loop vectorization | P0 |
| Typed memory, unaligned access, byte reversal | Only compiler-owned frame and argument accesses exist | Add explicit bounded memory IR before SIMD, atomics, or FFI lowering | P0 |
| Generated-code atomics | Runtime control structures use C++ atomics; generated IR has none | Add typed atomic IR with explicit memory order and natural-alignment rules | P1 |
| Fast internal calls | Calls currently use the portable runtime-helper ABI | Add a private JIT-to-JIT convention without weakening external ABI safety | P1 |
| Tail calls | Not represented | Add verified tail transfers after fast calls and unwind metadata exist | P2 |
| Self-modifying code | Published code is immutable RX | Keep code immutable; use atomic RW non-executable patch cells and versioned replacement | P1 |
| Direct physical-register access | Intentionally absent from public IR | Keep physical registers private to MIR; expose only verified role constraints | Architectural rule |
| Function-local stack storage | Only compiler-managed spills, call areas, and metadata slots exist | Add bounded typed frame slots with alignment and lifetime verification | P1 |
| All-in-one hidden compilation | Native encoders are already internal | Add an opaque versioned C17 embedding ABI over the C++ implementation | P1 |
| Serialization and AOT | No persistent compilation artifact | Add canonical portable IR packages first, then validated target code objects | P1 |

P0 closes a prerequisite for several language and numerical workloads. P1 is
required before claiming a broadly embeddable commercial backend. P2 improves
call-heavy workloads after the metadata and lifecycle foundations are proven.

## Target and feature profiles

Native compilation receives an immutable target profile containing the
architecture, operating-system ABI, endianness, scalar ISA baseline, optional
instruction features, and vector-width policy. The profile is part of the code
cache identity and serialized-artifact compatibility key. A compiled function
may only execute on a host whose discovered feature set contains its required
set.

Host discovery uses CPUID plus XGETBV on x86-64, operating-system capability
interfaces on AArch64, and the Linux RISC-V hardware-probe or auxiliary-vector
interfaces where available. Cross compilation requires an explicit profile and
must not infer target features from the build host. Unknown features select the
portable baseline rather than optimistic emission.

The current minimum profiles remain AArch64 with FP64, x86-64 with SSE2, and
RV64IMD. Optional profiles are monotonic: code compiled for a wider profile is
stored and invalidated independently from baseline code.

## Typed memory contract

Memory IR is a prerequisite for vectors, atomics, inline caches, and efficient
runtime object access. Initial operations cover signed and unsigned 8-, 16-,
32-, and 64-bit integer loads, Float32/Float64 loads, matching stores, and
explicit address calculation. Every access records:

- an address space and trusted or bounds-checked provenance;
- byte width and required alignment;
- native, little-endian, or big-endian interpretation;
- volatility and alias class;
- the frontend exit site used when a dynamic bounds or alignment guard fails.

Address addition is checked as an unsigned operation before dereference.
Untrusted public IR cannot manufacture an arbitrary process pointer. Frontends
bind a base pointer to a bounded region or to a runtime-owned object layout,
and guards dominate the access. The verifier rejects widths that cross the
proven region and rejects an unbounded dynamic offset.

Unaligned scalar loads and stores have byte-exact semantics and never rely on C
or C++ undefined behavior. A backend may use one native unaligned instruction
when its target contract permits it; otherwise it emits aligned pieces and
combines them. Unaligned atomics are never synthesized and are rejected.

The scalar IR adds `byte_swap16`, `byte_swap32`, and `byte_swap64`. Explicit
endian loads and stores lower through those operations when the host order
differs. UniJIT's generated-code format remains little-endian on the current
targets, while the portable serialized format uses a canonical byte order.

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
- a bounded compile-time shuffle with verifier-checked lane indices.

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

### Vectorization stages

1. Land explicit vector IR, verifier, interpreter, folding, allocation, and
   three-backend lowering.
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
Invalid load/store order combinations are verifier errors.

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

Typed frame slots provide fixed compile-time size, power-of-two alignment,
value kind, and lexical lifetime. Dynamic or unbounded `alloca` is not part of
the public contract. Frame layout accounts for ABI alignment, Windows shadow
space, stack probes for large frames, spill/call/edge temporary areas, and
zeroization of slots explicitly marked sensitive.

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
codes plus bounded diagnostics.

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

1. Specify and implement bounded typed memory, byte reversal, unaligned scalar
   access, frame slots, target profiles, and negative verifier tests.
2. Deliver strict 128-bit SIMD with interpreter parity, spills, calls, CFG edge
   copies, feature discovery, and scalar fallback where required.
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
