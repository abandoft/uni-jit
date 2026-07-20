# QuickJS frontend

The QuickJS integration builds the pinned stock runtime in isolation and adds
an opt-in `unijit` object without modifying QuickJS sources. Code generation
uses UniJIT's own typed SSA, optimizer, register allocator, and native encoders;
QuickJS and SLJIT are not code-generation backends.

## Embedding API

Configure with `-DUNIJIT_BUILD_QUICKJS_REFERENCE=ON`, link
`unijit_quickjs_frontend`, and install the API before running untrusted script:

```cpp
JSRuntime* runtime = JS_NewRuntime();
JSContext* context = JS_NewContext(runtime);
if (unijit_quickjs_install(context) != 0) {
  // Read the pending QuickJS exception and abort initialization.
}
```

JavaScript can then create a specialized native closure:

```js
const native = unijit.compile(function(a, b) {
  return (a + 2.5) * (b - -3) / 2;
});
```

Every declared argument must be a JavaScript Number. Missing or non-Number
arguments throw a `TypeError`; extra arguments follow ordinary JavaScript
fixed-parameter behavior and are ignored. Results are returned as Numbers.

Accepted straight-line callables publish verified baseline code synchronously
and remain callable while a single bounded background worker compiles optimized
code after 64 invocations. The worker retains immutable source and native state
but never accesses QuickJS runtime objects. Optimized publication uses the
baseline generation, so late work cannot replace a newer tier, and exact-source
optimized cache entries converge duplicate source without sharing JavaScript
object lifetime.

The installed object also exposes lifecycle controls and telemetry:

```js
const completed = unijit.wait(native, 5000);
const state = unijit.stats(native);
const cancelled = unijit.cancel(native);
```

`wait` bounds only the caller's wait and returns whether the current task
reached a terminal state. `cancel` immediately removes queued work or requests
cooperative cancellation from a running compiler. `stats` reports the active
tier and generation, hotness and compilation outcomes, promotions, current
task state, cancellation state, OSR attempts/entries/exits, scheduler load,
native code size, and IR node counts. Garbage collection requests cancellation
automatically; task-owned shared state remains valid until a running job
observes cancellation and exits.

## Specialization contract

The first tier accepts conventional `function` source with zero to 64 unique
ASCII parameter names and a body containing exactly one `return` expression.
The expression may contain parameters, decimal numeric literals, parentheses,
unary `+`/`-`, binary `+`, `-`, `*`, and `/`, and one top-level ordered `<`,
`<=`, `>`, or `>=` comparison. Arithmetic lowers to Float64 SSA; comparisons
lower to ordered Word results and the adapter returns an actual JavaScript
Boolean in both baseline and optimized tiers. NaN comparisons are false as
required by JavaScript Number semantics.

Arrow functions, closures, default or rest parameters, property access, calls,
statements, chained comparisons, and coercive operands are rejected with a
source byte position.
This strict boundary prevents silently compiling semantics that the current IR
cannot represent.

The counted-loop tier additionally accepts a conventional body with Float64
`let` initializers, one unit-increment `for` loop, arithmetic assignments, and
ordered `if`/`else` comparisons whose arms update loop locals. Every mutable
value becomes a typed CFG block parameter, conditional updates merge through
SSA edges, and the loop backedge polls an execution-context safepoint. Calls,
object access, coercion, `break`, `continue`, and nested loops remain rejected.
This tier is intended for complete numeric recurrences rather than repeatedly
crossing the VM boundary for a single arithmetic expression.

At installation, the module captures the original
`Function.prototype.toString` callable. Per-function `toString` overrides and
later prototype mutation therefore cannot substitute different source for the
function being compiled. The compiled closure owns its executable allocation
through a QuickJS class object and releases it from the class finalizer. The
complete counted-loop path currently installs one optimized CFG code version
in the baseline slot and reports `tierable: false`, because repeating the same
CFG compilation has no distinct lower-latency form.

## Reproducible V8 target benchmark

The embedded benchmark and Node execute the same
`benchmark/quickjs/numeric_call.js` source. Stock QuickJS and both V8 modes run
the source loop, while UniJIT compiles that complete loop into CFG native code;
the measured region therefore has one callable boundary per sample rather than
one boundary per iteration. Every QuickJS, UniJIT, V8
Jitless, and V8 sample must produce the same Float64 bit pattern:

```sh
cmake -S . -B build/quickjs -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_QUICKJS_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/quickjs --target unijit_quickjs_benchmark
python3 benchmark/quickjs/run_v8.py \
  --unijit build/quickjs/bin/unijit_quickjs_benchmark \
  --node "$(command -v node)" \
  --warmup 10000 --iterations 100000 --samples 7 \
  > build/quickjs/quickjs-targets.json
```

The record includes the exact UniJIT and QuickJS revisions, Node and V8
versions, host OS and architecture, measurement policy, medians, checksums,
and UniJIT ratios against both V8 modes. This remains one numeric loop rather
than a whole-language performance claim; CI retains every target record for
trend analysis.
