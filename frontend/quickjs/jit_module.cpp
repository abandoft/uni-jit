#include "unijit_quickjs.h"

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

#include "source_translator.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/tiering.h"

namespace {

using unijit::frontend::quickjs::TranslationResult;
using unijit::frontend::quickjs::ResultKind;
using unijit::ir::Word;

constexpr std::size_t kMaximumParameters = 64;
constexpr char kCompiledFunctionStateProperty[] =
    "__unijit_compiled_function_state__";
constexpr std::uint64_t kQuickJsBaselineFingerprint =
    0x554A515342415345ULL;
constexpr std::uint64_t kQuickJsOptimizedFingerprint =
    0x554A51534F505449ULL;
constexpr unijit::jit::TieringThresholds kQuickJsTieringThresholds{
    64, std::numeric_limits<std::uint64_t>::max(), 64};
constexpr std::size_t kQuickJsSchedulerBytes = 8U * 1024U * 1024U;
JSClassID compiled_function_class_id = JS_INVALID_CLASS_ID;
std::once_flag compiled_function_class_once;

struct CompiledFunctionState final {
  CompiledFunctionState(std::uint64_t id, std::size_t parameters,
                        ResultKind result, std::string retained_source,
                        bool supports_tiering)
      : parameter_count(parameters),
        result_kind(result),
        source(std::move(retained_source)),
        tierable(supports_tiering),
        scheduling_identity("quickjs:" + std::to_string(id)),
        code(kQuickJsTieringThresholds) {}

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

struct QuickJsService final {
  QuickJsService() {
    unijit::jit::CompilationSchedulerOptions options;
    options.worker_count = 1;
    options.maximum_queued_tasks = 64;
    options.maximum_queued_bytes = kQuickJsSchedulerBytes;
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

QuickJsService& quickjs_service() {
  static QuickJsService service;
  return service;
}

void finalize_compiled_function(JSRuntime*, JSValue value) {
  auto* owned = static_cast<OwnedFunction*>(
      JS_GetOpaque(value, compiled_function_class_id));
  if (owned != nullptr && owned->state != nullptr) {
    (void)owned->state->current_ticket().cancel();
  }
  delete owned;
}

bool ensure_compiled_function_class(JSContext* context) {
  std::call_once(compiled_function_class_once, [] {
    JS_NewClassID(&compiled_function_class_id);
  });
  JSRuntime* runtime = JS_GetRuntime(context);
  if (JS_IsRegisteredClass(runtime, compiled_function_class_id) != 0) {
    return true;
  }
  JSClassDef definition{};
  definition.class_name = "UniJITCompiledFunction";
  definition.finalizer = finalize_compiled_function;
  return JS_NewClass(runtime, compiled_function_class_id, &definition) == 0;
}

unijit::Status cancelled_compilation_status() {
  return {unijit::StatusCode::kCancelled,
          "QuickJS optimization was cancelled"};
}

unijit::Status fail_optimization(
    const std::shared_ptr<CompiledFunctionState>& state,
    unijit::Status status) {
  (void)state->code.report_optimization_failure();
  return status;
}

unijit::Status compile_optimized(
    QuickJsService* service,
    const std::shared_ptr<CompiledFunctionState>& state,
    std::uint64_t generation,
    const unijit::jit::CompilationCancellation& cancellation) {
  if (cancellation.stop_requested()) {
    return fail_optimization(state, cancelled_compilation_status());
  }

  unijit::jit::CodeHandle optimized = service->optimized_cache.find(
      state->source, kQuickJsOptimizedFingerprint);
  if (!optimized.valid()) {
    TranslationResult translation =
        unijit::frontend::quickjs::translate_numeric_function(
            state->source, unijit::jit::OptimizationLevel::kOptimized);
    if (!translation.ok()) {
      return fail_optimization(state, std::move(translation.status));
    }
    if (translation.parameter_count != state->parameter_count ||
        translation.result_kind != state->result_kind) {
      return fail_optimization(
          state, {unijit::StatusCode::kCodeGenerationFailed,
                  "QuickJS optimized signature differs from its baseline"});
    }
    if (cancellation.stop_requested()) {
      return fail_optimization(state, cancelled_compilation_status());
    }
    unijit::jit::CodeCachePublication publication =
        service->optimized_cache.publish(
            state->source, kQuickJsOptimizedFingerprint,
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
                "cached QuickJS optimized signature differs from baseline"});
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
    const std::shared_ptr<CompiledFunctionState>& state) noexcept {
  if (!state->tierable || !state->code.try_begin_optimization()) {
    return;
  }
  const unijit::jit::TieredCodeSnapshot baseline = state->code.snapshot();
  if (baseline.tier != unijit::jit::CodeTier::kBaseline) {
    (void)state->code.report_optimization_failure();
    return;
  }

  try {
    QuickJsService& service = quickjs_service();
    if (service.scheduler == nullptr) {
      (void)state->code.report_optimization_failure();
      return;
    }
    unijit::jit::CompilationRequest request;
    request.identity = state->scheduling_identity;
    request.generation = baseline.generation;
    request.estimated_bytes = state->source.size() + 4096U;
    request.priority = unijit::jit::CompilationPriority::kNormal;
    request.job = [&service, state, generation = baseline.generation](
                      const unijit::jit::CompilationCancellation&
                          cancellation) {
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

JSValue invoke_compiled_function(JSContext* context, JSValueConst, int argc,
                                 JSValueConst* arguments, int,
                                 JSValue* function_data) {
  auto* owned = static_cast<OwnedFunction*>(
      JS_GetOpaque(function_data[0], compiled_function_class_id));
  if (owned == nullptr || owned->state == nullptr ||
      !owned->state->code.snapshot().valid()) {
    return JS_ThrowTypeError(context, "invalid UniJIT compiled function");
  }
  const std::shared_ptr<CompiledFunctionState> state = owned->state;
  if (argc < static_cast<int>(state->parameter_count)) {
    return JS_ThrowTypeError(
        context, "compiled function requires %llu arguments",
        static_cast<unsigned long long>(state->parameter_count));
  }

  std::array<Word, kMaximumParameters> native_arguments{};
  for (std::size_t index = 0; index < state->parameter_count; ++index) {
    if (JS_IsNumber(arguments[index]) == 0) {
      return JS_ThrowTypeError(
          context, "argument %llu must be a Number",
          static_cast<unsigned long long>(index + 1));
    }
    double number = 0.0;
    if (JS_ToFloat64(context, &number, arguments[index]) != 0) {
      return JS_EXCEPTION;
    }
    native_arguments[index] = unijit::ir::pack_float64(number);
  }

  unijit::runtime::ExecutionContext execution_context;
  const unijit::jit::TieredInvocationResult invocation = state->code.invoke(
      native_arguments.data(), state->parameter_count, &execution_context);
  if (!invocation.ok()) {
    return JS_ThrowInternalError(
        context, "UniJIT invocation failed at site %llu: %s",
        static_cast<unsigned long long>(
            invocation.result.status.location()),
        invocation.result.status.message().c_str());
  }
  schedule_if_hot(state);
  if (state->result_kind == ResultKind::kBoolean) {
    return JS_NewBool(context, invocation.result.value != 0);
  }
  return JS_NewFloat64(context,
                       unijit::ir::unpack_float64(invocation.result.value));
}

JSValue get_function_to_string(JSContext* context) {
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) {
    return global;
  }
  JSValue constructor = JS_GetPropertyStr(context, global, "Function");
  JS_FreeValue(context, global);
  if (JS_IsException(constructor)) {
    return constructor;
  }
  JSValue prototype = JS_GetPropertyStr(context, constructor, "prototype");
  JS_FreeValue(context, constructor);
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue to_string = JS_GetPropertyStr(context, prototype, "toString");
  JS_FreeValue(context, prototype);
  if (JS_IsException(to_string)) {
    return to_string;
  }
  if (JS_IsFunction(context, to_string) == 0) {
    JS_FreeValue(context, to_string);
    return JS_ThrowTypeError(context,
                             "Function.prototype.toString is not callable");
  }
  return to_string;
}

JSValue compile_with_to_string(JSContext* context, JSValueConst function_value,
                               JSValueConst to_string) {
  if (JS_IsFunction(context, function_value) == 0) {
    return JS_ThrowTypeError(context, "unijit.compile expects a function");
  }

  JSValue source_value =
      JS_Call(context, to_string, function_value, 0, nullptr);
  if (JS_IsException(source_value)) {
    return source_value;
  }

  std::size_t source_size = 0;
  const char* source = JS_ToCStringLen(context, &source_size, source_value);
  JS_FreeValue(context, source_value);
  if (source == nullptr) {
    return JS_EXCEPTION;
  }

  std::string retained_source;
  try {
    retained_source.assign(source, source_size);
  } catch (const std::bad_alloc&) {
    JS_FreeCString(context, source);
    return JS_ThrowOutOfMemory(context);
  }
  JS_FreeCString(context, source);

  try {
    const std::string_view source_view(retained_source);
    const bool tierable =
        unijit::frontend::quickjs::supports_tiered_translation(source_view);
    QuickJsService& service = quickjs_service();
    unijit::jit::CodeHandle baseline = service.baseline_cache.find(
        source_view, kQuickJsBaselineFingerprint);
    std::size_t parameter_count = baseline.parameter_count();
    ResultKind result_kind =
        baseline.valid() &&
                baseline.return_type() == unijit::ir::ValueType::kWord
            ? ResultKind::kBoolean
            : ResultKind::kFloat64;
    if (!baseline.valid()) {
      TranslationResult translation =
          unijit::frontend::quickjs::translate_numeric_function(
              source_view,
              tierable ? unijit::jit::OptimizationLevel::kBaseline
                       : unijit::jit::OptimizationLevel::kOptimized);
      if (!translation.ok()) {
        return JS_ThrowTypeError(
            context, "UniJIT rejected source at byte %llu: %s",
            static_cast<unsigned long long>(translation.status.location()),
            translation.status.message().c_str());
      }
      parameter_count = translation.parameter_count;
      result_kind = translation.result_kind;
      unijit::jit::CodeCachePublication publication =
          service.baseline_cache.publish(
              source_view, kQuickJsBaselineFingerprint,
              std::move(translation.function));
      if (!publication.ok()) {
        return JS_ThrowInternalError(
            context, "UniJIT cache publication failed: %s",
            publication.status.message().c_str());
      }
      baseline = std::move(publication.handle);
    }
    if (parameter_count > kMaximumParameters) {
      return JS_ThrowTypeError(context,
                               "compiled function has too many arguments");
    }
    if (!ensure_compiled_function_class(context)) {
      return JS_ThrowOutOfMemory(context);
    }

    auto state = std::make_shared<CompiledFunctionState>(
        service.allocate_identity(), parameter_count, result_kind,
        std::move(retained_source), tierable);
    const unijit::Status baseline_status =
        state->code.publish_baseline(std::move(baseline));
    if (!baseline_status.ok()) {
      return JS_ThrowInternalError(
          context, "UniJIT baseline publication failed: %s",
          baseline_status.message().c_str());
    }

    JSValue holder = JS_NewObjectClass(
        context, static_cast<int>(compiled_function_class_id));
    if (JS_IsException(holder)) {
      return holder;
    }
    auto* owned = new (std::nothrow) OwnedFunction{std::move(state)};
    if (owned == nullptr) {
      JS_FreeValue(context, holder);
      return JS_ThrowOutOfMemory(context);
    }
    JS_SetOpaque(holder, owned);

    JSValue data[] = {holder};
    JSValue compiled = JS_NewCFunctionData(
        context, invoke_compiled_function,
        static_cast<int>(parameter_count), 0, 1, data);
    if (!JS_IsException(compiled) &&
        JS_DefinePropertyValueStr(
            context, compiled, kCompiledFunctionStateProperty,
            JS_DupValue(context, holder), 0) < 0) {
      JS_FreeValue(context, compiled);
      compiled = JS_EXCEPTION;
    }
    JS_FreeValue(context, holder);
    return compiled;
  } catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(context);
  } catch (...) {
    return JS_ThrowInternalError(
        context, "unexpected UniJIT QuickJS compilation failure");
  }
}

JSValue compile_from_javascript(JSContext* context, JSValueConst, int argc,
                                JSValueConst* arguments, int,
                                JSValue* function_data) {
  if (argc < 1) {
    return JS_ThrowTypeError(context, "unijit.compile expects a function");
  }
  return compile_with_to_string(context, arguments[0], function_data[0]);
}

bool get_compiled_state(
    JSContext* context, JSValueConst value,
    std::shared_ptr<CompiledFunctionState>* state) {
  if (JS_IsFunction(context, value) == 0) {
    JS_ThrowTypeError(context,
                      "expected a function returned by unijit.compile");
    return false;
  }
  JSValue holder =
      JS_GetPropertyStr(context, value, kCompiledFunctionStateProperty);
  if (JS_IsException(holder)) {
    return false;
  }
  auto* owned = static_cast<OwnedFunction*>(
      JS_GetOpaque(holder, compiled_function_class_id));
  if (owned == nullptr || owned->state == nullptr) {
    JS_FreeValue(context, holder);
    JS_ThrowTypeError(context,
                      "expected a function returned by unijit.compile");
    return false;
  }
  *state = owned->state;
  JS_FreeValue(context, holder);
  return true;
}

const char* tier_name(unijit::jit::CodeTier tier) noexcept {
  switch (tier) {
    case unijit::jit::CodeTier::kBaseline:
      return "baseline";
    case unijit::jit::CodeTier::kOptimized:
      return "optimized";
    default:
      return "none";
  }
}

const char* task_state_name(
    unijit::jit::CompilationTaskState state) noexcept {
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

std::int64_t metric_value(std::uint64_t value) noexcept {
  constexpr auto kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return static_cast<std::int64_t>(value > kMaximum ? kMaximum : value);
}

bool set_property(JSContext* context, JSValueConst object, const char* name,
                  JSValue value) {
  if (JS_IsException(value)) {
    return false;
  }
  return JS_SetPropertyStr(context, object, name, value) >= 0;
}

bool set_metric(JSContext* context, JSValueConst object, const char* name,
                std::uint64_t value) {
  return set_property(context, object, name,
                      JS_NewInt64(context, metric_value(value)));
}

bool set_flag(JSContext* context, JSValueConst object, const char* name,
              bool value) {
  return set_property(context, object, name, JS_NewBool(context, value));
}

bool set_text(JSContext* context, JSValueConst object, const char* name,
              const char* value) {
  return set_property(context, object, name, JS_NewString(context, value));
}

JSValue compiled_function_stats(JSContext* context, JSValueConst, int argc,
                                JSValueConst* arguments) {
  if (argc != 1) {
    return JS_ThrowTypeError(context,
                             "unijit.stats expects one compiled function");
  }
  std::shared_ptr<CompiledFunctionState> state;
  if (!get_compiled_state(context, arguments[0], &state)) {
    return JS_EXCEPTION;
  }

  const unijit::jit::TieredCodeStats stats = state->code.stats();
  const unijit::jit::TieredCodeSnapshot snapshot = state->code.snapshot();
  const unijit::jit::CompilationStats* compilation =
      snapshot.handle.compilation_stats();
  const unijit::jit::CompilationTicket ticket = state->current_ticket();
  QuickJsService& service = quickjs_service();
  const unijit::jit::CompilationSchedulerStats scheduler_stats =
      service.scheduler == nullptr
          ? unijit::jit::CompilationSchedulerStats{}
          : service.scheduler->stats();

  JSValue result = JS_NewObject(context);
  if (JS_IsException(result)) {
    return result;
  }
  const bool populated =
      set_text(context, result, "active_tier", tier_name(stats.active_tier)) &&
      set_flag(context, result, "tierable", state->tierable) &&
      set_metric(context, result, "generation", stats.generation) &&
      set_metric(context, result, "invocations", stats.hotness.invocations) &&
      set_metric(context, result, "compilation_attempts",
                 stats.hotness.compilation_attempts) &&
      set_metric(context, result, "successful_compilations",
                 stats.hotness.successful_compilations) &&
      set_metric(context, result, "failed_compilations",
                 stats.hotness.failed_compilations) &&
      set_metric(context, result, "promotions", stats.promotions) &&
      set_metric(context, result, "withdrawals", stats.withdrawals) &&
      set_metric(context, result, "osr_attempts", stats.osr_attempts) &&
      set_metric(context, result, "osr_entries", stats.osr_entries) &&
      set_metric(context, result, "osr_exits", stats.osr_exits) &&
      set_text(context, result, "compilation_state",
               task_state_name(ticket.state())) &&
      set_flag(context, result, "cancellation_requested",
               ticket.cancellation_requested()) &&
      set_flag(context, result, "scheduler_available",
               service.scheduler != nullptr) &&
      set_metric(context, result, "scheduler_queued_tasks",
                 scheduler_stats.queued_tasks) &&
      set_metric(context, result, "scheduler_active_workers",
                 scheduler_stats.active_workers) &&
      set_metric(context, result, "code_size",
                 compilation == nullptr ? 0 : compilation->code_size) &&
      set_metric(context, result, "input_ir_nodes",
                 compilation == nullptr ? 0
                                        : compilation->input_ir_nodes) &&
      set_metric(context, result, "active_ir_nodes",
                 compilation == nullptr ? 0
                                        : compilation->optimized_ir_nodes);
  if (!populated) {
    JS_FreeValue(context, result);
    return JS_EXCEPTION;
  }
  return result;
}

JSValue wait_for_compiled_function(JSContext* context, JSValueConst, int argc,
                                   JSValueConst* arguments) {
  if (argc != 2) {
    return JS_ThrowTypeError(
        context, "unijit.wait expects a compiled function and timeout");
  }
  std::shared_ptr<CompiledFunctionState> state;
  if (!get_compiled_state(context, arguments[0], &state)) {
    return JS_EXCEPTION;
  }
  std::int64_t timeout = 0;
  if (JS_ToInt64(context, &timeout, arguments[1]) != 0) {
    return JS_EXCEPTION;
  }
  if (timeout < 0) {
    return JS_ThrowRangeError(context,
                              "unijit.wait timeout cannot be negative");
  }
  const unijit::jit::CompilationTicket ticket = state->current_ticket();
  const bool completed =
      !ticket.valid() || ticket.wait_for(std::chrono::milliseconds(timeout));
  return JS_NewBool(context, completed);
}

JSValue cancel_compiled_function(JSContext* context, JSValueConst, int argc,
                                 JSValueConst* arguments) {
  if (argc != 1) {
    return JS_ThrowTypeError(context,
                             "unijit.cancel expects one compiled function");
  }
  std::shared_ptr<CompiledFunctionState> state;
  if (!get_compiled_state(context, arguments[0], &state)) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(context, state->current_ticket().cancel());
}

}  // namespace

extern "C" JSValue unijit_quickjs_compile(JSContext* context,
                                           JSValueConst function_value) {
  if (context == nullptr) {
    return JS_EXCEPTION;
  }
  JSValue to_string = get_function_to_string(context);
  if (JS_IsException(to_string)) {
    return to_string;
  }
  JSValue result =
      compile_with_to_string(context, function_value, to_string);
  JS_FreeValue(context, to_string);
  return result;
}

extern "C" int unijit_quickjs_install(JSContext* context) {
  if (context == nullptr) {
    return -1;
  }
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) {
    return -1;
  }
  JSValue module = JS_NewObject(context);
  if (JS_IsException(module)) {
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue to_string = get_function_to_string(context);
  if (JS_IsException(to_string)) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue data[] = {to_string};
  JSValue compile = JS_NewCFunctionData(context, compile_from_javascript, 1, 0,
                                        1, data);
  JS_FreeValue(context, to_string);
  if (JS_IsException(compile)) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  if (JS_SetPropertyStr(context, module, "compile", compile) < 0) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue stats =
      JS_NewCFunction(context, compiled_function_stats, "stats", 1);
  if (JS_IsException(stats) ||
      JS_SetPropertyStr(context, module, "stats", stats) < 0) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue wait =
      JS_NewCFunction(context, wait_for_compiled_function, "wait", 2);
  if (JS_IsException(wait) ||
      JS_SetPropertyStr(context, module, "wait", wait) < 0) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue cancel =
      JS_NewCFunction(context, cancel_compiled_function, "cancel", 1);
  if (JS_IsException(cancel) ||
      JS_SetPropertyStr(context, module, "cancel", cancel) < 0) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  if (JS_SetPropertyStr(context, global, "unijit", module) < 0) {
    JS_FreeValue(context, global);
    return -1;
  }
  JS_FreeValue(context, global);
  return 0;
}
