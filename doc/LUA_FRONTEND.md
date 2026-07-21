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
Float64 tag. This explicit entry point keeps numeric dispatch deterministic.

Both entry points publish a verified baseline immediately. Straight-line
callables claim background optimization after 64 invocations. Accepted numeric
loops also derive loop-latch executions from their constant or guarded
parameter start and nonzero step plus guarded integer limit, and can claim
optimization after 10,000 iterations in one call. Structured `break` and
early-return guards against the induction value additionally clamp that count
to the exact first exiting iteration, avoiding premature promotion when a
large declared range exits early.
The bounded worker compiles an immutable copy of numeric bytecode and constants;
it never reads a Lua stack, closure, `Proto`, or other garbage-collected object.
Publication checks the captured baseline generation so late work cannot replace
a newer callable version.

The module exposes lifecycle controls and telemetry:

```lua
local completed = unijit.wait(native, 5000)
local state = unijit.stats(native)
local cancelled = unijit.cancel(native)
```

`stats` reports active tier and generation, invocation and backedge hotness,
compilation outcomes, promotions, OSR attempts/entries/exits, scheduler state,
code size, and IR size.
`wait` bounds only the caller's wait. `cancel` immediately removes queued work
or requests cooperative cancellation from a running compiler; collection of
the callable requests the same cancellation automatically.

## Supported bytecode contract

The initial tier accepts straight-line `MOVE`, integer `LOADI`/`LOADK`, integer
`ADDI`, `ADDK`, `SUBK`, `MULK`, `ADD`, `SUB`, `MUL`, unary `UNM`/`BNOT`, and
dynamic or constant `BAND`/`BOR`/`BXOR`, immediate `SHLI`/`SHRI`, dynamic
`SHL`/`SHR`, plus one-value fixed returns. Binary arithmetic, bitwise, and
shift operations are accepted only when the corresponding Lua metamethod
fallback instruction is structurally present.
Runtime integer guards make every accepted metamethod dispatch unreachable in
the specialized closure. Integer unary minus wraps exactly as Lua does at
`math.mininteger`, and all bitwise operations consume all 64 integer bits.
Shifts match `luaV_shiftl`: negative amounts reverse direction, logical right
shift never propagates the sign bit, and magnitudes of 64 or more return zero.
Right shift negates its amount modulo 2^64, retaining Lua's exact
`math.mininteger` behavior.

The Float64 tier accepts the corresponding straight-line numeric loads,
constant arithmetic, binary `ADD`/`SUB`/`MUL`/`DIV`, unary `UNM`, and one-value
returns. Float64 unary minus toggles only the sign bit, preserving signed zero,
infinity, and NaN payloads.
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
start and step are integer constants or guarded function parameters and the
step is nonzero. The limit is a guarded runtime integer. Positive and negative
steps preserve Lua 5.5's direction-sensitive zero-iteration and wrapping-
integer semantics, including strides equal to `math.mininteger`. A zero
parameter step raises Lua's exact error before native entry.
Loop-carried Lua registers become explicit CFG block parameters, and a bytecode
liveness scan avoids carrying dead setup registers. Loop bodies use the same
integer arithmetic contract, including exact `UNM`, `BNOT`, `BAND`, `BOR`, and
`BXOR` plus `SHLI`, `SHRI`, `SHL`, and `SHR`, in both tiers. Constant bitwise
and shift setup expressions remain visible to the loop start/step analysis.
The constant-step baseline
emits one scalar body copy, while its optimized path uses overflow-safe
eight-way body unrolling whenever the seven-step group offset is representable
and otherwise safely retains a scalar group. Parameter steps use Lua-equivalent
unsigned remaining-iteration counts: the baseline consumes one iteration per
dispatch, and the optimized tier selects eight-body groups plus a bounded 0–7
iteration tail without signed-overflow tests. Every form polls one cooperative
safepoint per dispatch, so no more than eight source iterations occur between
optimized polls. Starts and limits near either signed boundary, reverse loops,
zero-iteration loops, and strides that cross zero preserve stock Lua results.

The loop body may contain one single-level integer condition using `==`, `~=`,
`<`, `<=`, `>`, or `>=`. Its true arm may be a straight-line supported
arithmetic region, `break`, or a one-value early `return`, with straight-line
statements allowed before and after the guard. Branch merges and exits carry
the exact loop-local state. The baseline emits a compact scalar CFG; the
optimized tier expands eight independently guarded source iterations while
retaining one cooperative poll per group and a 0–7 iteration tail. Multiple
conditions, `else` arms, nested loops, calls, tables, and arbitrary jumps remain
explicitly rejected.

The compiled closure owns shared tier state through a Lua userdata upvalue. Its
finalizer cancels outstanding compilation and is idempotent, so collection and
adversarial repeated finalizer calls cannot double-free native code. A running
job retains the snapshot and tier state until it reaches a terminal result.

The invocation fast path reads guarded arguments directly from Lua 5.5's
current `CallInfo` and `TValue` frame, then writes the integer result to the
reserved C-call stack space. This is intentionally coupled to the pinned Lua
5.5 source revision and is verified on every supported native backend. It
avoids treating Lua's public C API call overhead as part of the generated-code
cost while retaining an integer tag check for every declared parameter.
