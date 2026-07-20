# On-stack replacement entry

UniJIT provides a target-independent transfer contract for moving a running
interpreter region into a compiled native region without restarting the source
function. The contract separates frontend frame layouts from native parameter
layouts and keeps the selected compiled generation alive for the complete
transfer and any subsequent deoptimization.

## Interpreter frame and entry plan

An `OsrFrame` is bound to one stable frontend entry site and resume offset. It
contains at most 64 unique logical slots, each carrying exact Word or Float64
value bits. Frontends construct the frame before entering native code, so the
transfer path never reads target registers or runtime-private frame memory.

An `OsrEntryPlan` is bound to the same site and offset. Its ordered bindings map
logical frame slots to native parameters. Plan construction rejects duplicate
slots, unsupported types, and more than 64 arguments. Entry additionally
requires:

- an exact site and resume-offset match between the frame and plan;
- one binding for every compiled parameter, in native parameter order;
- exact Word or Float64 agreement with the compiled signature; and
- every bound logical slot to exist in the interpreter frame with that type.

Extra interpreter slots are allowed because a frontend frame may contain state
that is not live in the selected native region.

## Transfer and exits

`CompiledFunction::enter_osr` validates and marshals the frame into a fixed
64-word argument area, then uses the ordinary managed invocation boundary.
Successful marshalling performs no heap allocation. Execution therefore keeps
the same context clearing, safepoint, assumption, diagnosed-exit, and ABI return
rules as a normal compiled invocation.

`CodeHandle::enter_osr` performs the same transfer through an immutable code
lease. Cache replacement, invalidation, eviction, clearing, or destruction
cannot redirect or reclaim the selected generation while the lease remains
alive.

`OsrEntryResult` retains the exact marshalled arguments even when native code
exits. A frontend passes those arguments, the same execution context, and the
exit site back to `reconstruct_deoptimization` on the same compiled function or
code lease. This preserves exact argument bits and prevents reconstruction from
accidentally using a newer generation.

## Current boundary

The current entry vocabulary covers primitive Word and Float64 state and
transfers into a separately compiled native region through its published entry
point. Lua, QuickJS, and PocketPy adapters still need to trigger this contract
from their stock interpreter backedges, define language-specific live-slot
plans, and connect normal or deoptimized results to their concrete frames.
Object-valued state can be rebuilt after a diagnosed exit through the atomic
materialization contract in [MATERIALIZATION.md](MATERIALIZATION.md). Direct
entry at arbitrary internal machine-code offsets and runtime-specific unwind
registration remain later extensions.
