# Strict portable 128-bit SIMD

## Delivery status

UniJIT now has the semantic core for explicit, fixed-width 128-bit SIMD in
both straight-line SSA and CFG SSA, native AArch64 Advanced SIMD/NEON and
x86-64 SSE2 lowering, and bounded RV64IMD scalar lowering for the complete
current explicit operation surface. The public types, verifier, two reference
interpreters, optimizer folding, deterministic differential generator, table
limits, allocation, legalizers, and encoders are implemented independently of
SLJIT or another JIT backend.

This is a three-backend execution, bounded-vector-memory, complete-loop
performance, and target-scoped capability-reporting claim for the current
explicit operation surface. RISC-V 64 does not require or claim RVV: it keeps
each vector in an aligned two-word stack slot and emits a finite RV64IMD scalar
sequence. An optional profile-specific RVV fast path remains follow-on work.

## Representation and lane order

`Vector128` stores 16 logical bytes. Lane zero always occupies the
lowest-addressed logical bytes, independent of host endianness. Lane bits are
packed little-endian within those logical bytes, so a verifier, interpreter,
serializer, and backend agree without copying a host C vector object or
depending on its layout.

The data types are:

| Type | Lane shape |
|---|---:|
| `I8x16` | 16 × 8-bit integer |
| `I16x8` | 8 × 16-bit integer |
| `I32x4` | 4 × 32-bit integer |
| `I64x2` | 2 × 64-bit integer |
| `F32x4` | 4 × IEEE binary32 |
| `F64x2` | 2 × IEEE binary64 |

Masks are distinct `Mask8x16`, `Mask16x8`, `Mask32x4`, and `Mask64x2`
types. Every mask lane is canonical: all bits zero or all bits one. A mask is
not a scalar Boolean, and data vectors do not implicitly convert to masks.
The scalar invocation ABI remains `Word`/`Float64`; vector parameters and
returns are rejected until the external ABI, stack maps, and register calling
convention are complete. CFG block parameters may carry a whole vector, which
gives the interpreters and optimizer an exact edge-copy contract now.

## Operation contract

The initial operation surface is deliberately explicit:

- constants and zero for every vector type;
- data-vector splat from the low lane bits of a `Word`;
- lane extraction to raw `Word` bits, with optional sign extension only for
  integer data vectors;
- lane insertion from raw `Word` bits into integer or floating data vectors;
- bitwise NOT, AND, OR, and XOR over all vector and mask bits;
- wrapping integer add, subtract, and multiply independently per lane;
- lane-wise Float32/Float64 add, subtract, multiply, and divide;
- signed and unsigned integer comparisons and ordered floating comparisons;
- canonical-mask selection between two same-shaped data or mask vectors;
- lane-sign extraction into the low bits of one `Word`;
- an immutable same-vector shuffle with one verified source index per lane;
- low- or high-half signed/unsigned integer widening from 8 to 16, 16 to 32,
  or 32 to 64 bits.

Integer arithmetic is modulo the lane width. Floating insertion, extraction,
constants, bitwise operations, selection, and shuffling preserve exact bits,
including signed zero and NaN payloads. Arithmetic uses one strict scalar
operation per lane: it is not reassociated, contracted to FMA, flushed to zero,
or replaced by reciprocal approximation. Ordered comparisons return false if
either operand is NaN and always produce a canonical mask. Exact NaN payload
selection after arithmetic is not a cross-architecture promise; each native
tier must bit-match its target's reference interpreter for qualified inputs,
while NaN-sensitive operations such as minimum/maximum remain absent until
their result-bit contract is specified.

The public `noexcept` semantic helpers are total for malformed direct calls:
an unknown type or operation, out-of-range lane, invalid shuffle, invalid
widening shape, or noncanonical selection mask produces a zero scalar/vector.
This safety behavior is not an IR legalization path. The verifier rejects the
same malformed state with a diagnostic before either interpreter or optimizer
executes it.

## Verification and storage

Vector constants, shuffle descriptors, and the third operand of selection use
separate bounded side tables. Every table entry must be referenced exactly
once by the corresponding node; negative, duplicated, dangling, or
unreferenced entries are invalid IR. The verifier additionally proves:

- operand availability and CFG dominance, including the selection false arm;
- exact data, result-mask, lane, shuffle, and widening shapes;
- canonical mask constants and the absence of dynamic mask splats/inserts;
- separation of integer and ordered floating comparison modes;
- scalar-only runtime helpers, frame locals, trusted-object fields,
  deoptimization recovery, object materialization, OSR, parameters, and
  returns.

`CompilationLimits` independently bounds vector constants, shuffles, and
selection operands to 65,536 entries each by default. These checks run before
expensive optimization or native allocation.

## Bounded vector memory

Straight-line and CFG IR expose explicit 128-bit vector loads and stores for
the six data-vector types. Mask-vector memory is deliberately rejected:
untrusted bytes are not allowed to introduce noncanonical mask lanes. Every
descriptor must use `MemoryWidth::k128`, disable scalar sign extension, name a
declared execution-context region, and declare a power-of-two alignment from 1
through 16 bytes.

Lane zero remains at the lowest address. Native and explicit little-endian
transfers preserve the in-memory byte order used by the logical vector
representation on the supported little-endian product targets. Explicit
big-endian order reverses bytes independently inside each typed lane; it never
reverses lane order or the complete 128-bit payload. This rule applies equally
to integer and raw IEEE floating lane bits.

The complete 16-byte bounds, write permission, and absolute address alignment
are checked before a store changes any byte. A failed transfer records its
declared site and original unsigned byte offset through the existing diagnosed
exit path. Volatile descriptors retain byte-observable interpreter accesses,
stores return their original vector value, and the optimizer preserves load
and store ordering as memory effects. Vector payloads remain intentionally
absent from scalar exit capture even though each vector memory site owns a
stack map for the scalar state that is recoverable there.

AArch64 uses full-width `LDR`/`STR Q` transfers and per-lane byte reversal for
explicit big endian. x86-64 uses unaligned-safe SSE2 `MOVDQU` transfers and a
bounded aligned stack/GPR legalization for big-endian lanes. RISC-V 64 retains
its stack-only vector representation and emits a finite per-lane RV64IMD byte
sequence when byte order or alignment prevents a naturally aligned scalar
lane access. None of these paths uses a runtime helper, heap allocation,
unchecked partial store, or RVV state.

## Interpretation and optimization

Both reference interpreters maintain scalar and 128-bit value stores. CFG
edge transfer snapshots every source before assigning target block parameters,
so duplicated and cyclic logical vector edges retain parallel-copy semantics.

The optimizer treats every vector operation as pure, retains all three select
operands in liveness, remaps side tables, and folds complete constant vector
expressions bit-for-bit. It can collapse a constant vector program to one
scalar constant. Dynamic vector expressions remain explicit; they are never
silently discarded or reinterpreted by allocation. All three product backends
compile the current surface, while an incomplete future operation or target
must still be rejected before its encoder can publish code.

## Qualification and remaining gates

Core negative tests cover vector parameters/returns, malformed masks and
shuffles, invalid comparison domains, dynamic mask construction, independent
table limits, and native rejection boundaries. The deterministic differential
corpus generates all six data shapes and compares straight-line SSA, CFG SSA
with a whole-vector edge, both optimized forms, and constant folding across
random arithmetic, mask logic, selection, lane movement, shuffling, sign-mask
extraction, and widening.

The typed allocation foundation is delivered. Straight-line and CFG allocation
use independent Word and physical SIMD banks, allow Float64 and vectors to
share the latter without overlap, reserve aligned two-word spill and
caller-clobber backup slots, detect mixed Float64/vector CFG register cycles by
physical bank, and preserve a full 128-bit cycle source when required. The
AArch64 and x86-64 backends consume those plans for full-width loads, stores,
mixed edge copies, and helper-call saves. x86-64 additionally aligns the
generated frame itself before addressing even-numbered two-word vector slots
and adjusts System V or Windows shadow-space calls from that aligned base. A
stack-only vector mode is consumed by the RISC-V backend without depending on
RVV. Non-reference vector lanes are deliberately excluded from the
scalar `ExecutionContext` capture payload; vector deoptimization remains
unsupported.

The AArch64 encoder covers integer and floating arithmetic, bitwise logic,
canonical integer and ordered floating comparisons, mask selection, splats,
lane insert/extract, sign masks, constant shuffles, and signed/unsigned
widening. I64x2 multiplication is deliberately scalar-legalized per lane
because the architectural 128-bit NEON floor has no matching two-lane multiply
instruction. No operation silently selects relaxed floating-point semantics.
Lane-sign-mask extraction is also reported as a bounded legalization because
the AArch64 baseline expands it into explicit lane extraction and scalar mask
assembly.

The x86-64 encoder uses the mandatory SSE2 baseline directly for packed
integer add/subtract and I16x8 multiplication, packed Float32/Float64
arithmetic and ordered comparisons, bitwise logic and selection, most sign
masks, 32/64-bit constant shuffles, and signed/unsigned widening. Integer
comparisons, I8x16/I32x4/I64x2 multiplication, lane insertion, I8x16/I16x8
shuffles, and the remaining sign-mask shape use bounded aligned stack
temporaries and scalar GPR sequences because SSE2 has no complete direct
instruction surface. These are native legalizations with no runtime helper,
heap allocation, unbounded loop, or relaxed arithmetic.

The RISC-V 64 encoder consumes the stack-only allocation plan and scalarizes
each lane to bounded RV64IMD integer or floating-point sequences. Constants,
spills, helper crossings, and CFG edges copy both 64-bit halves; shuffles and
widening use a separate aligned temporary so source lanes cannot be destroyed.
Integer and ordered floating comparisons materialize canonical all-zero or
all-one masks. This path has no runtime helper, heap allocation, unbounded
loop, RVV state requirement, or relaxed arithmetic. Discovered RVV capability
is reserved for a later target-profile-specific fast path.

Native qualification executes the current operation surface in both IR forms
on a real Apple AArch64 host, including deliberate `v0` clobbering by a runtime
helper, 24-way register pressure, aligned vector spills, vector CFG block
parameters, fallback edge temporaries, and mixed Float64/vector cycles. Both
baseline and optimized native tiers are checked against the appropriate
reference interpreter by the committed 128-program deterministic corpus and
two extended 512-program runs; the same corpus passes under ASan/UBSan. The
identical operation, clobber, spill, edge, mixed-cycle, and differential suite
also passes in Rosetta x86-64, real Ubuntu GCC/Clang x86-64, and Windows MSVC
x86-64 processes. Linux and Rosetta ASan/UBSan plus the Linux ThreadSanitizer
suite pass with x86-64 vector compilation enabled; the real Ubuntu
qualification executes the committed corpus and both extended 512-program
seeds natively. A real Bianbu RISC-V 64 host executes the same current surface
through the bounded scalar path, including baseline/optimized parity, stack
pressure, helper crossings, typed CFG edges, the committed corpus, and both
extended 512-program seeds.

The `unijit_cfg_simd_benchmark` qualification boundary runs a fixed strict
I32x4 add/XOR/shuffle recurrence entirely inside generated CFG loops. It
bit-matches the vector native result with both an equivalent four-lane scalar
native CFG and the vector interpreter, then reports compilation latency, code
size, spill slots, median nanoseconds per source-loop iteration, speedup ratios,
and a deterministic checksum. The machine-readable gate requires seven
samples of 1,000-iteration loops across 500 measured invocations, at least
1.10x speedup over equivalent scalar generated code, at least 10x over the
interpreter, and at most 1,024 bytes of vector native code. Hosted AArch64,
Ubuntu GCC/Clang and Windows MSVC x86-64, hosted macOS x86-64, and real Bianbu
RISC-V 64 all pass; per-host JSON records are retained by the platform
workflow, and RISC-V explicitly reports `scalarized` rather than `native`.

The remaining profile-specific SIMD work is optional RVV lowering selected
only by an explicit compatible target profile and proven against the same
scalar oracle and real-host matrix.

`preflight_capabilities()` now validates either IR form and an immutable target
profile, then classifies the complete typed operation set without allocating
executable memory or emitting code. Reports include `native`, `legalized`,
`scalarized`, `helper`, and `unsupported` counts, required feature bits, the
fixed vector width, execution-context need, and per-vector-class resource
masks for Word, floating, and vector registers plus aligned vector stack
storage. Compilation runs the same classifier after optimization, so published
telemetry describes operations that actually reach lowering; the immutable
report is retained by `CompiledFunction` and every `CodeHandle` lease.
