# Generated-code atomic memory contract

## Scope

UniJIT atomics operate only on naturally aligned integer cells inside a
declared bounded `ExecutionContext` memory region. They never accept an IR
pointer, host object address, GC reference, vector lane, or unbounded local
address. The first delivered widths are 8, 16, 32, and 64 bits on AArch64,
x86-64, and RISC-V 64.

Atomic memory is a distinct concurrency contract, not a volatile spelling of
ordinary bounded memory. An embedder must allocate storage suitable for the
declared atomic width and must not race atomic generated-code access with
ordinary non-atomic access. Concurrent interpreter and native mutation of the
same cell is outside the contract; differential tests use separate initialized
regions. GC pointer atomics remain deferred until a frontend provides root
visibility and write barriers.

## Typed operations and results

The public IR surface comprises:

- atomic load and store;
- exchange;
- fetch-add, fetch-and, fetch-or, and fetch-xor;
- compare-exchange with strong or weak intent;
- acquire, release, acquire-release, and sequentially consistent fences.

Atomic load returns the observed zero-extended bit pattern in `Word`. Store
returns the original input `Word`, while memory receives its low-width bits.
Exchange and every fetch operation return the value observed before the
modification, zero-extended for widths below 64 bits. Integer addition wraps at
the selected cell width.

Compare-exchange takes an expected value and a desired value, compares their
low-width bits with memory, conditionally stores the desired low-width bits,
and returns both the zero-extended observed value and a canonical zero-or-one
success value. The success result is equivalent to comparing the observed
low-width value with the expected low-width value. A weak operation may be
implemented as strong and therefore need not introduce a spurious failure;
when a backend later selects a genuinely weak primitive it must preserve the
explicit success result.

## Memory orders

The order vocabulary is `relaxed`, `acquire`, `release`, `acq_rel`, and
`seq_cst`. Verification rejects orders outside these operation-specific sets:

| Operation | Accepted order |
|---|---|
| Load | relaxed, acquire, seq_cst |
| Store | relaxed, release, seq_cst |
| Exchange and fetch operations | all five orders |
| Compare-exchange success | all five orders |
| Compare-exchange failure | relaxed, acquire, seq_cst |
| Fence | acquire, release, acq_rel, seq_cst |

A compare-exchange failure order cannot be stronger than its success order.
In particular, acquire failure requires acquire, acq_rel, or seq_cst success,
and seq_cst failure requires seq_cst success. Release and acq_rel are never
valid failure orders. A relaxed fence is rejected instead of retained as an
effectful no-op.

The interpreter may implement an order more strongly than requested, but it
must never implement it more weakly. Native lowering uses the weakest target
sequence that satisfies the declared order. Optimizers treat every atomic
operation and fence as ordered and effectful: they are not folded, eliminated,
commoned, reordered across another memory effect, or changed to ordinary
loads/stores.

## Provenance and failure atomicity

An atomic access descriptor names one bounded region, alias class, width, and
memory order. Byte order is always native, sign extension is forbidden, and
the required alignment is exactly the access width. The verifier rejects a
128-bit width, non-power-of-two or overstated alignment, explicit endian
conversion, and malformed order combinations.

Before touching memory, the interpreter and generated code validate:

1. a context and declared region binding exist;
2. the complete access width is in range under unsigned offset arithmetic;
3. a modifying operation has write permission;
4. the absolute target address is naturally aligned.

Any failure records the declared site and byte offset and returns through the
ordinary diagnosed runtime-exit path. A failed preflight changes no byte and
performs no atomic operation. Compare-exchange mismatch is a successful IR
execution with a false success result, not a runtime exit.

## Target mapping and progress

- x86-64 uses aligned loads/stores under the TSO model, `LOCK` read-modify-write
  forms, `CMPXCHG`, and only the fences required by the declared order. A
  sequentially consistent store cannot silently become a plain store. This
  mapping is implemented for both straight-line and CFG IR: exchange and
  sequentially consistent stores use `XCHG`, fetch-add uses `LOCK XADD`,
  compare-exchange uses `LOCK CMPXCHG`, bitwise fetch operations use a
  `LOCK CMPXCHG` retry loop, and a sequentially consistent fence uses `MFENCE`.
- AArch64 uses acquire/release load-store forms and LSE read-modify-write
  instructions only when the immutable target profile authorizes LSE.
  Otherwise it uses bounded LL/SC attempts followed by a progress-preserving
  runtime fallback rather than an unbounded exclusive loop.
- RISC-V 64 requires an explicit `A` target feature for AMO or LR/SC lowering.
  Baseline RV64IMD does not imply atomic-extension availability. Bounded LR/SC
  retries fall back to a runtime helper; unsupported widths or profiles are
  reported by capability preflight instead of optimistically emitted.

Runtime fallback is a disclosed `helper` lowering decision. Helpers use the
same resolved region address, width, order, and return-value contract; they do
not receive an unchecked frontend pointer. Lock freedom is therefore a target
capability, while semantic availability and progress remain deterministic.

The current delivery boundary enables atomic capability preflight only for
x86-64. AArch64 and RISC-V 64 remain `unsupported` before code emission until
their mappings and target-feature contracts are complete; no release claim
combines the x86-64 slice with those pending backends.

## Qualification gates

Delivery requires both IR forms, verifier-negative coverage, reference
execution, baseline and optimized native parity, installed-package use, and
target-profile capability reporting. Tests cover every operation, width, and
valid order; truncation and wraparound; compare-exchange match and mismatch;
read-only, out-of-range, and misaligned failure atomicity; CFG edges; spills;
and diagnosed stack maps.

Concurrency qualification adds message-passing and store-buffering litmus
cases, contended fetch-add and compare-exchange stress, deterministic replay,
ThreadSanitizer coverage of runtime integration, and real AArch64, Ubuntu and
Windows x86-64, and RISC-V 64 execution. No release claims atomics from encoder
unit tests alone.

The completed x86-64 slice has baseline and optimized native parity for both IR
forms, all four widths and every valid order, compare-exchange match and
mismatch, diagnosed read-only/out-of-range/misaligned failures with stack maps,
and contended multi-thread fetch-add. It passes the complete core suite with
GCC and Clang plus ASan/UBSan on a real Ubuntu x86-64 host. Cross-architecture
litmus tests, Windows x64 execution, installed-package coverage, and the two
remaining native backends are still release gates for the overall atomic
feature block.
