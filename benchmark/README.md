# Core benchmarks

The bootstrap benchmark compares the public reference-interpreter API with a
direct call to published native code. It uses a fixed seed, a separate warmup,
multiple measured samples, median latency, and result checksums.

```sh
cmake -S . -B build/benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=OFF \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/benchmark --parallel
build/benchmark/benchmark/unijit_core_benchmark \
  --warmup 10000 --iterations 100000 --samples 7 \
  > build/benchmark/core-arithmetic.json
```

The JSON record contains the OS, architecture, compiler, iteration policy,
compilation latency, optimized IR size, native code size, median execution
latencies, and checksums. Generated results stay under `build/`; curated
release reports will be committed separately with their environment manifest.

## Lua reference baseline

The pinned Lua 5.5 interpreter is compiled directly by CMake. LuaJIT's native
build is run on a source copy below `build/references/`, so neither submodule is
modified by generated files.

```sh
cmake -S . -B build/language -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=OFF \
  -DUNIJIT_BUILD_LUA_REFERENCE=ON \
  -DUNIJIT_BUILD_LUAJIT_REFERENCE=ON
cmake --build build/language --parallel
python3 benchmark/lua/run_reference.py \
  --lua build/language/bin/lua5.5-reference \
  --luajit build/language/references/luajit/src/luajit \
  > build/language/lua-reference.json
```

Both engines execute the same source with identical warmup and sample policy.
The runner rejects mismatched checksums and records the exact submodule commits.

## Lua integer frontend comparison

The integer-call workload compares stock Lua 5.5, a guarded UniJIT native
closure, and LuaJIT using identical Lua source, inputs, warmup, and sample
counts. The surrounding loop remains Lua code in all three engines, so the
record includes both call-boundary and kernel execution costs.

```sh
cmake -S . -B build/language -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_TESTS=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON \
  -DUNIJIT_BUILD_LUA_REFERENCE=ON \
  -DUNIJIT_BUILD_LUAJIT_REFERENCE=ON
cmake --build build/language --parallel --target \
  unijit_lua55 unijit_lua55_benchmark_runner unijit_luajit_reference
python3 benchmark/lua/run_integer.py \
  --lua build/language/bin/lua5.5-reference \
  --unijit build/language/bin/lua5.5-unijit \
  --luajit build/language/references/luajit/src/luajit \
  > build/language/lua-integer.json
```

The runner requires exact checksums across all three engines and records the
UniJIT, Lua, and LuaJIT revisions. Results describe this narrow integer
workload only; they are not presented as whole-language performance.

Passing `--script benchmark/lua/integer_loop.lua` to the same runner measures
an entire 1,000-iteration Lua numeric loop compiled through CFG SSA. Reported
latency is normalized per inner-loop iteration, and the exact three-engine
checksum requirement remains in force.

Passing `--script benchmark/lua/float_call.lua` measures the guarded Float64
specialization with identical floating-point inputs in stock Lua, UniJIT, and
LuaJIT. Checksums are compared without integer truncation before performance
ratios are emitted.

## PocketPy call comparison

The PocketPy benchmark compares stock 2.1.8 bytecode and a guarded UniJIT
Float64 callable through the same `py_call` API. It uses identical inputs,
warmup, sample counts, and bitwise result checksums:

```sh
cmake -S . -B build/pocketpy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/pocketpy --target unijit_pocketpy_benchmark
build/pocketpy/bin/unijit_pocketpy_benchmark \
  --warmup 10000 --iterations 100000 --samples 7 \
  > build/pocketpy/pocketpy-call.json
```

This isolates the current numeric call tier. It does not substitute for the
planned shared-kernel comparison with Python 3.14.6 JIT.
