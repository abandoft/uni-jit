# Trusted runtime object layouts

## Delivered scope

Trusted object layouts let generated code access a small, runtime-owned
primitive record without exposing an arbitrary process pointer to IR. The
delivered floor is available in straight-line and CFG IR, both reference
interpreters, the optimizer, register allocator, compiler metadata, and the
AArch64, x86-64, and RISC-V 64 native backends.

This is a distinct provenance class from bounded typed memory. Bounded regions
accept dynamic offsets and check each access. A trusted object has an immutable
compile-time layout identity, byte size, slot, field offset, and field type;
managed invocation validates its binding once before native entry and native
code then performs direct fixed-offset loads and stores.

## Layout and field contract

`FunctionBuilder::create_trusted_object` and
`ControlFlowBuilder::create_trusted_object` append a positional
`TrustedObjectDescriptor` containing:

| Field | Contract |
|---|---|
| `layout_identity` | Nonzero semantic ABI identity chosen by the embedder |
| `byte_size` | Native in-process record size, 8 through 2,048 bytes and a multiple of 8 |

The identity is a stable schema version or collision-resistant schema hash. It
is not a host address, C++ RTTI token, or transient allocation identity. A
layout change that can alter any field's offset, type, size, or meaning must
change the identity. The descriptor is part of compiled-function metadata and
survives optimization unchanged.

`load_object` and `store_object` use a declared object slot and a compile-time
byte offset. Phase one accepts exactly naturally aligned eight-byte `Word` or
`Float64` fields. The offset must be a multiple of eight and the complete field
must fit the declared layout. Stores return the original typed SSA value.
Float64 fields preserve all 64 IEEE payload bits.

Layouts use the native in-process ABI and byte order. They are not portable
serialized records and cannot be reused across incompatible target ABIs merely
because their identity matches. IR cannot obtain the object base, derive a new
address, select a dynamic field offset, or access padding through a narrower
type. GC references, tagged object references, write barriers, unions, packed
fields, vector fields, and aggregates are outside this delivered contract.

## Binding and ownership

Before interpretation or managed native invocation, the embedder binds a
positional array of `runtime::TrustedObject` records to an
`ExecutionContext`. Each record contains a base pointer, available byte size,
layout identity, and writable flag. Extra trailing bindings are permitted; the
compiled function observes only its declared prefix.

The embedder owns the binding array and object storage. Their addresses,
sizes, identities, and permissions remain unchanged for the complete
invocation. The executing thread may modify declared writable fields through
the generated function. Concurrent access to the same object requires the
embedder's own synchronization; the current primitive loads and stores are not
atomic operations. An `ExecutionContext` still belongs to one executing thread
at a time.

`bind_trusted_objects` performs structural validation. Interpretation and
`CompiledFunction::invoke` then perform one complete preflight before any IR
effect:

1. every declared slot has a binding;
2. each binding has the exact layout identity;
3. its size covers the complete declared layout without address-space wrap;
4. its base is naturally aligned for an eight-byte field;
5. every object containing any store is bound writable.

Failure returns `StatusCode::kInvalidArgument` before generated code enters or
any object field changes. This up-front permission rule deliberately rejects a
read-only binding for a function that may store on any control-flow path. A
load-only function accepts a read-only binding.

Functions with trusted objects require managed preflight. Their public
`native_entry()` is null even on a compatible host, preventing callers from
bypassing validation; `invoke()` uses the internal published entry only after
preflight. A missing context therefore fails closed. The reference
interpreters apply the same validation and field semantics.

## Effects, optimization, and lowering

Object loads and stores are currently ordered effects. Dead loads remain
observable, stores are never removed merely because their SSA result is dead,
and local value numbering is cleared across every object access. This is
conservative for runtime-owned state and avoids assuming alias independence
before the IR has an explicit immutable/noalias contract.

After preflight, each backend loads the trusted-object table from the execution
context, selects the fixed binding, loads its base, and performs one native
eight-byte field operation. There is no per-field dynamic bounds branch in
generated code. The maximum of 64 object bindings and the 2,048-byte layout
ceiling keep both binding and field offsets representable by the current
RISC-V 64 signed 12-bit lowering while also bounding metadata and validation
latency.

`CompilationStats::trusted_objects` reports the retained descriptor count.
The independent `CompilationLimits::maximum_trusted_objects` default is 64 and
is enforced before verification and lowering.

## Construction example

```cpp
struct alignas(8) CounterLayout {
  unijit::ir::Word value;
  double scale;
};

constexpr std::uint64_t kCounterLayoutV1 =
    UINT64_C(0x434f554e54455231);

unijit::ir::FunctionBuilder builder(
    {unijit::ir::ValueType::kWord});
const auto counter = builder.create_trusted_object(
    kCounterLayoutV1, sizeof(CounterLayout));
const auto stored = builder.store_object(
    counter, offsetof(CounterLayout, value), builder.parameter(0));
builder.set_return(stored);
const auto function = std::move(builder).build();

CounterLayout data{0, 1.0};
unijit::runtime::TrustedObject binding{
    &data, sizeof(data), kCounterLayoutV1, true};
unijit::runtime::ExecutionContext context;
context.bind_trusted_objects(&binding, 1);

auto compiled = unijit::jit::Compiler::compile(function);
const std::array<unijit::ir::Word, 1> arguments = {42};
auto result = compiled.function->invoke(arguments.data(), arguments.size(),
                                        &context);
```

Production embedding code checks every builder, binding, compilation, and
invocation status. The compact example only shows the ownership model.

## Qualification and next steps

The core matrix covers Word and Float64 loads/stores, store results,
interpreter/optimized/native parity, baseline and optimized compilation, CFG
cross-block state, read-only load-only bindings, repeated managed validation,
and untouched neighboring fields. Negative fixtures cover missing contexts,
identity mismatch, undersized or null storage, misaligned bases and fields,
write permission, malformed descriptors, fields beyond a layout, and resource
limits. Native execution is required on AArch64 and real Ubuntu/Windows
x86-64; RISC-V 64 remains a real-host release gate in addition to backend
syntax validation.

Future phases may add immutable/noalias metadata, verified narrower integers,
vectors, aggregate copies, GC reference fields with frontend-supplied barriers,
and versioned layout adaptation. None may expose a raw object address or weaken
managed preflight. Generated-code atomics remain a separate explicit
memory-order contract.
