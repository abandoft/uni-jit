# Canonical stack maps

Every compiled runtime guard and safepoint owns one immutable stack-map record.
The record binds its unique semantic site to a native instruction offset, the
active frame size, the exit kind, and every typed SSA value live immediately
before the effect executes.

## Canonical frame locations

Backends publish only stable frame locations. Before a guard or safepoint can
exit, each live register value is copied to a canonical eight-byte slot while
values already resident in the frame remain available there. This removes
physical register numbering and target-specific register classes from the
public contract.

`StackMapValue::frame_offset` is measured from the native stack pointer after
the generated prologue has reserved `StackMapRecord::frame_size` bytes. The
location is valid at `native_offset` while the generated frame is active. Word
and Float64 entries both occupy one value-bits slot; the retained `ValueType`
controls interpretation.

Straight-line lowering reserves a separate canonical area when the function
contains a guard or safepoint. CFG lowering reuses its value-ID-indexed frame
area. A backward liveness pass excludes dead definitions, preserves operands
needed after the effect, and translates live successor block parameters back
to the matching predecessor edge arguments. Loop-carried state is resolved by
a fixed-point dataflow calculation. Each site is limited to 64 live values so
its complete state fits the execution context's fixed capture area, and a
function is limited to 262,144 total live-value entries. These limits bound
metadata and exit work for adversarial graphs.

## Metadata ownership

`CompiledFunction::stack_maps()` and `stack_map(site)` expose the immutable
table. `CodeHandle` exposes the same API, so records remain queryable across
cache replacement, invalidation, eviction, clearing, and destruction for as
long as an execution lease survives. Runtime exit sites are unique within one
function, making site lookup deterministic. Optimization removes the stack map
when it proves the associated guard cannot exit.

Stack-map value IDs name the verified IR actually lowered for that compiled
generation. Frontends must not assume that optimized value IDs match their
input graph or source-level slots.

`CompilationStats::stack_map_count` and `stack_map_value_count` report the
published table and total retained values for observability and capacity
planning.

## Diagnosed-exit capture

`ExecutionContext` contains a fixed 64-word capture area. A backend copies the
canonical live values into that area only after a guard fails or an interrupt
poll fires, records the exact count, and then restores the generated frame.
The exit path performs no heap allocation and preserves Float64 values as exact
bits.

`CompiledFunction::reconstruct_stack_map(context)` validates the exit site,
guard-versus-safepoint kind, and complete captured count before returning typed
SSA values. `CodeHandle` exposes the same operation so callers reconstruct
against the exact attempted generation even if another thread replaces active
code. A successful later invocation clears the previous capture count together
with its exit diagnostics. Only the first `captured_value_count()` entries are
valid; unused capacity is intentionally left uninitialized so constructing a
context does not zero 512 bytes on every hot invocation.

## Current boundary

Canonical maps and diagnosed-exit capture preserve arbitrary live Word and
Float64 SSA state, within the per-site bound, across normal ABI return.
`RecoveryOperation::captured_value` consumes that state as a logical-frame
input: compilation preserves and remaps the requested definition, verifies
straight-line availability or CFG dominance, forces it through allocation and
liveness, and resolves it to the exact captured index. Materialized object
recovery, concrete interpreter-frame installation, unwind registration,
resumable transfer into a stock runtime, and on-stack replacement build on this
primitive frame state but are not claimed by the current interface.
