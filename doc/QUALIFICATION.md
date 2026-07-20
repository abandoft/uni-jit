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
- straight-line Float64 SSA with bounded finite inputs and all four arithmetic
  operations;
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
