#include "unijit_pocketpy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <pocketpy.h>

#include "source_translator.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/tiering.h"

namespace {

using unijit::frontend::pocketpy::TranslationResult;
using unijit::frontend::pocketpy::ResultKind;
using unijit::ir::Word;

constexpr char kModuleName[] = "unijit";
constexpr char kCompiledFunctionTypeName[] = "_CompiledFunction";
constexpr std::size_t kMaximumParameters = 64;
constexpr std::size_t kMaximumSourceBytes = 1024U * 1024U;
constexpr std::uint64_t kPocketPyBaselineFingerprint =
    0x554A504B42415345ULL;
constexpr std::uint64_t kPocketPyOptimizedFingerprint =
    0x554A504B4F505449ULL;
constexpr unijit::jit::TieringThresholds kPocketPyTieringThresholds{
    64, 10000, 64};
constexpr std::size_t kPocketPySchedulerBytes = 8U * 1024U * 1024U;

struct CompiledFunctionState final {
  CompiledFunctionState(std::uint64_t id, std::size_t parameters,
                        ResultKind result, std::string retained_source,
                        bool supports_tiering)
      : parameter_count(parameters), result_kind(result),
        source(std::move(retained_source)),
        tierable(supports_tiering),
        scheduling_identity("pocketpy:" + std::to_string(id)),
        code(kPocketPyTieringThresholds) {}

  void retain_ticket(unijit::jit::CompilationTicket retained) {
    std::lock_guard<std::mutex> lock(ticket_mutex);
    ticket = std::move(retained);
  }

  unijit::jit::CompilationTicket current_ticket() const {
    std::lock_guard<std::mutex> lock(ticket_mutex);
    return ticket;
  }

  std::size_t parameter_count;
  ResultKind result_kind;
  std::string source;
  bool tierable;
  std::string scheduling_identity;
  unijit::jit::TieredCode code;
  mutable std::mutex ticket_mutex;
  unijit::jit::CompilationTicket ticket;
};

struct OwnedFunction final {
  std::shared_ptr<CompiledFunctionState> state;
};

struct PocketPyService final {
  PocketPyService() {
    unijit::jit::CompilationSchedulerOptions options;
    options.worker_count = 1;
    options.maximum_queued_tasks = 64;
    options.maximum_queued_bytes = kPocketPySchedulerBytes;
    unijit::jit::CompilationSchedulerCreation creation =
        unijit::jit::CompilationScheduler::create(options);
    scheduler_status = std::move(creation.status);
    scheduler = std::move(creation.scheduler);
  }

  std::uint64_t allocate_identity() noexcept {
    std::uint64_t current =
        next_identity.fetch_add(1, std::memory_order_relaxed);
    if (current == 0) {
      current = next_identity.fetch_add(1, std::memory_order_relaxed);
    }
    return current;
  }

  unijit::jit::CodeCache baseline_cache;
  unijit::jit::CodeCache optimized_cache;
  unijit::Status scheduler_status;
  std::unique_ptr<unijit::jit::CompilationScheduler> scheduler;
  std::atomic<std::uint64_t> next_identity{1};
};

PocketPyService &pocketpy_service() {
  static PocketPyService service;
  return service;
}

static_assert(alignof(OwnedFunction) <= alignof(std::uint64_t),
              "PocketPy userdata must provide sufficient alignment");

py_Type compiled_function_type() {
  return py_gettype(kModuleName, py_name(kCompiledFunctionTypeName));
}

void destroy_compiled_function(void *userdata) {
  auto *owned = static_cast<OwnedFunction *>(userdata);
  if (owned->state != nullptr) {
    (void)owned->state->current_ticket().cancel();
  }
  owned->~OwnedFunction();
}

bool reject_direct_construction(int, py_Ref) {
  return TypeError("UniJIT compiled functions cannot be constructed directly");
}

unijit::Status cancelled_compilation_status() {
  return {unijit::StatusCode::kCancelled,
          "PocketPy optimization was cancelled"};
}

unijit::Status
fail_optimization(const std::shared_ptr<CompiledFunctionState> &state,
                  unijit::Status status) {
  (void)state->code.report_optimization_failure();
  return status;
}

unijit::Status
compile_optimized(PocketPyService *service,
                  const std::shared_ptr<CompiledFunctionState> &state,
                  std::uint64_t generation,
                  const unijit::jit::CompilationCancellation &cancellation) {
  if (cancellation.stop_requested()) {
    return fail_optimization(state, cancelled_compilation_status());
  }

  unijit::jit::CodeHandle optimized = service->optimized_cache.find(
      state->source, kPocketPyOptimizedFingerprint);
  if (!optimized.valid()) {
    TranslationResult translation =
        unijit::frontend::pocketpy::translate_numeric_function(
            state->source, unijit::jit::OptimizationLevel::kOptimized);
    if (!translation.ok()) {
      return fail_optimization(state, std::move(translation.status));
    }
    if (translation.parameter_count != state->parameter_count ||
        translation.result_kind != state->result_kind) {
      return fail_optimization(
          state, {unijit::StatusCode::kCodeGenerationFailed,
                  "PocketPy optimized signature differs from its baseline"});
    }
    if (cancellation.stop_requested()) {
      return fail_optimization(state, cancelled_compilation_status());
    }
    unijit::jit::CodeCachePublication publication =
        service->optimized_cache.publish(state->source,
                                         kPocketPyOptimizedFingerprint,
                                         std::move(translation.function));
    if (!publication.ok()) {
      return fail_optimization(state, std::move(publication.status));
    }
    optimized = std::move(publication.handle);
  }

  const unijit::ir::ValueType expected_return_type =
      state->result_kind == ResultKind::kBoolean
          ? unijit::ir::ValueType::kWord
          : unijit::ir::ValueType::kFloat64;
  if (optimized.parameter_count() != state->parameter_count ||
      optimized.return_type() != expected_return_type) {
    return fail_optimization(
        state, {unijit::StatusCode::kCodeGenerationFailed,
                "cached PocketPy optimized signature differs from baseline"});
  }
  if (cancellation.stop_requested()) {
    return fail_optimization(state, cancelled_compilation_status());
  }
  const unijit::Status promotion =
      state->code.publish_optimized(std::move(optimized), generation);
  if (!promotion.ok()) {
    return fail_optimization(state, promotion);
  }
  return unijit::Status::ok_status();
}

void schedule_if_hot(
    const std::shared_ptr<CompiledFunctionState> &state) noexcept {
  if (!state->tierable || !state->code.try_begin_optimization()) {
    return;
  }
  const unijit::jit::TieredCodeSnapshot baseline = state->code.snapshot();
  if (baseline.tier != unijit::jit::CodeTier::kBaseline) {
    (void)state->code.report_optimization_failure();
    return;
  }

  try {
    PocketPyService &service = pocketpy_service();
    if (service.scheduler == nullptr) {
      (void)state->code.report_optimization_failure();
      return;
    }
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    const std::size_t estimated_bytes = state->source.size() > maximum - 4096U
                                            ? maximum
                                            : state->source.size() + 4096U;
    unijit::jit::CompilationRequest request;
    request.identity = state->scheduling_identity;
    request.generation = baseline.generation;
    request.estimated_bytes = estimated_bytes;
    request.priority = unijit::jit::CompilationPriority::kNormal;
    request.job =
        [&service, state, generation = baseline.generation](
            const unijit::jit::CompilationCancellation &cancellation) {
          return compile_optimized(&service, state, generation, cancellation);
        };
    unijit::jit::CompilationSubmission submission =
        service.scheduler->try_submit(std::move(request));
    if (!submission.ok()) {
      (void)state->code.report_optimization_failure();
      return;
    }
    state->retain_ticket(std::move(submission.ticket));
  } catch (...) {
    (void)state->code.report_optimization_failure();
  }
}

bool create_compiled_function(std::string_view source) {
  if (source.size() > kMaximumSourceBytes) {
    return ValueError("unijit.compile source exceeds %d bytes",
                      static_cast<int>(kMaximumSourceBytes));
  }
  try {
    PocketPyService &service = pocketpy_service();
    const bool tierable =
        unijit::frontend::pocketpy::supports_tiered_translation(source);
    unijit::jit::CodeHandle baseline =
        service.baseline_cache.find(source, kPocketPyBaselineFingerprint);
    std::size_t parameter_count = baseline.parameter_count();
    ResultKind result_kind =
        baseline.valid() &&
                baseline.return_type() == unijit::ir::ValueType::kWord
            ? ResultKind::kBoolean
            : ResultKind::kFloat64;
    if (!baseline.valid()) {
      TranslationResult translation =
          unijit::frontend::pocketpy::translate_numeric_function(
              source, tierable ? unijit::jit::OptimizationLevel::kBaseline
                               : unijit::jit::OptimizationLevel::kOptimized);
      if (!translation.ok()) {
        return ValueError("UniJIT rejected source at byte %d: %s",
                          static_cast<int>(translation.status.location()),
                          translation.status.message().c_str());
      }
      parameter_count = translation.parameter_count;
      result_kind = translation.result_kind;
      unijit::jit::CodeCachePublication publication =
          service.baseline_cache.publish(source, kPocketPyBaselineFingerprint,
                                         std::move(translation.function));
      if (!publication.ok()) {
        return RuntimeError("UniJIT cache publication failed: %s",
                            publication.status.message().c_str());
      }
      baseline = std::move(publication.handle);
    }
    if (parameter_count > kMaximumParameters) {
      return ValueError("compiled function has too many arguments");
    }

    auto state = std::make_shared<CompiledFunctionState>(
        service.allocate_identity(), parameter_count, result_kind,
        std::string(source), tierable);
    const unijit::Status baseline_status =
        state->code.publish_baseline(std::move(baseline));
    if (!baseline_status.ok()) {
      return RuntimeError("UniJIT baseline publication failed: %s",
                          baseline_status.message().c_str());
    }

    const py_Type type = compiled_function_type();
    if (type == 0) {
      return RuntimeError("UniJIT is not installed in the current VM");
    }
    void *storage = py_newobject(py_retval(), type, 0, sizeof(OwnedFunction));
    ::new (storage) OwnedFunction{std::move(state)};
    return true;
  } catch (const std::bad_alloc &) {
    return RuntimeError("unable to allocate UniJIT compiled-function state");
  }
}

bool invoke_compiled_function(int argc, py_Ref argv) {
  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  if (argc < 1) {
    return TypeError("compiled function call is missing self");
  }
  if (!py_checktype(py_arg(0), type)) {
    return false;
  }
  auto *owned = static_cast<OwnedFunction *>(py_touserdata(py_arg(0)));
  if (owned->state == nullptr || !owned->state->code.snapshot().valid()) {
    return RuntimeError("invalid UniJIT compiled function");
  }
  const std::shared_ptr<CompiledFunctionState> state = owned->state;

  const int argument_count = argc - 1;
  if (argument_count != static_cast<int>(state->parameter_count)) {
    return TypeError("compiled function expects %d arguments, got %d",
                     static_cast<int>(state->parameter_count), argument_count);
  }

  std::array<Word, kMaximumParameters> native_arguments{};
  for (std::size_t index = 0; index < state->parameter_count; ++index) {
    double number = 0.0;
    if (!py_castfloat(py_arg(static_cast<int>(index) + 1), &number)) {
      return false;
    }
    native_arguments[index] = unijit::ir::pack_float64(number);
  }

  unijit::runtime::ExecutionContext context;
  const unijit::jit::TieredInvocationResult invocation = state->code.invoke(
      native_arguments.data(), state->parameter_count, &context);
  if (!invocation.ok()) {
    if (invocation.result.status.code() ==
        unijit::StatusCode::kRuntimeExit) {
      const std::size_t site = invocation.result.status.location();
      if (invocation.attempted_handle.valid()) {
        const unijit::runtime::ReconstructionResult reconstruction =
            invocation.attempted_handle.reconstruct_deoptimization(
                site, native_arguments.data(), state->parameter_count,
                context);
        if (reconstruction.ok() &&
            reconstruction.frame.reason ==
                unijit::runtime::DeoptimizationReason::kDivisionByZero) {
          const unijit::runtime::RecoveredValue *divisor =
              reconstruction.frame.find(state->parameter_count);
          if (divisor != nullptr &&
              divisor->type == unijit::ir::ValueType::kFloat64 &&
              unijit::ir::unpack_float64(divisor->value) == 0.0) {
            return ZeroDivisionError("float division by zero");
          }
        }
      }
      return RuntimeError(
          "UniJIT could not reconstruct runtime exit at site %d",
          static_cast<int>(site));
    }
    return RuntimeError("UniJIT invocation failed at site %d: %s",
                        static_cast<int>(
                            invocation.result.status.location()),
                        invocation.result.status.message().c_str());
  }
  state->code.record_backedges(context.safepoint_polls());
  schedule_if_hot(state);
  if (state->result_kind == ResultKind::kBoolean) {
    py_newbool(py_retval(), invocation.result.value != 0);
  } else {
    py_newfloat(py_retval(),
                unijit::ir::unpack_float64(invocation.result.value));
  }
  return true;
}

py_i64 metric_value(std::uint64_t value) noexcept {
  constexpr auto kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<py_i64>::max());
  return static_cast<py_i64>(value > kMaximum ? kMaximum : value);
}

bool set_metric(py_Ref dictionary, const char *name, std::uint64_t value) {
  py_Ref temporary = py_pushtmp();
  py_newint(temporary, metric_value(value));
  const bool stored = py_dict_setitem_by_str(dictionary, name, temporary);
  py_pop();
  return stored;
}

bool set_flag(py_Ref dictionary, const char *name, bool value) {
  py_Ref temporary = py_pushtmp();
  py_newbool(temporary, value);
  const bool stored = py_dict_setitem_by_str(dictionary, name, temporary);
  py_pop();
  return stored;
}

bool set_text(py_Ref dictionary, const char *name, const char *value) {
  py_Ref temporary = py_pushtmp();
  py_newstr(temporary, value);
  const bool stored = py_dict_setitem_by_str(dictionary, name, temporary);
  py_pop();
  return stored;
}

const char *tier_name(unijit::jit::CodeTier tier) noexcept {
  switch (tier) {
  case unijit::jit::CodeTier::kBaseline:
    return "baseline";
  case unijit::jit::CodeTier::kOptimized:
    return "optimized";
  default:
    return "none";
  }
}

const char *task_state_name(unijit::jit::CompilationTaskState state) noexcept {
  switch (state) {
  case unijit::jit::CompilationTaskState::kQueued:
    return "queued";
  case unijit::jit::CompilationTaskState::kRunning:
    return "running";
  case unijit::jit::CompilationTaskState::kSucceeded:
    return "succeeded";
  case unijit::jit::CompilationTaskState::kFailed:
    return "failed";
  case unijit::jit::CompilationTaskState::kCancelled:
    return "cancelled";
  default:
    return "idle";
  }
}

bool compiled_function_stats(int argc, py_Ref argv) {
  if (argc != 1) {
    return TypeError("unijit.stats() expects 1 argument, got %d", argc);
  }
  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  if (!py_checktype(py_arg(0), type)) {
    return false;
  }
  const auto *owned =
      static_cast<const OwnedFunction *>(py_touserdata(py_arg(0)));
  if (owned->state == nullptr) {
    return RuntimeError("invalid UniJIT compiled function");
  }
  const std::shared_ptr<CompiledFunctionState> state = owned->state;
  const unijit::jit::TieredCodeStats stats = state->code.stats();
  const unijit::jit::TieredCodeSnapshot snapshot = state->code.snapshot();
  const unijit::jit::CompilationStats *compilation =
      snapshot.handle.compilation_stats();
  const unijit::jit::CompilationTicket ticket = state->current_ticket();
  PocketPyService &service = pocketpy_service();
  const unijit::jit::CompilationSchedulerStats scheduler_stats =
      service.scheduler == nullptr ? unijit::jit::CompilationSchedulerStats{}
                                   : service.scheduler->stats();

  py_newdict(py_retval());
  return set_text(py_retval(), "active_tier", tier_name(stats.active_tier)) &&
         set_flag(py_retval(), "tierable", state->tierable) &&
         set_metric(py_retval(), "generation", stats.generation) &&
         set_metric(py_retval(), "invocations", stats.hotness.invocations) &&
         set_metric(py_retval(), "backedges", stats.hotness.backedges) &&
         set_metric(py_retval(), "compilation_attempts",
                    stats.hotness.compilation_attempts) &&
         set_metric(py_retval(), "successful_compilations",
                    stats.hotness.successful_compilations) &&
         set_metric(py_retval(), "failed_compilations",
                    stats.hotness.failed_compilations) &&
         set_metric(py_retval(), "promotions", stats.promotions) &&
         set_metric(py_retval(), "withdrawals", stats.withdrawals) &&
         set_metric(py_retval(), "osr_attempts", stats.osr_attempts) &&
         set_metric(py_retval(), "osr_entries", stats.osr_entries) &&
         set_metric(py_retval(), "osr_exits", stats.osr_exits) &&
         set_text(py_retval(), "compilation_state",
                  task_state_name(ticket.state())) &&
         set_flag(py_retval(), "cancellation_requested",
                  ticket.cancellation_requested()) &&
         set_flag(py_retval(), "scheduler_available",
                  service.scheduler != nullptr) &&
         set_metric(py_retval(), "scheduler_queued_tasks",
                    scheduler_stats.queued_tasks) &&
         set_metric(py_retval(), "scheduler_active_workers",
                    scheduler_stats.active_workers) &&
         set_metric(py_retval(), "code_size",
                    compilation == nullptr ? 0 : compilation->code_size) &&
         set_metric(py_retval(), "input_ir_nodes",
                    compilation == nullptr ? 0
                                           : compilation->input_ir_nodes) &&
         set_metric(py_retval(), "active_ir_nodes",
                    compilation == nullptr ? 0
                                           : compilation->optimized_ir_nodes);
}

bool wait_for_compiled_function(int argc, py_Ref argv) {
  if (argc != 2) {
    return TypeError("unijit.wait() expects 2 arguments, got %d", argc);
  }
  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  if (!py_checktype(py_arg(0), type) || !py_checkint(py_arg(1))) {
    return false;
  }
  const py_i64 timeout = py_toint(py_arg(1));
  if (timeout < 0) {
    return ValueError("unijit.wait() timeout cannot be negative");
  }
  const auto *owned =
      static_cast<const OwnedFunction *>(py_touserdata(py_arg(0)));
  if (owned->state == nullptr) {
    return RuntimeError("invalid UniJIT compiled function");
  }
  const unijit::jit::CompilationTicket ticket = owned->state->current_ticket();
  const bool completed =
      !ticket.valid() || ticket.wait_for(std::chrono::milliseconds(timeout));
  py_newbool(py_retval(), completed);
  return true;
}

bool cancel_compiled_function(int argc, py_Ref argv) {
  if (argc != 1) {
    return TypeError("unijit.cancel() expects 1 argument, got %d", argc);
  }
  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  if (!py_checktype(py_arg(0), type)) {
    return false;
  }
  const auto *owned =
      static_cast<const OwnedFunction *>(py_touserdata(py_arg(0)));
  if (owned->state == nullptr) {
    return RuntimeError("invalid UniJIT compiled function");
  }
  py_newbool(py_retval(), owned->state->current_ticket().cancel());
  return true;
}

bool compile_from_python(int argc, py_Ref argv) {
  if (argc != 1) {
    return TypeError("unijit.compile() expects 1 argument, got %d", argc);
  }
  if (!py_checkstr(py_arg(0))) {
    return false;
  }
  int source_size = 0;
  const char *source = py_tostrn(py_arg(0), &source_size);
  return create_compiled_function(
      std::string_view(source, static_cast<std::size_t>(source_size)));
}

} // namespace

extern "C" bool unijit_pocketpy_compile(const char *source) {
  if (source == nullptr) {
    return TypeError("unijit_pocketpy_compile() expects non-null source");
  }
  return create_compiled_function(source);
}

extern "C" int unijit_pocketpy_install(void) {
  py_GlobalRef module = py_getmodule(kModuleName);
  if (module != nullptr) {
    const py_Ref compile = py_getdict(module, py_name("compile"));
    const py_Ref stats = py_getdict(module, py_name("stats"));
    const py_Ref wait = py_getdict(module, py_name("wait"));
    const py_Ref cancel = py_getdict(module, py_name("cancel"));
    return compile != nullptr && stats != nullptr && wait != nullptr &&
                   cancel != nullptr && compiled_function_type() != 0
               ? 0
               : -1;
  }

  module = py_newmodule(kModuleName);
  const py_Type type = py_newtype(kCompiledFunctionTypeName, tp_object, module,
                                  destroy_compiled_function);
  py_bindmagic(type, py_name("__new__"), reject_direct_construction);
  py_bindmagic(type, py_name("__call__"), invoke_compiled_function);
  py_tpsetfinal(type);
  py_bindfunc(module, "compile", compile_from_python);
  py_bindfunc(module, "stats", compiled_function_stats);
  py_bindfunc(module, "wait", wait_for_compiled_function);
  py_bindfunc(module, "cancel", cancel_compiled_function);
  return 0;
}
