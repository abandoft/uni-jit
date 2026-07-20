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
```

The source-string API avoids pretending that PocketPy 2.1.8 exposes reliable
source provenance for arbitrary function objects. Each call requires exactly
the declared number of arguments. PocketPy `int` and `float` arguments are
converted to Float64, other types raise `TypeError`, and the result is a
PocketPy `float`.

The compiled callable owns its executable allocation in PocketPy userdata.
The VM garbage collector releases it through the type destructor. The internal
callable type is final and rejects direct construction, so an object with
uninitialized native ownership cannot be created from script.

## Initial specialization contract

The first tier accepts one conventional `def` with zero to 64 unique ASCII
parameter names and exactly one `return` expression. The expression may contain
parameters, decimal numeric literals, parentheses, unary `+`/`-`, and binary
`+`, `-`, and `*`. All accepted operations lower to Float64 SSA.

Closures, annotations, default or variadic parameters, assignments, multiple
statements, calls, attribute access, division, and non-ASCII identifiers are
rejected with a source byte position. Python keywords are rejected as function
or parameter names. Division remains excluded until native guards can preserve
PocketPy's `ZeroDivisionError` instead of exposing the hardware IEEE-754 result.
This narrow contract prevents fallback-free native code from silently changing
unsupported Python behavior.

## Reproducible call benchmark

The embedded benchmark compares PocketPy bytecode and the UniJIT callable
through the same `py_call` boundary. Every sample must produce the same bitwise
checksum:

```sh
cmake -S . -B build/pocketpy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/pocketpy --target unijit_pocketpy_benchmark
build/pocketpy/bin/unijit_pocketpy_benchmark \
  --warmup 10000 --iterations 100000 --samples 7
```

On the initial Darwin AArch64 validation host, this arithmetic call workload
measured 132.661 ns for stock PocketPy and 93.617 ns for UniJIT, a 1.417x
speedup. This is a narrow call-boundary result, not yet a claim of parity with
Python 3.14.6 JIT. Shared Python/PocketPy kernels and broader language coverage
remain delivery gates.
