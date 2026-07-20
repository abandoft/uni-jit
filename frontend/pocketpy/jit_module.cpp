#include "unijit_pocketpy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <pocketpy.h>

#include "source_translator.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/tiering.h"

namespace {

using unijit::frontend::pocketpy::TranslationResult;
using unijit::ir::Word;

constexpr char kModuleName[] = "unijit";
constexpr char kCompiledFunctionTypeName[] = "_CompiledFunction";
constexpr std::size_t kMaximumParameters = 64;
constexpr std::uint64_t kPocketPyBaselineFingerprint =
    0x554A504B42415345ULL;
constexpr std::uint64_t kPocketPyOptimizedFingerprint =
    0x554A504B4F505449ULL;
constexpr unijit::jit::TieringThresholds kPocketPyTieringThresholds{
    64, std::numeric_limits<std::uint64_t>::max(), 64};

struct OwnedFunction final {
  OwnedFunction(std::size_t parameters, std::string retained_source,
                bool supports_tiering)
      : parameter_count(parameters),
        source(std::move(retained_source)),
        tierable(supports_tiering),
        code(kPocketPyTieringThresholds) {}

  std::size_t parameter_count;
  std::string source;
  bool tierable;
  unijit::jit::TieredCode code;
};

unijit::jit::CodeCache &baseline_function_cache() {
  static unijit::jit::CodeCache cache;
  return cache;
}

unijit::jit::CodeCache &optimized_function_cache() {
  static unijit::jit::CodeCache cache;
  return cache;
}

static_assert(alignof(OwnedFunction) <= alignof(std::uint64_t),
              "PocketPy userdata must provide sufficient alignment");

py_Type compiled_function_type() {
  return py_gettype(kModuleName, py_name(kCompiledFunctionTypeName));
}

void destroy_compiled_function(void *userdata) {
  auto *owned = static_cast<OwnedFunction *>(userdata);
  owned->~OwnedFunction();
}

bool reject_direct_construction(int, py_Ref) {
  return TypeError("UniJIT compiled functions cannot be constructed directly");
}

void report_failed_optimization(OwnedFunction *owned) {
  (void)owned->code.report_optimization_failure();
}

void promote_if_hot(OwnedFunction *owned) {
  if (!owned->tierable || !owned->code.try_begin_optimization()) {
    return;
  }

  const unijit::jit::TieredCodeSnapshot baseline = owned->code.snapshot();
  if (baseline.tier != unijit::jit::CodeTier::kBaseline) {
    report_failed_optimization(owned);
    return;
  }

  unijit::jit::CodeHandle optimized = optimized_function_cache().find(
      owned->source, kPocketPyOptimizedFingerprint);
  if (!optimized.valid()) {
    TranslationResult translation =
        unijit::frontend::pocketpy::translate_numeric_function(
            owned->source, unijit::jit::OptimizationLevel::kOptimized);
    if (!translation.ok() ||
        translation.parameter_count != owned->parameter_count) {
      report_failed_optimization(owned);
      return;
    }
    unijit::jit::CodeCachePublication publication =
        optimized_function_cache().publish(
            owned->source, kPocketPyOptimizedFingerprint,
            std::move(translation.function));
    if (!publication.ok()) {
      report_failed_optimization(owned);
      return;
    }
    optimized = std::move(publication.handle);
  }

  const unijit::Status promotion = owned->code.publish_optimized(
      std::move(optimized), baseline.generation);
  if (!promotion.ok()) {
    report_failed_optimization(owned);
  }
}

bool create_compiled_function(std::string_view source) {
  try {
    const bool tierable =
        unijit::frontend::pocketpy::supports_tiered_translation(source);
    unijit::jit::CodeHandle baseline = baseline_function_cache().find(
        source, kPocketPyBaselineFingerprint);
    std::size_t parameter_count = baseline.parameter_count();
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
      unijit::jit::CodeCachePublication publication =
          baseline_function_cache().publish(
              source, kPocketPyBaselineFingerprint,
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

    OwnedFunction owned(parameter_count, std::string(source), tierable);
    const unijit::Status baseline_status =
        owned.code.publish_baseline(std::move(baseline));
    if (!baseline_status.ok()) {
      return RuntimeError("UniJIT baseline publication failed: %s",
                          baseline_status.message().c_str());
    }

    const py_Type type = compiled_function_type();
    if (type == 0) {
      return RuntimeError("UniJIT is not installed in the current VM");
    }
    void *storage =
        py_newobject(py_retval(), type, 0, sizeof(OwnedFunction));
    ::new (storage) OwnedFunction(std::move(owned));
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
  if (!owned->code.snapshot().valid()) {
    return RuntimeError("invalid UniJIT compiled function");
  }

  const int argument_count = argc - 1;
  if (argument_count != static_cast<int>(owned->parameter_count)) {
    return TypeError("compiled function expects %d arguments, got %d",
                     static_cast<int>(owned->parameter_count), argument_count);
  }

  std::array<Word, kMaximumParameters> native_arguments{};
  for (std::size_t index = 0; index < owned->parameter_count; ++index) {
    double number = 0.0;
    if (!py_castfloat(py_arg(static_cast<int>(index) + 1), &number)) {
      return false;
    }
    native_arguments[index] = unijit::ir::pack_float64(number);
  }

  unijit::runtime::ExecutionContext context;
  const unijit::jit::TieredInvocationResult invocation = owned->code.invoke(
      native_arguments.data(), owned->parameter_count, &context);
  if (!invocation.ok()) {
    if (invocation.result.status.code() ==
        unijit::StatusCode::kRuntimeExit) {
      const std::size_t site = invocation.result.status.location();
      if (invocation.attempted_handle.valid()) {
        const unijit::runtime::ReconstructionResult reconstruction =
            invocation.attempted_handle.reconstruct_deoptimization(
                site, native_arguments.data(), owned->parameter_count,
                context);
        if (reconstruction.ok() &&
            reconstruction.frame.reason ==
                unijit::runtime::DeoptimizationReason::kDivisionByZero) {
          const unijit::runtime::RecoveredValue *divisor =
              reconstruction.frame.find(owned->parameter_count);
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
  promote_if_hot(owned);
  py_newfloat(py_retval(),
              unijit::ir::unpack_float64(invocation.result.value));
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
  const unijit::jit::TieredCodeStats stats = owned->code.stats();
  const unijit::jit::TieredCodeSnapshot snapshot = owned->code.snapshot();
  const unijit::jit::CompilationStats *compilation =
      snapshot.handle.compilation_stats();

  py_newdict(py_retval());
  return set_text(py_retval(), "active_tier", tier_name(stats.active_tier)) &&
         set_flag(py_retval(), "tierable", owned->tierable) &&
         set_metric(py_retval(), "generation", stats.generation) &&
         set_metric(py_retval(), "invocations", stats.hotness.invocations) &&
         set_metric(py_retval(), "compilation_attempts",
                    stats.hotness.compilation_attempts) &&
         set_metric(py_retval(), "successful_compilations",
                    stats.hotness.successful_compilations) &&
         set_metric(py_retval(), "failed_compilations",
                    stats.hotness.failed_compilations) &&
         set_metric(py_retval(), "promotions", stats.promotions) &&
         set_metric(py_retval(), "withdrawals", stats.withdrawals) &&
         set_metric(py_retval(), "code_size",
                    compilation == nullptr ? 0 : compilation->code_size) &&
         set_metric(py_retval(), "input_ir_nodes",
                    compilation == nullptr ? 0
                                           : compilation->input_ir_nodes) &&
         set_metric(py_retval(), "active_ir_nodes",
                    compilation == nullptr ? 0
                                           : compilation->optimized_ir_nodes);
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
    return compile != nullptr && stats != nullptr &&
                   compiled_function_type() != 0
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
  return 0;
}
