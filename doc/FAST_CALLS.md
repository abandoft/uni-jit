# Generation-safe JIT internal calls

## Contract

UniJIT represents an internal call with a portable typed descriptor, not a host
code pointer. A descriptor fixes an ordered scalar parameter list and one scalar
result type. The delivered floor accepts `Word` and `Float64` in both
straight-line and CFG SSA. The verifier rejects undeclared slots, non-scalar
signatures, mismatched argument counts or types, unavailable CFG values, and
malformed flattened argument ranges.

Internal calls are effectful. The optimizer roots every argument, preserves a
call even when its result is dead, retains descriptor indices across rebuilding,
and clears local value numbering at the call boundary. The reference
interpreters require an explicitly bound oracle table in `ExecutionContext`.
Portable IR therefore contains neither a process-local function pointer nor a
serialized native address.

This first commercial slice dispatches to the ordinary compiled native-entry
ABI: a pointer to an ordered value-bits argument area plus the current execution
context, returning one value-bits word. It removes the runtime-helper callback
and argument-count path while retaining one safe ABI on AArch64, x86-64 System
V/Windows, and RISC-V 64. A later profile-specific convention may move a bounded
argument prefix into registers without changing the IR or binding contract.

## Binding and generation lifetime

Only a `CodeHandle` can bind a native target. `bind_fast_call` requires:

- a live caller and target handle;
- an in-range call slot;
- an exact parameter and result signature;
- an identical immutable target-profile key;
- a context-free target with a public native entry; and
- a target generation distinct from the caller itself.

Context-free currently means the target has no assumptions, trusted-object
bindings, patch cells, safepoints, diagnosed guards, bounded memory/atomics, or
nested fast calls. This fail-closed boundary prevents a callee from replacing
or ambiguously inheriting caller-owned runtime bindings. Contextual and nested
calls require an explicit compositional exit/stack-map contract and remain a
follow-on.

Each compiled caller owns an immutable target-table snapshot. Binding or
clearing copies the current table under a short writer mutex and release-publishes
one complete replacement. Managed invocation acquire-loads exactly one snapshot,
rejects the call before native entry if any declared target is unbound, installs
its entry array in the execution context, and restores the prior binding on
every return or diagnosed-exit path.

Every table entry holds both the native entry and shared ownership of the exact
compiled target generation. Cache replacement, invalidation, eviction, or
destruction can retire lookup visibility but cannot reclaim a generation still
reachable from an executing or published caller snapshot. Concurrent invocation
therefore observes either the complete old table or the complete new table;
it never combines an entry pointer with another generation's lifetime lease.
Clearing a slot removes it only from the newly published table, while readers
already holding the prior snapshot finish safely.

Functions containing fast-call descriptors expose no raw `native_entry()`.
Callers must enter through managed invocation so snapshot acquisition, unbound
checks, context installation, and lifetime retention cannot be bypassed.

## Native lowering

All three backends use the same sequence:

1. preserve caller-clobbered live Word and Float64 SSA values;
2. materialize ordered value bits into the bounded call-argument frame area;
3. pass that area's address as the first ABI argument and the current execution
   context as the second;
4. load the target entry from the invocation-fixed table and call indirectly;
5. move returned bits into the typed destination and restore live values.

AArch64 and RISC-V 64 preserve their link registers in the generated frame.
x86-64 applies the platform-specific stack alignment and Windows shadow-space
rule used for every native call. Call-argument sizing, call liveness, spill
slots, and context-frame storage are bounded before code publication.

The initial `maximum_fast_calls` compilation budget is 64 independently of the
flattened call-argument budget. A target table allocation failure is reported as
resource exhaustion; synchronization or publication failures return a
structured unavailable status rather than entering partially bound code.

## Qualification

Core tests cover Word and Float64 signatures, both IR forms, missing interpreter
oracles, optimization, unbound rejection, exact binding, generation retargeting,
cache invalidation with retained execution, safe clearing, self-target rejection,
raw-entry denial, metadata, and the independent descriptor limit. The installed
package consumer repeats construction, oracle execution, binding, invalidation,
retained execution, and clearing using only exported headers and CMake targets.

`unijit_fast_call_stress` races generated-code readers against thousands of
target-table publications. Every result must belong to one complete target
generation, the exact invocation count must complete, invalidated targets must
remain usable through the caller lease, and a cleared slot must fail closed.

`unijit_fast_call_benchmark` compares a generation-safe internal target with an
equivalent runtime helper inside a 1,000-iteration generated CFG loop. Each
sample crosses the complete managed caller boundary. The retained platform gate
requires seven samples, 100 warmups, 500 measured invocations, at most 1.5 times
the helper dispatch latency, at most 256 caller code bytes, and at most 64 target
code bytes. Emulator results are supplemental; commercial qualification uses
real AArch64, Ubuntu/Windows x86-64, and RISC-V 64 hosts.

## Deferred extensions

The delivered contract does not yet claim register-prefix argument passing,
contextual or recursive targets, polymorphic inline-cache policy, GC reference
arguments, direct relative call patching, tail transfer, exception unwinding
through nested JIT frames, or cross-process native target persistence. These
extensions must preserve typed verification, immutable RX code, generation
leases, canonical stack maps, target profiles, and retained real-host gates.
