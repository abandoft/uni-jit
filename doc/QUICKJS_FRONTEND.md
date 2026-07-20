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

## Initial specialization contract

The first tier accepts conventional `function` source with zero to 64 unique
ASCII parameter names and a body containing exactly one `return` expression.
The expression may contain parameters, decimal numeric literals, parentheses,
unary `+`/`-`, and binary `+`, `-`, `*`, and `/`. These operations lower to
Float64 SSA and preserve JavaScript Number arithmetic for the accepted subset.

Arrow functions, closures, default or rest parameters, property access, calls,
statements, and coercive operands are rejected with a source byte position.
This strict boundary prevents silently compiling semantics that the current IR
cannot represent.

At installation, the module captures the original
`Function.prototype.toString` callable. Per-function `toString` overrides and
later prototype mutation therefore cannot substitute different source for the
function being compiled. The compiled closure owns its executable allocation
through a QuickJS class object and releases it from the class finalizer.

## Reproducible V8 target benchmark

The embedded benchmark and Node execute the same
`benchmark/quickjs/numeric_call.js` source. Timing surrounds a language-level
loop that calls the selected kernel, so the comparison does not include one
C++-to-VM transition per measured iteration. Every QuickJS, UniJIT, V8
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
and UniJIT ratios against both V8 modes. This remains one numeric kernel rather
than a whole-language performance claim; CI retains every target record for
trend analysis.
