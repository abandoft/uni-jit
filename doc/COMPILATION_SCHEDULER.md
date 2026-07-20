# Background compilation scheduler

`unijit::jit::CompilationScheduler` is the shared C++17 execution boundary for
frontend-owned background compilation. It does not understand a language
object model and does not publish a tier by itself. A frontend submits work
that retains safe source or IR ownership, compiles off the runtime thread, then
performs its own cancellation and generation checks before publication.

## Bounded admission and backpressure

Creation fixes the worker count, maximum queued task count, and maximum queued
estimated bytes. A worker count of zero selects the detected hardware
concurrency, bounded to 256; both queue limits must be nonzero. Every request
must supply a nonempty identity of at most 4096 bytes, a generation, a nonzero
estimated byte cost, a priority, and a callable job.

The queue budgets cover waiting tasks. Running work is bounded separately by
the fixed worker count. `try_submit()` never waits and reports
`kResourceExhausted` when either queue budget is full. `submit_for()` applies
backpressure until capacity becomes available, an equivalent task appears,
the scheduler closes, or the deadline expires with `kDeadlineExceeded`.

Requests with the same exact identity and generation share one ticket while
queued or running. This global deduplication complements per-function hotness
claims and prevents separate runtime objects from compiling the same version
concurrently. Completed tasks leave the identity table, so a later request can
retry that generation deliberately.

Urgent, normal, and background queues use bounded 4:2:1 weighted selection.
Urgent work wins immediately, while the burst limits guarantee that sustained
urgent traffic cannot permanently starve normal or background compilation.

## Tickets and cooperative cancellation

A copyable `CompilationTicket` retains task completion state independently of
the scheduler object. It exposes an opaque ID, queued/running/terminal state,
the cancellation request bit, nonblocking result inspection, blocking wait,
and bounded wait. Job exceptions are contained at the worker boundary and
reported as `kCodeGenerationFailed`; they do not terminate a worker or prevent
unrelated tasks from completing.

`cancel()` removes queued work immediately, releases both admission budgets,
and completes all deduplicated tickets with `kCancelled`. Running work receives
a lock-free `CompilationCancellation` request. Jobs must poll it at natural
phase boundaries and must check it again immediately before publishing code or
mutating frontend state. C++ cannot safely force-stop a thread that ignores the
token, so deterministic cancellation depends on that cooperative contract.

Jobs must retain source, IR, caches, and publication state through owned or
weak shared objects. They must not capture raw language-runtime userdata whose
collector can reclaim it concurrently. A common safe sequence is:

1. Retain immutable source or IR and the expected tier generation.
2. Check cancellation before expensive parsing and lowering phases.
3. Compile into an unpublished local result.
4. Recheck cancellation and frontend lifetime.
5. Publish with `TieredCode::publish_optimized(expected_generation)`.
6. Report a failed hotness compilation if publication is rejected.

## Shutdown and observability

`shutdown(kDrain)` closes admission, executes all accepted work, joins every
worker, and is idempotent. `shutdown(kCancel)` additionally removes queued
tasks and requests cancellation from active jobs before joining. Destruction
uses cancel shutdown as a fail-safe. Scheduler ownership should remain outside
its worker callbacks; a callback cannot synchronously join its own thread.

`wait_idle()` and `wait_idle_for()` observe a queue with no active workers.
Statistics report current and peak queue tasks/bytes and active workers, plus
submitted, deduplicated, started, successful, failed, cancelled, capacity-
rejected, closed-rejected, waiting, and timed-out submission totals. Counters
saturate instead of wrapping in long-running processes.

Blocking submission from a scheduler worker is unsupported because it can
create self-inflicted backpressure. Frontends should use nonblocking admission
from worker callbacks and leave bounded waits to runtime or management threads.
