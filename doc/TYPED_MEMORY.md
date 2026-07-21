# Bounded typed memory

## Delivered scope

UniJIT exposes a region-based memory contract instead of allowing IR to form
arbitrary process pointers. The first delivered slice supports Word loads and
stores with 8-, 16-, 32-, or 64-bit widths in both straight-line and CFG IR.
It is implemented by the reference interpreters, optimizer, register
allocator, AArch64, x86-64, and RISC-V 64 native backends.

The following related capabilities remain roadmap items and are not implied by
the Word-memory API: Float32/Float64 memory values, standalone byte-swap
operations, trusted runtime-object layouts, frame-local slots, vector memory,
atomics, and arbitrary address arithmetic.

## Region and descriptor model

An embedder declares the number of regions when it constructs a
`FunctionBuilder` or `ControlFlowBuilder`. Before interpretation or managed
native invocation, it binds an array of `runtime::MemoryRegion` descriptors to
the `ExecutionContext`. Each descriptor contains a data pointer, a byte size,
and a writable flag. The embedder owns both the descriptor array and its data
and keeps them alive for the complete invocation.

Each IR access carries a `MemoryAccessDescriptor`:

| Field | Contract |
|---|---|
| `region` | Zero-based index into the function's declared region table |
| `width` | Exactly 1, 2, 4, or 8 bytes |
| `alignment` | Power of two, at least 1 and no greater than the width |
| `byte_order` | Native, explicitly little-endian, or explicitly big-endian |
| `sign_extend` | Zero-extend by default; sign-extend a load narrower than Word |
| `is_volatile` | Preserve byte-observable access semantics |
| `alias_class` | Frontend alias identity retained for future dependence analysis |

Stores return the original Word value. `sign_extend` is invalid on stores.
All current memory nodes are treated as effectful: optimization preserves their
order and does not yet use `alias_class` to remove or reorder an access.

## Dynamic checks and exits

The dynamic operand is a byte offset whose 64-bit representation is checked as
an unsigned value. This makes negative Word offsets fail without wrapping.
Before dereferencing, execution checks that:

1. a non-null execution context and region table are bound;
2. the declared region exists and has a non-null base for non-empty storage;
3. a store targets a writable region;
4. `width <= size` and `offset <= size - width`;
5. the absolute address satisfies the descriptor alignment.

The subtraction form avoids an overflowing `offset + width` check. A failure
records `ExitReason::kRuntime`, the access site, and the original offset in the
execution context, then returns `StatusCode::kRuntimeExit`. A direct generated
entry invoked without the required context fails closed. Memory sites share
the same function-local unique site namespace as guards and safepoints.

Every native memory site has a stack map. Values live at a failing access are
captured through the same diagnosed-exit path used by guards, so a frontend can
reconstruct its logical state after the generated frame has returned through
the ordinary ABI boundary.

## Byte-exact semantics and lowering

The interpreter reads and writes bytes explicitly, so endian conversion and
unaligned access do not depend on C or C++ object aliasing or alignment rules.
Current supported native targets are little-endian:

- AArch64 and x86-64 use their native scalar width operations and explicit
  byte reversal for big-endian descriptors. Their architecture contracts allow
  the emitted scalar forms to access the verified unaligned address.
- RISC-V 64 uses one native width operation for naturally aligned native or
  little-endian access. It assembles or scatters bytes for explicit big-endian
  access and for descriptors that permit an address less aligned than the
  width, avoiding platform-dependent misaligned traps.

This distinction is a lowering choice only. All paths have identical bounds,
permission, alignment, byte-order, sign-extension, result, and exit semantics.

## Public construction example

```cpp
using namespace unijit;

ir::FunctionBuilder builder(2, 1);
ir::MemoryAccessDescriptor access;
access.width = ir::MemoryWidth::k32;
access.alignment = 1;
access.byte_order = ir::MemoryByteOrder::kLittleEndian;

const ir::Value stored = builder.store_word(
    builder.parameter(0), builder.parameter(1), access, 100);
const ir::Value loaded = builder.load_word(
    builder.parameter(0), access, 101);
builder.set_return(builder.add(stored, loaded));
const ir::Function function = std::move(builder).build();

std::array<std::uint8_t, 64> bytes{};
runtime::MemoryRegion region{bytes.data(), bytes.size(), true};
runtime::ExecutionContext context;
context.bind_memory_regions(&region, 1);

auto compiled = jit::Compiler::compile(function);
const std::array<ir::Word, 2> arguments = {3, 0x12345678};
auto result = compiled.function->invoke(arguments.data(), arguments.size(),
                                        &context);
```

Production code checks every returned `Status`; the compact example only
shows the ownership and construction shape.

## Resource and qualification gates

Compilation defaults limit a function to 64 declared regions and 65,536
memory accesses. The verifier rejects invalid descriptors, undeclared regions,
unavailable or wrongly typed operands, duplicated descriptors, and duplicated
runtime sites before native publication.

Qualification covers both IR forms, permission and range failures, null
contexts, optimizer preservation, stack maps, all four widths, both explicit
byte orders, native order, signed extension, natural-alignment fast paths, and
unaligned byte paths. The native matrix is checked against the interpreter and
the expected byte layout on AArch64, real Ubuntu and Windows x86-64, and real
RISC-V 64 hardware. The versioned two-path microbenchmark and initial real-host
records are documented in [`PORTABILITY.md`](PORTABILITY.md).

The next memory milestones are Float32/Float64 values and standalone byte
reversal, followed by verified frame slots. Strict 128-bit SIMD then builds on
the same region, alias, endian, alignment, bounds, stack-map, and target-profile
contracts. Atomics remain a later, naturally aligned contract with explicit
memory ordering; they will not reuse the unaligned scalar fallback.
