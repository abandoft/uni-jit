#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#include <quickjs.h>

#include "source_translator.h"
#include "unijit_quickjs.h"
#include "unijit/ir/function.h"

int main() {
  JSRuntime* runtime = JS_NewRuntime();
  if (runtime == nullptr) {
    std::cerr << "unable to create the stock QuickJS runtime\n";
    return EXIT_FAILURE;
  }
  JSContext* context = JS_NewContext(runtime);
  if (context == nullptr) {
    std::cerr << "unable to create the stock QuickJS context\n";
    JS_FreeRuntime(runtime);
    return EXIT_FAILURE;
  }

  constexpr char kSource[] = "(19 + 23)";
  JSValue result = JS_Eval(context, kSource, std::strlen(kSource),
                           "<unijit-quickjs-smoke>", JS_EVAL_TYPE_GLOBAL);
  double number = 0.0;
  const bool passed = !JS_IsException(result) &&
                      JS_ToFloat64(context, &number, result) == 0 &&
                      number == 42.0;
  JS_FreeValue(context, result);

  if (!passed) {
    std::cerr << "stock QuickJS embedding smoke test failed\n";
    return EXIT_FAILURE;
  }

  const auto translation =
      unijit::frontend::quickjs::translate_numeric_function(
          "function affine(a, b) { return (a + 2.5) * (b - -3) / 2; }");
  if (!translation.ok() || translation.parameter_count != 2) {
    std::cerr << "QuickJS numeric source did not compile: "
              << translation.status.message() << '\n';
    return EXIT_FAILURE;
  }
  const auto baseline_translation =
      unijit::frontend::quickjs::translate_numeric_function(
          "function affine(a, b) { return (a + 2.5) * (b - -3) / 2; }",
          unijit::jit::OptimizationLevel::kBaseline);
  if (!baseline_translation.ok() ||
      baseline_translation.parameter_count != translation.parameter_count ||
      baseline_translation.function->stats().optimized_ir_nodes !=
          baseline_translation.function->stats().input_ir_nodes ||
      translation.function->stats().optimized_ir_nodes >
          baseline_translation.function->stats().optimized_ir_nodes) {
    std::cerr << "QuickJS baseline and optimized translations are not distinct\n";
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> arguments = {
      unijit::ir::pack_float64(1.5), unijit::ir::pack_float64(4.0)};
  const auto native = translation.function->invoke(arguments.data(),
                                                    arguments.size());
  if (!native.ok() || unijit::ir::unpack_float64(native.value) != 14.0) {
    std::cerr << "QuickJS numeric source produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char*, 8> kComparisonSources = {
      "function compare(a, b) { return a < b; }",
      "function compare(a, b) { return a <= b; }",
      "function compare(a, b) { return a > b; }",
      "function compare(a, b) { return a >= b; }",
      "function compare(a, b) { return a == b; }",
      "function compare(a, b) { return a != b; }",
      "function compare(a, b) { return a === b; }",
      "function compare(a, b) { return a !== b; }"};
  constexpr std::array<unijit::ir::Word, 8> kEqualComparisonResults = {
      0, 1, 0, 1, 1, 0, 1, 0};
  constexpr std::array<unijit::ir::Word, 8> kUnorderedComparisonResults = {
      0, 0, 0, 0, 0, 1, 0, 1};
  const std::array<unijit::ir::Word, 2> equal_arguments = {
      unijit::ir::pack_float64(2.0), unijit::ir::pack_float64(2.0)};
  const std::array<unijit::ir::Word, 2> unordered_arguments = {
      unijit::ir::pack_float64(
          std::numeric_limits<double>::quiet_NaN()),
      unijit::ir::pack_float64(2.0)};
  for (std::size_t index = 0; index < kComparisonSources.size(); ++index) {
    const auto comparison =
        unijit::frontend::quickjs::translate_numeric_function(
            kComparisonSources[index]);
    const auto equal = comparison.ok()
                           ? comparison.function->invoke(
                                 equal_arguments.data(), equal_arguments.size())
                           : unijit::ir::EvaluationResult{};
    const auto unordered =
        comparison.ok()
            ? comparison.function->invoke(unordered_arguments.data(),
                                          unordered_arguments.size())
            : unijit::ir::EvaluationResult{};
    if (!comparison.ok() ||
        comparison.result_kind !=
            unijit::frontend::quickjs::ResultKind::kBoolean ||
        comparison.function->return_type() !=
            unijit::ir::ValueType::kWord ||
        !equal.ok() || equal.value != kEqualComparisonResults[index] ||
        !unordered.ok() ||
        unordered.value != kUnorderedComparisonResults[index]) {
      std::cerr << "QuickJS numeric comparison semantics were not preserved\n";
      return EXIT_FAILURE;
    }
  }

  constexpr char kNegateSource[] =
      "function negate(value) { return -value; }";
  constexpr char kNegateLoopSource[] =
      "function negateLoop(value) {"
      "  let result = value;"
      "  for (let iteration = 0.0; iteration < 1.0; ++iteration) {"
      "    result = -result;"
      "  }"
      "  return result;"
      "}";
  constexpr std::array<std::uint64_t, 6> kNegateSamples = {
      UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
      UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000005678)};
  for (const auto level : {unijit::jit::OptimizationLevel::kBaseline,
                           unijit::jit::OptimizationLevel::kOptimized}) {
    const auto negate =
        unijit::frontend::quickjs::translate_numeric_function(kNegateSource,
                                                               level);
    const auto negate_loop =
        unijit::frontend::quickjs::translate_numeric_function(
            kNegateLoopSource, level);
    for (const std::uint64_t bits : kNegateSamples) {
      const unijit::ir::Word argument =
          static_cast<unijit::ir::Word>(bits);
      const auto direct = negate.ok()
                              ? negate.function->invoke(&argument, 1)
                              : unijit::ir::EvaluationResult{};
      const auto loop = negate_loop.ok()
                            ? negate_loop.function->invoke(&argument, 1)
                            : unijit::ir::EvaluationResult{};
      if (!direct.ok() || !loop.ok() ||
          static_cast<std::uint64_t>(direct.value) !=
              (bits ^ (UINT64_C(1) << 63U)) ||
          loop.value != direct.value) {
        std::cerr << "QuickJS unary minus did not preserve exact Float64 bits\n";
        return EXIT_FAILURE;
      }
    }
  }

  constexpr char kCountedLoopSource[] =
      "function numericWorkload(count) {"
      "  let lhs = 1.25;"
      "  let rhs = -7.5;"
      "  let checksum = 20.0 + -20.0;"
      "  for (let iteration = 0; iteration < count; ++iteration) {"
      "    checksum += (lhs + rhs) * (lhs - 3.25) + rhs * 0.75;"
      "    lhs += 0.125;"
      "    rhs -= 0.0625;"
      "    if (lhs > 4096.0) { lhs = 1.25; }"
      "    if (rhs < -4096.0) { rhs = -7.5; }"
      "  }"
      "  return checksum;"
      "}";
  const auto counted_loop =
      unijit::frontend::quickjs::translate_numeric_function(
          kCountedLoopSource);
  const auto baseline_counted_loop =
      unijit::frontend::quickjs::translate_numeric_function(
          kCountedLoopSource, unijit::jit::OptimizationLevel::kBaseline);
  if (!counted_loop.ok() || counted_loop.parameter_count != 1 ||
      !counted_loop.function->requires_context() ||
      !baseline_counted_loop.ok() ||
      baseline_counted_loop.function->stats().input_ir_nodes !=
          baseline_counted_loop.function->stats().optimized_ir_nodes ||
      counted_loop.function->stats().optimized_ir_nodes >=
          baseline_counted_loop.function->stats().optimized_ir_nodes ||
      !unijit::frontend::quickjs::supports_tiered_translation(
          kCountedLoopSource)) {
    std::cerr << "QuickJS counted loop did not compile: "
              << counted_loop.status.message() << '\n';
    return EXIT_FAILURE;
  }
  constexpr std::size_t kLoopIterations = 10000;
  double expected_loop = 0.0;
  double expected_lhs = 1.25;
  double expected_rhs = -7.5;
  for (std::size_t iteration = 0; iteration < kLoopIterations; ++iteration) {
    expected_loop += (expected_lhs + expected_rhs) *
                         (expected_lhs - 3.25) +
                     expected_rhs * 0.75;
    expected_lhs += 0.125;
    expected_rhs -= 0.0625;
    if (expected_lhs > 4096.0) {
      expected_lhs = 1.25;
    }
    if (expected_rhs < -4096.0) {
      expected_rhs = -7.5;
    }
  }
  const std::array<unijit::ir::Word, 1> loop_arguments = {
      unijit::ir::pack_float64(static_cast<double>(kLoopIterations))};
  const auto loop_result = counted_loop.function->invoke(
      loop_arguments.data(), loop_arguments.size());
  if (!loop_result.ok() ||
      loop_result.value != unijit::ir::pack_float64(expected_loop)) {
    std::cerr << "QuickJS counted loop produced the wrong native result: "
              << (loop_result.ok()
                      ? unijit::ir::unpack_float64(loop_result.value)
                      : 0.0)
              << " != " << expected_loop << " ("
              << loop_result.status.message() << ")\n";
    return EXIT_FAILURE;
  }

  constexpr char kLoopControlSource[] =
      "function gatedSum(count) {"
      "  let sum = 0.0;"
      "  for (let iteration = 0; iteration < count; iteration++) {"
      "    if (iteration >= 8.0) { break; }"
      "    if (iteration < 3.0) { continue; }"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}";
  const auto loop_control =
      unijit::frontend::quickjs::translate_numeric_function(
          kLoopControlSource);
  const std::array<unijit::ir::Word, 1> loop_control_arguments = {
      unijit::ir::pack_float64(100.0)};
  const auto loop_control_result =
      loop_control.ok()
          ? loop_control.function->invoke(loop_control_arguments.data(),
                                          loop_control_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!loop_control.ok() || !loop_control_result.ok() ||
      loop_control_result.value != unijit::ir::pack_float64(25.0)) {
    std::cerr << "QuickJS break/continue loop semantics were not preserved: "
              << loop_control.status.message() << '\n';
    return EXIT_FAILURE;
  }

  constexpr char kEqualityLoopSource[] =
      "function equalityLoop() {"
      "  let sum = 0.0;"
      "  for (let iteration = 0.0; iteration !== 6.0; ++iteration) {"
      "    if (iteration === 2.0) { continue; }"
      "    if (iteration == 5.0) { break; }"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}";
  const auto equality_loop =
      unijit::frontend::quickjs::translate_numeric_function(
          kEqualityLoopSource);
  const auto equality_loop_result =
      equality_loop.ok() ? equality_loop.function->invoke(nullptr, 0)
                         : unijit::ir::EvaluationResult{};
  if (!equality_loop.ok() || !equality_loop_result.ok() ||
      equality_loop_result.value != unijit::ir::pack_float64(8.0)) {
    std::cerr << "QuickJS equality loop semantics were not preserved: "
              << equality_loop.status.message() << '\n';
    return EXIT_FAILURE;
  }
  constexpr char kRejectedControlElse[] =
      "function rejected(count) {"
      "  let sum = 0.0;"
      "  for (let iteration = 0; iteration < count; iteration++) {"
      "    if (iteration > 2.0) { break; } else { sum += iteration; }"
      "  }"
      "  return sum;"
      "}";
  if (unijit::frontend::quickjs::translate_numeric_function(
          kRejectedControlElse)
          .ok()) {
    std::cerr << "QuickJS accepted a break guard with an else arm\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char*, 4> kStridedLoopSources = {
      "function stepped() {"
      "  let sum = 0.0;"
      "  for (let iteration = 1.0; iteration <= 9.0; iteration += 2.0) {"
      "    if (iteration > 5.0) { break; }"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}",
      "function descending() {"
      "  let sum = 0.0;"
      "  for (let iteration = 10.0; iteration >= 0.0; iteration -= 2.0) {"
      "    if (iteration < 4.0) { continue; }"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}",
      "function decrement() {"
      "  let sum = 0.0;"
      "  for (let iteration = 3.0; iteration > 0.0; iteration--) {"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}",
      "function prefixDecrement() {"
      "  let sum = 0.0;"
      "  for (let iteration = 3.0; iteration > 0.0; --iteration) {"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}"};
  constexpr std::array<double, 4> kStridedLoopResults = {9.0, 28.0, 6.0,
                                                         6.0};
  for (std::size_t index = 0; index < kStridedLoopSources.size(); ++index) {
    const auto strided =
        unijit::frontend::quickjs::translate_numeric_function(
            kStridedLoopSources[index]);
    const auto strided_result =
        strided.ok() ? strided.function->invoke(nullptr, 0)
                     : unijit::ir::EvaluationResult{};
    if (!strided.ok() || !strided_result.ok() ||
        strided_result.value !=
            unijit::ir::pack_float64(kStridedLoopResults[index])) {
      std::cerr << "QuickJS strided loop semantics were not preserved: "
                << strided.status.message() << '\n';
      return EXIT_FAILURE;
    }
  }
  constexpr std::array<const char*, 2> kRejectedLoopSteps = {
      "function zeroStep() {"
      "  let sum = 0.0;"
      "  for (let iteration = 0.0; iteration < 10.0; iteration += 0.0) {"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}",
      "function dynamicStep(step) {"
      "  let sum = 0.0;"
      "  for (let iteration = 0.0; iteration < 10.0; iteration += step) {"
      "    sum += iteration;"
      "  }"
      "  return sum;"
      "}"};
  for (const char* source : kRejectedLoopSteps) {
    if (unijit::frontend::quickjs::translate_numeric_function(source).ok()) {
      std::cerr << "QuickJS accepted an unsupported induction step\n";
      return EXIT_FAILURE;
    }
  }

  constexpr std::array<const char*, 4> kRejectedSources = {
      "function(a, a) { return a; }",
      "function(a) { return external + a; }",
      "function(a) { const b = a; return b; }",
      "function(a, b, c) { return a < b < c; }"};
  for (const char* source : kRejectedSources) {
    if (unijit::frontend::quickjs::translate_numeric_function(source).ok()) {
      std::cerr << "unsupported QuickJS source was accepted: " << source
                << '\n';
      return EXIT_FAILURE;
    }
  }

  if (unijit_quickjs_install(context) != 0) {
    std::cerr << "unable to install the UniJIT QuickJS module\n";
    return EXIT_FAILURE;
  }
  std::string oversized_function = "function oversized(){";
  oversized_function.append(1024U * 1024U + 1U, ' ');
  oversized_function.append("return 1;} unijit.compile(oversized);");
  result = JS_Eval(context, oversized_function.data(),
                   oversized_function.size(), "<unijit-quickjs-budget>",
                   JS_EVAL_TYPE_GLOBAL);
  if (!JS_IsException(result)) {
    std::cerr << "QuickJS accepted source beyond the compilation budget\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);
  JSValue oversized_exception = JS_GetException(context);
  JS_FreeValue(context, oversized_exception);
  constexpr char kNativeSource[] =
      "function sourceFunction(a, b) {"
      "  return (a + 2.5) * (b - -3) / 2;"
      "}"
      "sourceFunction.toString = () => 'function(a, b) { return 999; }';"
      "function compareFunction(a, b) { return (a + 1) >= b * 2; }"
      "function equalityFunction(a, b) { return a !== b; }"
      "function negateFunction(value) { return -value; }"
      "const native = unijit.compile(sourceFunction);"
      "const nativeCached = unijit.compile(sourceFunction);"
      "const nativeCompare = unijit.compile(compareFunction);"
      "const nativeEquality = unijit.compile(equalityFunction);"
      "const nativeNegate = unijit.compile(negateFunction);"
      "native(1.5, 4.0, 99) + nativeCached(1.5, 4.0);";
  result = JS_Eval(context, kNativeSource, std::strlen(kNativeSource),
                   "<unijit-quickjs-native>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 || number != 28.0) {
    std::cerr << "QuickJS did not execute the compiled native closure\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  constexpr char kBooleanSource[] =
      "let comparisonTyped = true;"
      "let negateExact = true;"
      "for (let index = 0; index < 64; ++index) {"
      "  comparisonTyped = comparisonTyped && nativeCompare(3, 2) === true;"
      "  comparisonTyped = comparisonTyped && nativeEquality(3, 2) === true;"
      "  negateExact = negateExact && Object.is(nativeNegate(0), -0);"
      "  negateExact = negateExact && Object.is(nativeNegate(-0), 0);"
      "}"
      "const comparisonWaited = unijit.wait(nativeCompare, 5000);"
      "const equalityWaited = unijit.wait(nativeEquality, 5000);"
      "const negateWaited = unijit.wait(nativeNegate, 5000);"
      "const comparisonStats = unijit.stats(nativeCompare);"
      "const equalityStats = unijit.stats(nativeEquality);"
      "const negateStats = unijit.stats(nativeNegate);"
      "comparisonTyped && negateExact && comparisonWaited && equalityWaited &&"
      "negateWaited &&"
      "nativeCompare(2, 2) === false &&"
      "nativeEquality(0, -0) === false &&"
      "nativeEquality(NaN, NaN) === true &&"
      "typeof nativeCompare(3, 2) === 'boolean' &&"
      "typeof nativeEquality(3, 2) === 'boolean' &&"
      "comparisonStats.active_tier === 'optimized' &&"
      "comparisonStats.promotions === 1 &&"
      "equalityStats.active_tier === 'optimized' &&"
      "equalityStats.promotions === 1 &&"
      "negateStats.active_tier === 'optimized' &&"
      "negateStats.promotions === 1;";
  result = JS_Eval(context, kBooleanSource, std::strlen(kBooleanSource),
                   "<unijit-quickjs-boolean>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result) || JS_ToBool(context, result) != 1) {
    std::cerr << "QuickJS did not retain Boolean results across tiering\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  const std::string equality_loop_call =
      std::string("const nativeEqualityLoop = unijit.compile(") +
      kEqualityLoopSource + "); nativeEqualityLoop();";
  result = JS_Eval(context, equality_loop_call.data(),
                   equality_loop_call.size(),
                   "<unijit-quickjs-equality-loop>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 || number != 8.0) {
    std::cerr << "QuickJS runtime did not execute an equality loop\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  const std::string negate_loop_call =
      std::string("const nativeNegateLoop = unijit.compile(") +
      kNegateLoopSource +
      "); Object.is(nativeNegateLoop(0), -0) && "
      "Object.is(nativeNegateLoop(-0), 0);";
  result = JS_Eval(context, negate_loop_call.data(), negate_loop_call.size(),
                   "<unijit-quickjs-negate-loop>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result) || JS_ToBool(context, result) != 1) {
    std::cerr << "QuickJS runtime did not preserve signed zero in unary loops\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  constexpr char kTieringSource[] =
      "let tieringChecksum = 0.0;"
      "for (let index = 0; index < 64; ++index) {"
      "  tieringChecksum += native(1.5, 4.0);"
      "}"
      "const tieringWaited = unijit.wait(native, 5000);"
      "const tieringStats = unijit.stats(native);"
      "tieringWaited && tieringChecksum === 896.0 &&"
      "tieringStats.tierable === true &&"
      "tieringStats.active_tier === 'optimized' &&"
      "tieringStats.invocations >= 65 &&"
      "tieringStats.backedges === 0 &&"
      "tieringStats.compilation_attempts === 1 &&"
      "tieringStats.successful_compilations === 1 &&"
      "tieringStats.failed_compilations === 0 &&"
      "tieringStats.promotions === 1 &&"
      "tieringStats.compilation_state === 'succeeded' &&"
      "tieringStats.scheduler_available === true &&"
      "tieringStats.code_size > 0 &&"
      "unijit.cancel(native) === false;";
  result = JS_Eval(context, kTieringSource, std::strlen(kTieringSource),
                   "<unijit-quickjs-tiering>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result) || JS_ToBool(context, result) != 1) {
    std::cerr << "QuickJS did not promote a hot callable asynchronously\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  const std::string counted_loop_call =
      std::string("const nativeLoop = unijit.compile(") +
      kCountedLoopSource + "); nativeLoop(10000);";
  result = JS_Eval(context, counted_loop_call.data(), counted_loop_call.size(),
                   "<unijit-quickjs-counted-loop>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 ||
      unijit::ir::pack_float64(number) !=
          unijit::ir::pack_float64(expected_loop)) {
    std::cerr << "QuickJS did not execute the compiled counted-loop closure\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  const std::string control_loop_call =
      std::string("const nativeControlLoop = unijit.compile(") +
      kLoopControlSource + "); nativeControlLoop(100);";
  result = JS_Eval(context, control_loop_call.data(), control_loop_call.size(),
                   "<unijit-quickjs-loop-control>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 || number != 25.0) {
    std::cerr << "QuickJS runtime did not execute loop control guards\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  const std::string strided_loop_call =
      std::string("const nativeStridedLoop = unijit.compile(") +
      kStridedLoopSources[1] + "); nativeStridedLoop();";
  result = JS_Eval(context, strided_loop_call.data(), strided_loop_call.size(),
                   "<unijit-quickjs-strided-loop>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 || number != 28.0) {
    std::cerr << "QuickJS runtime did not execute a strided loop\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  constexpr char kLoopStatsSource[] =
      "const loopWaited = unijit.wait(nativeLoop, 5000);"
      "const loopStats = unijit.stats(nativeLoop);"
      "loopWaited &&"
      "loopStats.tierable === true &&"
      "loopStats.active_tier === 'optimized' &&"
      "loopStats.invocations === 1 &&"
      "loopStats.backedges === 10000 &&"
      "loopStats.compilation_attempts === 1 &&"
      "loopStats.successful_compilations === 1 &&"
      "loopStats.failed_compilations === 0 &&"
      "loopStats.promotions === 1 &&"
      "loopStats.compilation_state === 'succeeded' &&"
      "loopStats.active_ir_nodes < loopStats.input_ir_nodes;";
  result = JS_Eval(context, kLoopStatsSource, std::strlen(kLoopStatsSource),
                   "<unijit-quickjs-loop-stats>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result) || JS_ToBool(context, result) != 1) {
    std::cerr << "QuickJS did not tier a single long counted-loop call\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  constexpr char kGuardSource[] = "native('1.5', 4.0);";
  result = JS_Eval(context, kGuardSource, std::strlen(kGuardSource),
                   "<unijit-quickjs-guard>", JS_EVAL_TYPE_GLOBAL);
  if (!JS_IsException(result)) {
    std::cerr << "QuickJS native closure accepted a non-Number argument\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);
  JSValue exception = JS_GetException(context);
  JS_FreeValue(context, exception);

  JS_RunGC(runtime);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  std::cout << "stock QuickJS embedding smoke test passed\n";
  return EXIT_SUCCESS;
}
