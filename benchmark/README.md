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

The CFG Float64 benchmark keeps four loop-carried values live while native
addition, subtraction, multiplication, and division update them. It reports
latency per completed loop iteration and native code size, making register-bank
transport changes directly comparable without a language-runtime boundary:

```sh
build/benchmark/benchmark/unijit_cfg_float64_benchmark \
  --loop-iterations 1000 --warmup 100 --invocations 1000 --samples 7 \
  > build/benchmark/cfg-float64.json
```

Every native sample is checksum-matched against the CFG reference interpreter.
Hosted platform validation rejects fewer than seven samples, a narrower
measurement boundary, less than a 5x interpreter speedup, or more than 400
native code bytes, and retains the structured gate decision with the raw record.

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
checksum requirement remains in force. The record includes the direct
UniJIT-over-LuaJIT ratio; hosted validation retains it and requires at least a
1.25x speedup over stock Lua plus a 1.10x speedup over LuaJIT.

Passing `--script benchmark/lua/float_call.lua` measures the guarded Float64
specialization with identical floating-point inputs in stock Lua, UniJIT, and
LuaJIT. Checksums are compared without integer truncation before performance
ratios are emitted.

## QuickJS, V8 Jitless, and V8 target comparison

The JavaScript target runner executes one checked-in source file in stock
QuickJS, the UniJIT QuickJS closure, V8 Jitless, and normal V8. UniJIT compiles
the complete numeric loop, including its recurrence state and reset branches,
while the other engines execute the same source loop. Each sample crosses its
callable boundary once, and all results must have the same Float64 bit pattern:

```sh
cmake -S . -B build/quickjs -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_QUICKJS_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/quickjs --target unijit_quickjs_benchmark
python3 benchmark/quickjs/run_v8.py \
  --unijit build/quickjs/bin/unijit_quickjs_benchmark \
  --node "$(command -v node)" \
  > build/quickjs/quickjs-targets.json
```

The JSON records exact source revisions and Node/V8 versions. Hosted
validation pins Node, runs both V8 modes, and retains the record as an
artifact. It also requires at least a 1.25x speedup over stock QuickJS and a
1.10x speedup over V8 Jitless at the complete-loop boundary, retaining the
machine-readable gate decision beside the raw result.

## PocketPy and CPython 3.14.6 JIT target comparison

The Python target runner executes one checked-in workload in PocketPy, the
UniJIT PocketPy callable, CPython 3.14.6 with JIT disabled, and CPython 3.14.6
with JIT enabled. UniJIT compiles the complete range loop and its conditional
state updates, while the other engines execute the same source loop. Each
sample crosses its callable boundary once, and all results must have the same
Float64 bit pattern:

```sh
cmake -S . -B build/pocketpy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUNIJIT_BUILD_POCKETPY_REFERENCE=ON \
  -DUNIJIT_BUILD_BENCHMARKS=ON
cmake --build build/pocketpy --target unijit_pocketpy_benchmark
python3 benchmark/pocketpy/run_cpython.py \
  --unijit build/pocketpy/bin/unijit_pocketpy_benchmark \
  --python /path/to/jit-capable/python3.14 \
  > build/pocketpy/pocketpy-targets.json
```

The runner requires exactly CPython 3.14.6, checks `sys._jit` availability and
the requested runtime mode, and records exact source revisions. Hosted
validation verifies the official Python.org package checksum before extracting
it below `build/` and retains the comparison record as an artifact.
Hosted validation requires at least a 1.25x speedup over stock PocketPy and a
1.10x speedup over both CPython interpreter and JIT modes at the complete-loop
boundary. `doc/QUALIFICATION.md` defines replay, stress, and gate policy.
