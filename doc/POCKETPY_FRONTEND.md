# PocketPy frontend

The PocketPy integration builds the pinned 2.1.8 runtime in isolation and
registers an opt-in `unijit` module without modifying PocketPy sources. Native
code is produced by UniJIT's typed SSA, optimizer, register allocator, and
machine-code encoders. PocketPy and SLJIT are not code-generation backends.

## Embedding API

Configure with `-DUNIJIT_BUILD_POCKETPY_REFERENCE=ON`, link
`unijit_pocketpy_frontend`, then install the module in every PocketPy VM that
will use it:

```cpp
#include <pocketpy.h>

#include "unijit_pocketpy.h"

py_initialize();
if (unijit_pocketpy_install() != 0) {
  // Abort initialization: another incompatible module owns this name.
}
```

PocketPy code can then compile a deliberately explicit source string:

```python
import unijit

native = unijit.compile(
    "def affine(a, b): return (a + 2.5) * (b - -3)"
)
result = native(1.5, 4)
completed = unijit.wait(native, 5000)
metrics = unijit.stats(native)
cancelled = unijit.cancel(native)
```

The source-string API avoids pretending that PocketPy 2.1.8 exposes reliable
source provenance for arbitrary function objects. Each call requires exactly
the declared number of arguments. PocketPy `int` and `float` arguments are
converted to Float64, other types raise `TypeError`, and the result is a
PocketPy `float`.

Straight-line callables initially publish verified SSA through the explicit
low-latency baseline compiler, without running the optimization pipeline. Each
callable records saturating invocation and measured-backedge hotness; after 64
successful calls or 10,000 counted-loop safepoint polls, one atomic compilation
claimant submits the retained exact source to a one-worker
background scheduler bounded to 64 queued tasks and 8 MiB of estimated queued
input. The worker never accesses the PocketPy VM: it translates immutable
source, checks cooperative cancellation, reuses the optimized cache, and
publishes only if the captured baseline generation is still current. Baseline
and optimized mappings use independent exact-source caches, so a second
callable can reuse either tier without conflating their lifetime or profiling
state. Failed compilation is not allowed to break an already successful
invocation and is delayed before retry.

`unijit.stats(native)` returns the active tier, whether the source supports
tiering, generation, invocation, measured-backedge and compilation counters, promotion and
withdrawal counts, OSR attempts/entries/exits, compilation task state,
cancellation state, scheduler queue and worker use, code size, and input/active
IR node counts. `unijit.wait`
performs a timeout-bounded wait without polling the VM, and `unijit.cancel`
requests queued or running work cancellation. These APIs report optimization
lifecycle only; baseline execution remains available after rejection, timeout,
or failed optimization.

The compiled callable stores shared compilation state in PocketPy userdata. The
VM garbage collector requests cancellation through the type destructor, while
an already running worker retains the immutable source and native publication
state until terminal completion. The internal callable type is final and
rejects direct construction, so an object with uninitialized native ownership
cannot be created from script.

## Specialization contract

The first tier accepts one conventional `def` with zero to 64 unique ASCII
parameter names and exactly one `return` expression. The expression may contain
parameters, decimal numeric literals, parentheses, unary `+`/`-`, and binary
`+`, `-`, `*`, and `/`, plus one top-level ordered `<`, `<=`, `>`, or `>=`
comparison or numeric `==`/`!=` comparison. Arithmetic lowers to Float64 SSA;
comparisons return an actual PocketPy `bool` from both baseline and optimized
native tiers, with NaN remaining unequal and both signed zeroes comparing
equal.
Unary `-` lowers to the core sign-bit negation operation in both straight-line
and counted-loop translation. The native result therefore reverses signed zero
and infinity while retaining the exact NaN payload instead of evaluating
positive-zero subtraction.

Closures, annotations, default or variadic parameters, assignments, multiple
statements, calls, attribute access, chained comparisons, and non-ASCII
identifiers are rejected with a source byte position. Python keywords are
rejected as function or parameter names. Every division emits an inline
Float64 nonzero guard at the `/` source site; both `+0.0` and `-0.0` exit
through the execution context and become
PocketPy's `ZeroDivisionError("float division by zero")`. The compiled record
retains the source resume offset and reconstructs every entry parameter, the
exact guarded divisor bits, and the partial left-hand expression value that
represents the primitive operand stack immediately before division. The
adapter maps the exit only after validating that record and reconstructed
trigger, so future runtime exits cannot be silently misclassified as division
errors. This narrow contract prevents fallback-free native code from silently
changing unsupported Python behavior.

Source larger than 1 MiB is rejected before it is retained or translated, and
accepted IR remains subject to the core compilation budgets.

The counted-loop tier accepts four-space-indented numeric functions with
Float64 local initializers, one `for name in range(...)` loop, arithmetic or
augmented assignments, and ordered or equality `if`/`else` arms that update
loop locals.
`range(stop)` and `range(start, stop)` use a positive unit step;
`range(start, stop, step)` accepts a finite nonzero numeric-literal step and
selects the matching forward or reverse strict bound. Dynamic and zero steps
are rejected before CFG construction.
Mutable state is carried through typed CFG block parameters, branches merge via
SSA edges, and every backedge polls an execution-context safepoint. `/` and
`/=` emit effectful CFG Float64 nonzero guards at their exact source positions;
entered iterations reconstruct the original parameters, signed-zero divisor,
and every current loop local including the induction variable before raising
`ZeroDivisionError`, while a zero-iteration loop does not execute or reject its
dormant division. A single-statement true arm may use `if condition:` followed
by `break` or `continue` without an `else`: `break` carries the exact current
locals to loop exit, while `continue` passes them through the common range
update so the induction variable advances normally. Unconditional or
multi-statement control transfers, control guards with an `else`, calls, and
nested loops remain rejected.

Counted loops enter the low-latency CFG baseline first and report
`tierable: true`. At the 64-call or 10,000 measured-backedge threshold, the background worker compiles
an optimized CFG with constant folding, Word canonicalization, and dead-code
elimination while retaining every call, guard, safepoint, and frame-state root.
Checked counted-loop division therefore promotes through the normal generation
and cache lifecycle; both signed zeroes still reconstruct through the exact
attempted optimized lease before becoming `ZeroDivisionError`.

## Reproducible CPython JIT target benchmark

PocketPy, UniJIT, and CPython execute the same
`benchmark/pocketpy/numeric_call.py` workload. PocketPy and both CPython modes
run the source loop, while UniJIT compiles that complete loop into CFG native
code; the measured region has one callable boundary per sample rather than one
boundary per iteration. The CPython driver
requires exactly 3.14.6, verifies that `sys._jit` is available, and rejects a
process whose actual mode disagrees with `PYTHON_JIT=0` or `PYTHON_JIT=1`:

```sh
cmake -S . -B build/pocketpy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/pocketpy --target unijit_pocketpy_benchmark
python3 benchmark/pocketpy/run_cpython.py \
  --unijit build/pocketpy/bin/unijit_pocketpy_benchmark \
  --python /path/to/jit-capable/python3.14 \
  --warmup 10000 --iterations 100000 --samples 7 \
  > build/pocketpy/pocketpy-targets.json
```

The hosted baseline downloads the official Python.org macOS 3.14.6 package,
checks its published SHA-256 before use, runs interpreter and JIT modes, and
retains the complete comparison record. The current workload is a target
baseline, not a whole-language parity claim.
