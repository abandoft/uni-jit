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
a fixed-point dataflow calculation. Compilation rejects a function whose maps
would retain more than 262,144 total live-value entries, bounding metadata
growth from adversarial graphs.

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

## Current boundary

Canonical maps describe arbitrary live Word and Float64 SSA state while the
native frame is active. Existing managed guards still restore that frame and
return through the normal ABI boundary before frontend reconstruction. Captured
live-value transport, materialized objects, interpreter-frame installation,
unwind registration, and on-stack replacement build on this metadata but are
not claimed by the current interface.
