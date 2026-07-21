# Release qualification

UniJIT treats correctness under generated inputs, concurrent native-code
lifecycle safety, and repeatable target performance as release-blocking product
properties. Qualification executables are opt-in for ordinary local builds and
mandatory in hosted continuous validation.

## Deterministic differential fuzzing

`unijit_differential_fuzz` generates valid programs from a reported 64-bit
seed, compiles them with the production pipeline, and compares every native
result bit-for-bit with the matching reference interpreter. Each corpus covers:

- straight-line Word SSA with random constants, addition, subtraction,
  multiplication, spills, and optimizer input shapes;
- straight-line Float64 SSA with bounded finite inputs, all four binary
  arithmetic operations, and exact unary negation;
- typed Word and Float64 CFGs with diamonds, 1 to 12 loop-carried state values,
  permuted and duplicated edge sources, ordered comparisons, backedges, and
  mandatory safepoints.

Mismatch diagnostics contain the tier, seed, program index, input index,
statuses, and exact result bits. A hosted failure can therefore be replayed
locally without preserving a mutable random corpus.

## Concurrent code-cache stress

`unijit_code_cache_stress` runs configurable reader and writer populations over
a deliberately undersized cache. Writers concurrently publish, replace, and
invalidate fingerprinted generations while readers look up, invoke, and retain
leases across eviction. Every identity has a deterministic expected result.

The test rejects wrong execution, failed publication, counter drift, capacity
overflow, failed clearing, and reclamation of a retained lease. The dedicated
workflow also runs this executable under ThreadSanitizer, in addition to the
repository-wide AddressSanitizer and UndefinedBehaviorSanitizer job.

Core runtime tests also invalidate an assumption while a billion-iteration
native CFG loop is registered, require its safepoint to exit at the dependency
site, reconstruct the entry frame, verify invalidation quiescence, replace the
stale cache generation, and preserve an independent sticky interrupt. This path
is included in the dedicated ThreadSanitizer job.

The same runtime suite races four invocation threads against repeated optimized
publication and withdrawal. It requires every immutable snapshot to return the
baseline-equivalent value, rejects late compiler generations, verifies failed
compilation retry delay, and exercises explicit restartable fallback after an
assumption exit. The installed-package consumer compiles against the public
tiering API as an external project.

The compilation-scheduler stress gate holds all workers behind a deterministic
start barrier while many producer threads submit repeated identity/generation
pairs. It verifies global deduplication, weighted admission, queued and active
cancellation, bounded telemetry, concurrent production compilation, W^X
publication, code-cache residency, native execution, drain shutdown, and exact
lifecycle counter reconciliation. Hosted qualification expands the corpus to
16 producers and 20,000 submissions and repeats it under ThreadSanitizer.

Core unit and installed-package tests also reduce each public compilation
budget below a valid fixture and require deterministic rejection before
verification, dominance analysis, or executable publication. Frontend tests
submit source beyond the 1 MiB retention ceiling and require native runtime
range/value errors without entering translation.

Typed CFG runtime-call qualification executes Word helpers inside backedge
loops, keeps Word and Float64 state live across each call, and marshals twelve
ordered mixed arguments so every target exercises register and stack sources.
It also proves effectful dead-result calls remain observable and that guard and
safepoint exits return safely after a helper has changed the link register.
The external installed-package consumer builds and executes the same public CFG
call API without access to private headers.

CFG optimizer qualification folds a constant diamond, removes its recursively
unreachable effectful arm, reuses duplicate local constants and arithmetic,
and proves that baseline compilation retains an otherwise reachable guard while
optimized compilation removes its unreachable native exit and metadata. The
deterministic differential generator then compares optimized native execution
with the original CFG interpreter across the committed and extended seed sets.

Safepoint telemetry qualification checks exact interrupted and completed counts
in both interpreters and native straight-line/CFG loops. Separate compilations
disable `measure_safepoint_polls` and must execute the same results while
leaving the call-scoped counter at zero; the installed-package consumer repeats
both policies through the public `CompilationOptions` API.

QuickJS and PocketPy frontend tests compile the same single-level counted loop
with ordered `break` and `continue` guards, execute it both as a native function
and through the stock language runtime, and bit-match the result with the source
semantics. Each runtime test proves that the callable starts with unoptimized
CFG node counts, measures exactly 10,000 safepoint backedges in one long call,
promotes exactly once through the bounded scheduler, and publishes a smaller
optimized graph. PocketPy additionally replays both
signed-zero counted-loop division exits after promotion and requires exact
frame reconstruction before `ZeroDivisionError`. Separate negative fixtures
require an explicit rejection when a control guard contains an unsupported
`else` arm.

Float64 comparison qualification covers `<`, `<=`, equality, and inequality
in straight-line and CFG interpreter/native execution, including NaN and both
signed zeroes. QuickJS exercises loose and strict numeric equality, PocketPy
exercises `==` and `!=`, and both adapters execute equality-controlled loops
through direct optimized translation and their stock-runtime baseline path.
The deterministic CFG generator varies all four core predicates, while the
installed-package consumer compiles equality through both public IR builders.

Float64 negation qualification passes positive and negative zero, both
infinities, and custom NaN payloads through the straight-line and CFG
interpreters and native compilers, and requires constant folding to toggle only
the sign bit. QuickJS and PocketPy repeat exact signed-zero and NaN checks in
baseline and optimized straight-line and counted-loop translation; stock
runtime callables cross asynchronous tier promotion and retain signed zero at
the language boundary. The deterministic generator emits unary negation in
both IR forms, and the installed-package consumer builds both public APIs.

Strided-loop coverage executes QuickJS prefix/postfix decrement and `+=`/`-=`
updates plus PocketPy one-, two-, and three-argument `range` forms. Positive
and reverse loops are combined with early control guards, while zero and
dynamic PocketPy steps must fail translation. Stock-runtime tests execute the
reverse-step variants through the public module boundary.

The hosted core platform matrix builds and executes the stock Lua, QuickJS,
and PocketPy adapters on real Ubuntu GCC/Clang x86-64 hosts, in addition to
macOS AArch64/x86-64. Windows MSVC x86-64 executes the core, Lua, and PocketPy;
upstream stock QuickJS is excluded there because its C sources require GNU
extensions rather than the MSVC C frontend. Linux ASan/UBSan repeats all three
frontend suites, while the separate Windows MSVC ASan lane covers Lua's
`longjmp` bridge under the native sanitizer runtime.

## CFG register-residency benchmark

`unijit_cfg_float64_benchmark` executes one typed CFG with four loop-carried
Float64 values and native addition, subtraction, multiplication, and division.
Every measured native result is bit-matched with the reference interpreter,
and the retained JSON reports native code bytes, compilation latency, checksum,
and median nanoseconds per completed source-loop iteration. Core platform
validation runs this benchmark on hosted Linux GCC/Clang x86-64, macOS
AArch64/x86-64, and Windows MSVC x86-64 hosts. Every host must retain at least
a 5x speedup over the reference interpreter, emit no more than 400 native code
bytes for the fixed 29-node workload, and retain both its raw record and
structured gate decision.

Unit tests separately force a Float64 register-cycle backedge and a ten-value
edge that exceeds every supported floating-point register bank. They verify
parallel copy semantics and typed stack fallback through native execution.

## Performance gates

`tool/performance_gate.py` consumes retained benchmark JSON instead of parsing
human-readable logs. It rejects the wrong schema, a narrower measurement
boundary, missing or non-finite ratios, and any result below the configured
floor. Hosted validation currently enforces:

| Target | Complete-loop minimum |
|---|---:|
| UniJIT over stock Lua 5.5 | 1.25x |
| UniJIT over LuaJIT | 1.10x |
| UniJIT over stock QuickJS | 1.25x |
| UniJIT over V8 Jitless | 1.10x |
| UniJIT over stock PocketPy | 1.25x |
| UniJIT over CPython 3.14.6 interpreter | 1.10x |
| UniJIT over CPython 3.14.6 JIT | 1.10x |

The margins are performance floors, not claims that the current observed ratio
is stable across unrelated machines. Both the raw comparison and the
machine-readable gate decision are retained as workflow artifacts.

Lua records include the direct `unijit_speedup_over_luajit` ratio and all four
hosted Lua baselines are retained. Its release gate uses the complete
1,000-iteration numeric-loop boundary rather than the narrower native-call
microbenchmark. The commercial loop gate runs three independent seven-sample
trials and rotates stock Lua, UniJIT, and LuaJIT through every process-order
position, then compares the median trial result for each engine. Raw trial
medians, execution orders, and a structured pass or failure decision are
retained even when a floor is missed.

## Local execution

```sh
cmake -S . -B build/qualification -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_BUILD_QUALIFICATION_TESTS=ON \
  -DUNIJIT_WARNINGS_AS_ERRORS=ON
cmake --build build/qualification --parallel
ctest --test-dir build/qualification -L qualification --output-on-failure
```

Both executables accept explicit scale and seed options. Generated JSON records
and all other run output must remain under the repository `build/` directory.
