# Runtime execution contract

UniJIT native entries accept a value-bits argument area and an optional
execution context:

```cpp
std::int64_t generated(const std::int64_t* arguments,
                       unijit::runtime::ExecutionContext* context);
```

Passing a null context disables runtime polling. This is the lowest-overhead
path for trusted, bounded code. A non-null context enables diagnosed exits and
cooperative interruption without embedding frontend-specific state in the IR
or native backends. `CompiledFunction::requires_context()` identifies code with
mandatory runtime guards; its managed `invoke` path supplies a private context
when the caller does not provide one.

## Context ownership

One thread owns an `ExecutionContext` while compiled or interpreted code is
executing with it. Another thread may call `request_interrupt()` or
`clear_interrupt()`; the request flag is a lock-free 64-bit atomic. Exit
reason, site, and value fields are written only by the executing thread and
must be read after that invocation returns. A context must not be shared by
concurrent invocations.

Interruption is sticky until `clear_interrupt()` is called. Invocation clears
stale exit diagnostics but deliberately does not clear the interrupt request,
so a request made immediately before entry cannot be lost. `user_data` is an
opaque frontend-owned pointer and is never dereferenced by the core runtime.

## Safepoints and exits

Safepoints are explicit effectful IR nodes. Optimization cannot remove them
when their zero-valued SSA result is dead. The reference interpreters and all
three native backends implement the same behavior:

1. A null context or clear interrupt flag continues execution and produces a
   zero Word value.
2. A requested interrupt records `ExitReason::kSafepoint`, its stable site
   identifier, and a zero exit value.
3. Native code restores its complete frame and returns through the ordinary
   ABI boundary.
4. `CompiledFunction::invoke` translates the diagnostic into
   `StatusCode::kExecutionInterrupted` and retains the site in
   `Status::location()`.

Straight-line lowering supports safety around runtime-helper calls, including
caller-clobbered live values and link-register restoration. CFG lowering can
place a poll in a loop body so long-running generated code can be interrupted
without a signal handler or writable executable memory.

Float64 nonzero guards are explicit effectful SSA nodes. A passing finite,
infinite, subnormal, or NaN value continues with a zero Word effect result.
Either signed zero records `ExitReason::kRuntime`, the frontend-provided source
site, and a zero exit value before restoring the native frame. Managed
invocation reports `StatusCode::kRuntimeExit`; a frontend can then reconstruct
the language exception without unwinding through generated code. Calling a raw
native entry with a null context is only supported when `requires_context()` is
false. Optimization proves guards over known nonzero constants cannot exit and
removes them, so functions such as division by a literal keep the raw-entry
fast path without weakening dynamic-divisor checks.

Runtime helpers and generated code must not unwind a C++ exception across a
native entry. Later tiers will extend diagnosed runtime exits with full
deoptimization reconstruction metadata.
