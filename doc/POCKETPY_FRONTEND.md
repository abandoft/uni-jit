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

## Specialization contract

The first tier accepts one conventional `def` with zero to 64 unique ASCII
parameter names and exactly one `return` expression. The expression may contain
parameters, decimal numeric literals, parentheses, unary `+`/`-`, and binary
`+`, `-`, `*`, and `/`. All accepted operations lower to Float64 SSA.

Closures, annotations, default or variadic parameters, assignments, multiple
statements, calls, attribute access, and non-ASCII identifiers are rejected with
a source byte position. Python keywords are rejected as function or parameter
names. Every division emits an inline Float64 nonzero guard at the `/` source
site; both `+0.0` and `-0.0` exit through the execution context and become
PocketPy's `ZeroDivisionError("float division by zero")`. The compiled record
retains the source resume offset and reconstructs every entry parameter plus
the exact guarded divisor bits. The adapter maps the exit only after validating
that record and reconstructed trigger, so future runtime exits cannot be
silently misclassified as division errors. This narrow contract prevents
fallback-free native code from silently changing unsupported Python behavior.

The counted-loop tier accepts four-space-indented numeric functions with
Float64 local initializers, one `for name in range(count)` loop, arithmetic or
augmented assignments, and ordered `if`/`else` arms that update loop locals.
Mutable state is carried through typed CFG block parameters, branches merge via
SSA edges, and every backedge polls an execution-context safepoint. Calls,
nested loops, `break`, `continue`, and loop division are currently rejected;
division stays out of this tier until CFG exits can reproduce PocketPy's
checked exception semantics.

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
