# Transactional object materialization

UniJIT can convert a reconstructed primitive deoptimization frame into a
frontend-owned object graph without embedding Lua, QuickJS, or PocketPy object
layouts in generated code. The public contract is target independent and runs
after the native frame has been restored through the ordinary ABI boundary.

## Plans and inputs

A `MaterializationPlan` is bound to one stable deoptimization site and resume
offset. Each `ObjectRecipe` has a unique recipe id, a unique logical-frame
destination slot, an opaque frontend-defined kind, and an ordered field list.
A field can read:

- a typed Word or Float64 slot from the reconstructed frame;
- an immutable typed value-bits constant; or
- the handle of any object recipe in the same plan.

Object references may point forward or backward, including to the object being
defined. Final validation rejects unknown references, duplicate ids,
destination collisions, malformed primitive types, more than 4,096 objects,
or more than 262,144 total fields. Execution also rejects a plan whose site or
resume offset does not exactly match the reconstructed frame, a destination
that overwrites a primitive slot, or a recovered field with the wrong type.

## Two-phase transaction

The frontend supplies a complete set of `noexcept` callbacks plus opaque
state. Materialization follows one deterministic transaction:

1. Validate the complete plan, frame, callbacks, and all recovered inputs
   before entering the runtime transaction.
2. Call `begin` with the exact object count.
3. Call `allocate` for every recipe before storing any field, producing opaque
   64-bit handles. This shell-allocation phase makes cyclic graphs possible.
4. Populate fields in recipe and field order through `store_primitive` or
   `store_object`.
5. Add every object handle to its logical destination slot and call `commit`.

Any failure reported from `begin`, allocation, field population, or commit
calls `rollback` exactly once and returns no partially materialized frame.
UniJIT completes its own potentially allocating validation and staging before
`begin`; callback code owns all runtime allocation and rooting performed while
the transaction is active. A rollback callback must therefore be safe after a
failed begin attempt and must discard every shell created by the transaction.

## Result and ownership

`MaterializedFrame` retains the deoptimization reason, site, resume offset,
and every primitive slot, then adds object-valued destination slots with an
explicit `MaterializedValueKind::kObject`. Object handles are opaque to UniJIT;
their lifetime and garbage-collector rooting remain the frontend's
responsibility after commit.

`CompiledFunction::materialize_deoptimization` reconstructs primitive state
and runs the plan in one checked operation. `CodeHandle` exposes the same API,
so materialization remains tied to the exact attempted native generation even
after cache replacement, eviction, clearing, or destruction.

## Current boundary

This contract provides bounded, cyclic, transactional object-graph recovery
without exposing target registers or runtime object layouts. Frontend-specific
recipes and callbacks still need to allocate concrete Lua tables/closures,
QuickJS objects/environments, and PocketPy objects, then install the completed
logical frame into the corresponding stock interpreter before resumable
execution is claimed.
