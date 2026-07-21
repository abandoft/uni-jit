# Controlled frame locals

## Delivered contract

UniJIT exposes fixed frame slots instead of arbitrary stack addresses. A
`FunctionBuilder` or `ControlFlowBuilder` declares each slot before use with a
`ValueType` and an optional `sensitive` flag. The delivered slot types are
`Word` and `Float64`; each occupies eight bytes and lives for the complete
native invocation.

Every slot is initialized to all-zero bits on every interpreter or native
entry. A load produces a new SSA value with the declared type. A store requires
the same SSA type, writes the slot, and returns the original stored value. The
straight-line verifier requires ordinary definition-before-use, while the CFG
verifier also requires the stored value to dominate the store. Undeclared
slots, malformed metadata, and type mismatches are invalid IR.

Slots are effectful state. Optimization preserves loads and stores in program
order, retains descriptors even when a slot is only initialized, and never
common-subexpression-eliminates a load across another frame operation. The CFG
canonicalizer treats stores as roots, so state carried across an edge cannot be
removed as an apparently dead SSA result.

## Native layout and security boundary

Frame slots are placed in the generated stack frame after allocator spills and
CFG edge temporaries and before context, call-argument, and return-address
areas. Backends include them in checked frame-size arithmetic and preserve the
target ABI's stack alignment. IR cannot obtain a slot address, index beyond the
declared table, alias a spill, or retain a pointer after return.

Slots do not require an `ExecutionContext`; they are private invocation state.
They are not independently listed in stack maps because they are not logical
SSA values. A value loaded from a slot becomes ordinary typed SSA state and is
captured normally if it is live at a diagnosed exit.

A sensitive slot is overwritten with zero on every generated return path,
including guard, safepoint, and bounded-memory exits. Clearing occurs before
the frame is released. This flag promises erasure of the declared stack cell,
not every historical copy: a value may also have occupied a physical register,
an allocator spill, a call-preservation slot, or frontend-owned memory. Code
handling secrets that requires whole-value erasure must wait for a future
sensitive-value data-flow class with register/spill tainting and verified
epilogue coverage.

## Resource and qualification rules

`CompilationLimits::maximum_frame_slots` defaults to 256 and must be positive.
The compiler checks both IR forms before expensive optimization. A successful
`CompiledFunction` reports the declared count in
`CompilationStats::frame_slots`; allocator spill statistics remain separate.

Qualification covers zero initialization, repeated invocations, Word and
Float64 storage, stores returning their source value, cross-block state,
optimizer preservation, sensitivity metadata, additional sensitive epilogue
code, malformed IDs and types, and resource exhaustion. AArch64 and x86-64
execute the shared matrix natively during local qualification; all three
backend sources compile with warnings as errors, with real-host qualification
required before the delivery block is considered complete.

## Follow-on frame classes

The fixed eight-byte floor is sufficient for frontend temporaries and is a
prerequisite for trusted object access, SIMD, fast calls, and AOT frame layout.
Future extensions are separate verified descriptor classes:

1. 16-byte vector slots with explicit 16-byte alignment and vector type.
2. Bounded aggregate slots with compile-time size, power-of-two alignment, and
   no implicit typed aliasing.
3. Lexical lifetimes that permit layout reuse only after liveness proves no
   reference or deoptimization obligation survives.
4. Guard-page stack probes, platform unwind records, and configured byte
   budgets before frames may grow beyond the current backend limits.
5. Sensitive-value propagation that clears every register and spill copy and
   prevents transformations that would create an untracked secret lifetime.

Dynamic `alloca`, frontend-selected physical offsets, and arbitrary pointers
into a generated frame remain outside the public IR.
