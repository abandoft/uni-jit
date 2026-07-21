# Strict portable 128-bit SIMD

## Delivery status

UniJIT now has the semantic core for explicit, fixed-width 128-bit SIMD in
both straight-line SSA and CFG SSA. The public types, verifier, two reference
interpreters, optimizer folding, deterministic differential generator, table
limits, and fail-closed compilation boundary are implemented independently of
SLJIT or another JIT backend.

This is not yet a native-SIMD delivery claim. Vector register allocation,
16-byte spills, call preservation, bounded vector memory, AArch64 NEON,
x86-64 SSE2, RISC-V RVV or verified scalar fallback, capability telemetry,
and real-host performance gates remain P0 work. Compilation returns
`StatusCode::kCodeGenerationFailed` if a vector node survives optimization.
An optimized program whose vector work folds completely to scalar SSA may use
the existing scalar native backend; baseline compilation cannot bypass the
vector preflight.

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

## Interpretation and optimization

Both reference interpreters maintain scalar and 128-bit value stores. CFG
edge transfer snapshots every source before assigning target block parameters,
so duplicated and cyclic logical vector edges retain parallel-copy semantics.

The optimizer treats every vector operation as pure, retains all three select
operands in liveness, remaps side tables, and folds complete constant vector
expressions bit-for-bit. It can collapse a constant vector program to one
scalar constant. Dynamic vector expressions remain explicit; they are never
silently discarded or reinterpreted by allocation. Native compilation still
rejects them before an incomplete encoder can publish code.

## Qualification and remaining gates

Core negative tests cover vector parameters/returns, malformed masks and
shuffles, invalid comparison domains, dynamic mask construction, independent
table limits, and native fail-closed behavior. The deterministic differential
corpus generates all six data shapes and compares straight-line SSA, CFG SSA
with a whole-vector edge, both optimized forms, and constant folding across
random arithmetic, mask logic, selection, lane movement, shuffling, sign-mask
extraction, and widening.

The typed allocation foundation is delivered. Straight-line and CFG allocation
use independent Word and physical SIMD banks, allow Float64 and vectors to
share the latter without overlap, reserve aligned two-word spill and
caller-clobber backup slots, detect mixed Float64/vector CFG register cycles by
physical bank, and preserve a full 128-bit cycle source when required. A
stack-only vector mode is available to the RISC-V backend until RVV is selected.
Non-reference vector lanes are deliberately excluded from the scalar
`ExecutionContext` capture payload; vector deoptimization remains unsupported.

The P0 feature remains incomplete until all of the following are delivered:

1. bounded aligned and unaligned vector loads/stores using the existing memory
   provenance and diagnosed-exit model;
2. backend integration of the delivered allocation plans for 16-byte spills,
   CFG parallel copies, helper-call preservation, and any ABI-specific
   nonvolatile saves;
3. independent NEON and SSE2 lowering plus RVV lowering or an explicitly
   reported verified scalar fallback;
4. target-profile-scoped `native`/`legalized`/`scalarized`/`unsupported`
   preflight and compilation telemetry;
5. interpreter/native differential, sanitizer, spill, call, and complete-loop
   performance evidence on real AArch64, Ubuntu and Windows x86-64, and
   RISC-V 64 hosts.
