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
