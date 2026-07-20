# Lua 5.5 frontend

UniJIT's first language integration compiles a deliberately strict subset of
real Lua 5.5 bytecode into the shared SSA and native backends. It reads the
`Proto` owned by the pinned stock Lua runtime; it does not patch Lua and does
not route code generation through LuaJIT or SLJIT.

## Embedding API

Build the integration with `-DUNIJIT_BUILD_LUA_REFERENCE=ON`, link
`unijit_lua55_frontend`, and preload the module in the embedding application:

```cpp
luaL_requiref(state, "unijit", luaopen_unijit, 1);
lua_pop(state, 1);
```

Lua code can then specialize a fixed-parameter function:

```lua
local unijit = require("unijit")
local native = unijit.compile(function(a, b, c)
  return (a - b) * (b + c)
end)
```

The returned value is a normal Lua C closure. It accepts extra arguments in
the same way a fixed-parameter Lua function does, but every declared argument
must be a Lua integer. Native code uses Lua's 64-bit wrapping semantics for
addition, subtraction, and multiplication.

`unijit.compile_float(function)` creates a separate Float64 specialization.
Every declared argument must carry Lua's Float64 tag (integer values are not
silently converted at the guard boundary), and the result is returned with the
Float64 tag. This explicit entry point keeps the initial specialization policy
deterministic until runtime type profiling and multi-version dispatch land.

## Supported bytecode contract

The initial tier accepts straight-line `MOVE`, integer `LOADI`/`LOADK`, integer
`ADDI`, `ADDK`, `SUBK`, `MULK`, `ADD`, `SUB`, `MUL`, and one-value fixed
returns. Arithmetic is accepted only when the corresponding Lua metamethod
fallback instruction is structurally present. Runtime integer guards make
that fallback unreachable in the specialized closure.

The Float64 tier accepts the corresponding straight-line numeric loads,
constant arithmetic, binary `ADD`/`SUB`/`MUL`/`DIV`, and one-value returns.
Integer literals are converted exactly as Lua does when paired with a Float64
operand. `DIV` always produces Float64 and therefore also accepts two integer
literals; other integer-only arithmetic and non-Float64 results are rejected
rather than changing Lua's numeric tag semantics.

Varargs, general branches, calls, tables, floating-point values, upvalue
access, and all other opcodes are rejected at compile time with the bytecode
position. This is an explicit capability boundary rather than a silent
semantic fallback. Later frontend tiers will add guarded exits and
deoptimization for the broader Lua language.

The first CFG path also accepts one structured numeric `for` loop when its
start and step are integer constants and the step is exactly 1. The limit is a
guarded runtime integer. Loop-carried Lua registers become explicit CFG block
parameters, and a bytecode liveness scan avoids carrying dead setup
registers. The production path uses overflow-safe eight-way body unrolling, a
bounded scalar tail, and one cooperative safepoint poll per dispatch, so no
more than eight source iterations occur between polls. Starts near
`math.maxinteger`, zero-iteration loops, and signed limits preserve stock Lua
results; nested loops, non-unit steps, and early returns are rejected for now.

The compiled closure owns its executable allocation through a Lua userdata
upvalue. Its finalizer is idempotent, so collection and adversarial repeated
finalizer calls cannot double-free native code.

The invocation fast path reads guarded arguments directly from Lua 5.5's
current `CallInfo` and `TValue` frame, then writes the integer result to the
reserved C-call stack space. This is intentionally coupled to the pinned Lua
5.5 source revision and is verified on every supported native backend. It
avoids treating Lua's public C API call overhead as part of the generated-code
cost while retaining an integer tag check for every declared parameter.
