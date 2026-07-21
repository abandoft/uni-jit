#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <pocketpy.h>

#include "source_translator.h"
#include "unijit/ir/function.h"
#include "unijit_pocketpy.h"

int main() {
  const auto translation =
      unijit::frontend::pocketpy::translate_numeric_function(
          "def affine(a, b):\n"
          "    return (a + 2.5) * (b - -3)\n");
  if (!translation.ok() || translation.parameter_count != 2) {
    std::cerr << "PocketPy numeric source did not compile: "
              << translation.status.message() << '\n';
    return EXIT_FAILURE;
  }

  const std::array<unijit::ir::Word, 2> arguments = {
      unijit::ir::pack_float64(1.5), unijit::ir::pack_float64(4.0)};
  const auto native =
      translation.function->invoke(arguments.data(), arguments.size());
  if (!native.ok() || unijit::ir::unpack_float64(native.value) != 28.0) {
    std::cerr << "PocketPy numeric source produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char *, 6> kComparisonSources = {
      "def compare(a, b): return a < b",
      "def compare(a, b): return a <= b",
      "def compare(a, b): return a > b",
      "def compare(a, b): return a >= b",
      "def compare(a, b): return a == b",
      "def compare(a, b): return a != b"};
  constexpr std::array<unijit::ir::Word, 6> kEqualComparisonResults = {
      0, 1, 0, 1, 1, 0};
  constexpr std::array<unijit::ir::Word, 6> kUnorderedComparisonResults = {
      0, 0, 0, 0, 0, 1};
  const std::array<unijit::ir::Word, 2> equal_arguments = {
      unijit::ir::pack_float64(2.0), unijit::ir::pack_float64(2.0)};
  const std::array<unijit::ir::Word, 2> unordered_arguments = {
      unijit::ir::pack_float64(
          std::numeric_limits<double>::quiet_NaN()),
      unijit::ir::pack_float64(2.0)};
  for (std::size_t index = 0; index < kComparisonSources.size(); ++index) {
    const auto comparison =
        unijit::frontend::pocketpy::translate_numeric_function(
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
            unijit::frontend::pocketpy::ResultKind::kBoolean ||
        comparison.function->return_type() !=
            unijit::ir::ValueType::kWord ||
        !equal.ok() || equal.value != kEqualComparisonResults[index] ||
        !unordered.ok() ||
        unordered.value != kUnorderedComparisonResults[index]) {
      std::cerr << "PocketPy numeric comparison semantics were not preserved\n";
      return EXIT_FAILURE;
    }
  }

  constexpr char kNegateSource[] = "def negate(value): return -value";
  constexpr char kNegateLoopSource[] =
      "def negate_loop(value):\n"
      "    result = value\n"
      "    for iteration in range(1.0):\n"
      "        result = -result\n"
      "    return result\n";
  constexpr std::array<std::uint64_t, 4> kNegateSamples = {
      UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
      UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000005678)};
  for (const auto level : {unijit::jit::OptimizationLevel::kBaseline,
                           unijit::jit::OptimizationLevel::kOptimized}) {
    const auto negate =
        unijit::frontend::pocketpy::translate_numeric_function(kNegateSource,
                                                                level);
    const auto negate_loop =
        unijit::frontend::pocketpy::translate_numeric_function(
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
        std::cerr << "PocketPy unary minus did not preserve exact Float64 bits\n";
        return EXIT_FAILURE;
      }
    }
  }

  constexpr char kDivisionSource[] =
      "def quotient(a, b): return (a + 1) / b";
  const auto division =
      unijit::frontend::pocketpy::translate_numeric_function(kDivisionSource);
  if (!division.ok() || !division.function->requires_context()) {
    std::cerr << "PocketPy checked division did not compile\n";
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> division_arguments = {
      unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(2.0)};
  const auto quotient = division.function->invoke(division_arguments.data(),
                                                   division_arguments.size());
  if (!quotient.ok() || unijit::ir::unpack_float64(quotient.value) != 5.0) {
    std::cerr << "PocketPy checked division produced the wrong result\n";
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> zero_division_arguments = {
      unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(-0.0)};
  unijit::runtime::ExecutionContext division_context;
  const auto zero_division = division.function->invoke(
      zero_division_arguments.data(), zero_division_arguments.size(),
      &division_context);
  if (zero_division.ok() ||
      zero_division.status.code() != unijit::StatusCode::kRuntimeExit) {
    std::cerr << "PocketPy checked division accepted a zero divisor\n";
    return EXIT_FAILURE;
  }
  const std::size_t division_site =
      std::string_view(kDivisionSource).find('/');
  const auto *division_record =
      division.function->deoptimization_record(division_site);
  const auto reconstructed_division =
      division.function->reconstruct_deoptimization(
          division_site, zero_division_arguments.data(),
          zero_division_arguments.size(), division_context);
  const auto *recovered_lhs = reconstructed_division.frame.find(0);
  const auto *recovered_divisor = reconstructed_division.frame.find(2);
  const auto *recovered_operand = reconstructed_division.frame.find(3);
  if (division_record == nullptr ||
      division_record->reason !=
          unijit::runtime::DeoptimizationReason::kDivisionByZero ||
      !reconstructed_division.ok() ||
      reconstructed_division.frame.resume_offset != division_site ||
      recovered_lhs == nullptr ||
      recovered_lhs->value != zero_division_arguments[0] ||
      recovered_divisor == nullptr ||
      recovered_divisor->value != zero_division_arguments[1] ||
      recovered_operand == nullptr ||
      recovered_operand->value != unijit::ir::pack_float64(10.0)) {
    std::cerr << "PocketPy division exit did not reconstruct its frame\n";
    return EXIT_FAILURE;
  }

  const auto constant_division =
      unijit::frontend::pocketpy::translate_numeric_function(
          "def half(a): return a / 2");
  if (!constant_division.ok() ||
      constant_division.function->requires_context() ||
      !constant_division.function->deoptimization_table().empty()) {
    std::cerr << "PocketPy constant division retained a redundant guard\n";
    return EXIT_FAILURE;
  }

  const auto baseline_constant_division =
      unijit::frontend::pocketpy::translate_numeric_function(
          "def half(a): return a / 2",
          unijit::jit::OptimizationLevel::kBaseline);
  if (!baseline_constant_division.ok() ||
      !baseline_constant_division.function->requires_context() ||
      baseline_constant_division.function->deoptimization_table().empty() ||
      baseline_constant_division.function->stats().input_ir_nodes !=
          baseline_constant_division.function->stats().optimized_ir_nodes ||
      constant_division.function->stats().optimized_ir_nodes >=
          baseline_constant_division.function->stats().optimized_ir_nodes) {
    std::cerr << "PocketPy baseline and optimized tiers were not distinct\n";
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 1> constant_division_arguments = {
      unijit::ir::pack_float64(9.0)};
  const auto half = constant_division.function->invoke(
      constant_division_arguments.data(), constant_division_arguments.size());
  if (!half.ok() || unijit::ir::unpack_float64(half.value) != 4.5) {
    std::cerr << "PocketPy constant division produced the wrong result\n";
    return EXIT_FAILURE;
  }

  constexpr char kCountedLoopSource[] =
      "def numeric_workload(count):\n"
      "    lhs = 1.25\n"
      "    rhs = -7.5\n"
      "    checksum = 20.0 + -20.0\n"
      "    for iteration in range(count):\n"
      "        checksum = checksum + (lhs + rhs) * (lhs - 3.25) + rhs * 0.75\n"
      "        lhs = lhs + 0.125\n"
      "        rhs = rhs - 0.0625\n"
      "        if lhs > 4096.0:\n"
      "            lhs = 1.25\n"
      "        if rhs < -4096.0:\n"
      "            rhs = -7.5\n"
      "    return checksum\n";
  const auto counted_loop =
      unijit::frontend::pocketpy::translate_numeric_function(
          kCountedLoopSource);
  const auto baseline_counted_loop =
      unijit::frontend::pocketpy::translate_numeric_function(
          kCountedLoopSource, unijit::jit::OptimizationLevel::kBaseline);
  if (!counted_loop.ok() || counted_loop.parameter_count != 1 ||
      !counted_loop.function->requires_context() ||
      !baseline_counted_loop.ok() ||
      baseline_counted_loop.function->stats().input_ir_nodes !=
          baseline_counted_loop.function->stats().optimized_ir_nodes ||
      counted_loop.function->stats().optimized_ir_nodes >=
          baseline_counted_loop.function->stats().optimized_ir_nodes ||
      !unijit::frontend::pocketpy::supports_tiered_translation(
          kCountedLoopSource)) {
    std::cerr << "PocketPy counted loop did not compile: "
              << counted_loop.status.message() << '\n';
    return EXIT_FAILURE;
  }
  if (!unijit::frontend::pocketpy::supports_tiered_translation(
          "def half(a): return a / 2")) {
    std::cerr << "PocketPy straight-line source was not tierable\n";
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
    std::cerr << "PocketPy counted loop produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr char kLoopControlSource[] =
      "def gated_sum(count):\n"
      "    sum = 0.0\n"
      "    for iteration in range(count):\n"
      "        if iteration >= 8.0:\n"
      "            break\n"
      "        if iteration < 3.0:\n"
      "            continue\n"
      "        sum += iteration\n"
      "    return sum\n";
  const auto loop_control =
      unijit::frontend::pocketpy::translate_numeric_function(
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
    std::cerr << "PocketPy break/continue loop semantics were not preserved: "
              << loop_control.status.message() << '\n';
    return EXIT_FAILURE;
  }

  constexpr char kEqualityLoopSource[] =
      "def equality_loop():\n"
      "    sum = 0.0\n"
      "    for iteration in range(6.0):\n"
      "        if iteration == 2.0:\n"
      "            continue\n"
      "        if iteration == 5.0:\n"
      "            break\n"
      "        if iteration != 3.0:\n"
      "            sum += iteration\n"
      "    return sum\n";
  const auto equality_loop =
      unijit::frontend::pocketpy::translate_numeric_function(
          kEqualityLoopSource);
  const auto equality_loop_result =
      equality_loop.ok() ? equality_loop.function->invoke(nullptr, 0)
                         : unijit::ir::EvaluationResult{};
  if (!equality_loop.ok() || !equality_loop_result.ok() ||
      equality_loop_result.value != unijit::ir::pack_float64(5.0)) {
    std::cerr << "PocketPy equality loop semantics were not preserved: "
              << equality_loop.status.message() << '\n';
    return EXIT_FAILURE;
  }
  constexpr char kRejectedControlElse[] =
      "def rejected(count):\n"
      "    sum = 0.0\n"
      "    for iteration in range(count):\n"
      "        if iteration > 2.0:\n"
      "            break\n"
      "        else:\n"
      "            sum += iteration\n"
      "    return sum\n";
  if (unijit::frontend::pocketpy::translate_numeric_function(
          kRejectedControlElse)
          .ok()) {
    std::cerr << "PocketPy accepted a break guard with an else arm\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char*, 3> kStridedLoopSources = {
      "def two_argument_range():\n"
      "    sum = 0.0\n"
      "    for iteration in range(2.0, 8.0):\n"
      "        sum += iteration\n"
      "    return sum\n",
      "def stepped_range():\n"
      "    sum = 0.0\n"
      "    for iteration in range(1.0, 10.0, 2.0):\n"
      "        if iteration > 5.0:\n"
      "            break\n"
      "        sum += iteration\n"
      "    return sum\n",
      "def descending_range():\n"
      "    sum = 0.0\n"
      "    for iteration in range(10.0, -1.0, -2.0):\n"
      "        if iteration < 4.0:\n"
      "            continue\n"
      "        sum += iteration\n"
      "    return sum\n"};
  constexpr std::array<double, 3> kStridedLoopResults = {27.0, 9.0, 28.0};
  for (std::size_t index = 0; index < kStridedLoopSources.size(); ++index) {
    const auto strided =
        unijit::frontend::pocketpy::translate_numeric_function(
            kStridedLoopSources[index]);
    const auto strided_result =
        strided.ok() ? strided.function->invoke(nullptr, 0)
                     : unijit::ir::EvaluationResult{};
    if (!strided.ok() || !strided_result.ok() ||
        strided_result.value !=
            unijit::ir::pack_float64(kStridedLoopResults[index])) {
      std::cerr << "PocketPy strided range semantics were not preserved: "
                << strided.status.message() << '\n';
      return EXIT_FAILURE;
    }
  }
  constexpr std::array<const char*, 2> kRejectedRangeSteps = {
      "def zero_step():\n"
      "    sum = 0.0\n"
      "    for iteration in range(0.0, 10.0, 0.0):\n"
      "        sum += iteration\n"
      "    return sum\n",
      "def dynamic_step(step):\n"
      "    sum = 0.0\n"
      "    for iteration in range(0.0, 10.0, step):\n"
      "        sum += iteration\n"
      "    return sum\n"};
  for (const char *source : kRejectedRangeSteps) {
    if (unijit::frontend::pocketpy::translate_numeric_function(source).ok()) {
      std::cerr << "PocketPy accepted an unsupported range step\n";
      return EXIT_FAILURE;
    }
  }

  constexpr char kLoopDivisionSource[] =
      "def divide_loop(count, divisor):\n"
      "    quotient = 12.0\n"
      "    for iteration in range(count):\n"
      "        quotient = quotient / divisor\n"
      "    return quotient\n";
  const auto loop_division =
      unijit::frontend::pocketpy::translate_numeric_function(
          kLoopDivisionSource);
  const std::size_t loop_division_site =
      std::string_view(kLoopDivisionSource).find('/');
  if (!loop_division.ok() || loop_division.parameter_count != 2 ||
      !loop_division.function->requires_context() ||
      loop_division.function->deoptimization_table().size() != 1) {
    std::cerr << "PocketPy counted-loop division did not compile: "
              << loop_division.status.message() << '\n';
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> loop_division_arguments = {
      unijit::ir::pack_float64(2.0), unijit::ir::pack_float64(2.0)};
  const auto loop_quotient = loop_division.function->invoke(
      loop_division_arguments.data(), loop_division_arguments.size());
  if (!loop_quotient.ok() ||
      loop_quotient.value != unijit::ir::pack_float64(3.0)) {
    std::cerr << "PocketPy counted-loop division produced the wrong result\n";
    return EXIT_FAILURE;
  }
  constexpr std::array<double, 2> kLoopZeroes = {0.0, -0.0};
  for (double zero : kLoopZeroes) {
    const std::array<unijit::ir::Word, 2> zero_arguments = {
        unijit::ir::pack_float64(2.0), unijit::ir::pack_float64(zero)};
    unijit::runtime::ExecutionContext context;
    const auto exit = loop_division.function->invoke(
        zero_arguments.data(), zero_arguments.size(), &context);
    const auto reconstruction =
        loop_division.function->reconstruct_deoptimization(
            loop_division_site, zero_arguments.data(), zero_arguments.size(),
            context);
    const auto *divisor = reconstruction.frame.find(2);
    const auto *quotient_state = reconstruction.frame.find(3);
    const auto *iteration_state = reconstruction.frame.find(4);
    if (exit.ok() ||
        exit.status.code() != unijit::StatusCode::kRuntimeExit ||
        exit.status.location() != loop_division_site ||
        !reconstruction.ok() ||
        reconstruction.frame.reason !=
            unijit::runtime::DeoptimizationReason::kDivisionByZero ||
        divisor == nullptr || divisor->value != zero_arguments[1] ||
        quotient_state == nullptr ||
        quotient_state->value != unijit::ir::pack_float64(12.0) ||
        iteration_state == nullptr ||
        iteration_state->value != unijit::ir::pack_float64(0.0)) {
      std::cerr << "PocketPy loop division did not reconstruct signed zero\n";
      return EXIT_FAILURE;
    }
  }
  const std::array<unijit::ir::Word, 2> skipped_division_arguments = {
      unijit::ir::pack_float64(0.0), unijit::ir::pack_float64(0.0)};
  const auto skipped_division = loop_division.function->invoke(
      skipped_division_arguments.data(), skipped_division_arguments.size());
  if (!skipped_division.ok() ||
      skipped_division.value != unijit::ir::pack_float64(12.0)) {
    std::cerr << "zero-iteration PocketPy loop executed its division\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char *, 7> kRejectedSources = {
      "lambda a: a + 1",
      "def f(a, a): return a",
      "def f(class): return class",
      "def f(a): return external + a",
      "def f(a):\n    b = a\n    return b",
      "def f(a): return a; return 0",
      "def f(a, b, c): return a < b < c"};
  for (const char *source : kRejectedSources) {
    if (unijit::frontend::pocketpy::translate_numeric_function(source).ok()) {
      std::cerr << "unsupported PocketPy source was accepted: " << source
                << '\n';
      return EXIT_FAILURE;
    }
  }

  py_initialize();
  if (unijit_pocketpy_install() != 0 || unijit_pocketpy_install() != 0) {
    std::cerr << "unable to install the UniJIT PocketPy module\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  const std::string oversized_source(1024U * 1024U + 1U, ' ');
  if (unijit_pocketpy_compile(oversized_source.c_str()) ||
      !py_matchexc(tp_ValueError)) {
    std::cerr << "PocketPy accepted source beyond the compilation budget\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);
  constexpr char kNativeSource[] =
      "import unijit\n"
      "native = unijit.compile(\"def affine(a, b): return (a + 2.5) * "
      "(b - -3)\")\n"
      "native_cached = unijit.compile(\"def affine(a, b): return (a + 2.5) * "
      "(b - -3)\")\n"
      "divide = unijit.compile(\"def divide(a, b): return a / b\")\n"
      "compare = unijit.compile(\"def compare(a, b): return (a + 1) >= b * 2\")\n"
      "equality = unijit.compile(\"def equality(a, b): return a != b\")\n"
      "negate = unijit.compile(\"def negate(value): return -value\")\n";
  if (!py_exec(kNativeSource, "<unijit-pocketpy-native>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("cold_tier = unijit.stats(native)\n"
               "assert cold_tier['active_tier'] == 'baseline'\n"
               "assert cold_tier['tierable']\n"
               "assert cold_tier['invocations'] == 0\n"
               "assert cold_tier['backedges'] == 0\n"
               "assert cold_tier['compilation_state'] == 'idle'\n"
               "assert cold_tier['scheduler_available']\n"
               "assert unijit.wait(native, 0)\n"
               "assert not unijit.cancel(native)\n"
               "assert cold_tier['input_ir_nodes'] == "
               "cold_tier['active_ir_nodes']\n",
               "<unijit-pocketpy-cold-tier>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("comparison = compare(3, 2)\n"
               "equality_result = equality(3, 2)\n"
               "for comparison_iteration in range(63):\n"
               "    comparison = compare(3, 2)\n"
               "    equality_result = equality(3, 2)\n"
               "assert unijit.wait(compare, 5000)\n"
               "assert unijit.wait(equality, 5000)\n"
               "comparison_after = compare(2, 2)\n"
               "equality_after = equality(0.0, -0.0)\n"
               "comparison_tier = unijit.stats(compare)\n"
               "equality_tier = unijit.stats(equality)\n"
               "assert comparison_tier['active_tier'] == 'optimized'\n"
               "assert comparison_tier['promotions'] == 1\n"
               "assert equality_tier['active_tier'] == 'optimized'\n"
               "assert equality_tier['promotions'] == 1\n",
               "<unijit-pocketpy-boolean>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref comparison_result = py_getglobal(py_name("comparison"));
  const py_Ref comparison_after = py_getglobal(py_name("comparison_after"));
  const py_Ref equality_result = py_getglobal(py_name("equality_result"));
  const py_Ref equality_after = py_getglobal(py_name("equality_after"));
  if (comparison_result == nullptr || !py_isbool(comparison_result) ||
      !py_tobool(comparison_result) || comparison_after == nullptr ||
      !py_isbool(comparison_after) || py_tobool(comparison_after) ||
      equality_result == nullptr || !py_isbool(equality_result) ||
      !py_tobool(equality_result) || equality_after == nullptr ||
      !py_isbool(equality_after) || py_tobool(equality_after)) {
    std::cerr << "PocketPy did not retain bool results across tiering\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("negated_zero = negate(0.0)\n"
               "for negate_iteration in range(63):\n"
               "    negated_zero = negate(0.0)\n"
               "assert unijit.wait(negate, 5000)\n"
               "negated_zero = negate(0.0)\n"
               "negated_positive_zero = negate(-0.0)\n"
               "negate_tier = unijit.stats(negate)\n"
               "assert negate_tier['active_tier'] == 'optimized'\n"
               "assert negate_tier['promotions'] == 1\n",
               "<unijit-pocketpy-negate>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref negated_zero = py_getglobal(py_name("negated_zero"));
  const py_Ref negated_positive_zero =
      py_getglobal(py_name("negated_positive_zero"));
  if (negated_zero == nullptr || !py_isfloat(negated_zero) ||
      negated_positive_zero == nullptr ||
      !py_isfloat(negated_positive_zero) ||
      unijit::ir::pack_float64(py_tofloat(negated_zero)) !=
          unijit::ir::pack_float64(-0.0) ||
      unijit::ir::pack_float64(py_tofloat(negated_positive_zero)) !=
          unijit::ir::pack_float64(0.0)) {
    std::cerr << "PocketPy runtime did not preserve unary signed zero\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeLoopSource[] =
      "native_loop = unijit.compile('''def numeric_workload(count):\n"
      "    lhs = 1.25\n"
      "    rhs = -7.5\n"
      "    checksum = 20.0 + -20.0\n"
      "    for iteration in range(count):\n"
      "        checksum = checksum + (lhs + rhs) * (lhs - 3.25) + rhs * 0.75\n"
      "        lhs = lhs + 0.125\n"
      "        rhs = rhs - 0.0625\n"
      "        if lhs > 4096.0:\n"
      "            lhs = 1.25\n"
      "        if rhs < -4096.0:\n"
      "            rhs = -7.5\n"
      "    return checksum\n''')\n"
      "loop_result = native_loop(10000)\n";
  if (!py_exec(kNativeLoopSource, "<unijit-pocketpy-counted-loop>",
               EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref native_loop_result = py_getglobal(py_name("loop_result"));
  if (native_loop_result == nullptr || !py_isfloat(native_loop_result) ||
      unijit::ir::pack_float64(py_tofloat(native_loop_result)) !=
          unijit::ir::pack_float64(expected_loop)) {
    std::cerr << "PocketPy did not execute the compiled counted-loop callable\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeLoopControlSource[] =
      "native_control_loop = unijit.compile('''def gated_sum(count):\n"
      "    sum = 0.0\n"
      "    for iteration in range(count):\n"
      "        if iteration >= 8.0:\n"
      "            break\n"
      "        if iteration < 3.0:\n"
      "            continue\n"
      "        sum += iteration\n"
      "    return sum\n''')\n"
      "control_loop_result = native_control_loop(100)\n";
  if (!py_exec(kNativeLoopControlSource,
               "<unijit-pocketpy-loop-control>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref control_loop_result =
      py_getglobal(py_name("control_loop_result"));
  if (control_loop_result == nullptr || !py_isfloat(control_loop_result) ||
      py_tofloat(control_loop_result) != 25.0) {
    std::cerr << "PocketPy runtime did not execute loop control guards\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeEqualityLoopSource[] =
      "native_equality_loop = unijit.compile('''def equality_loop():\n"
      "    sum = 0.0\n"
      "    for iteration in range(6.0):\n"
      "        if iteration == 2.0:\n"
      "            continue\n"
      "        if iteration == 5.0:\n"
      "            break\n"
      "        if iteration != 3.0:\n"
      "            sum += iteration\n"
      "    return sum\n''')\n"
      "equality_loop_result = native_equality_loop()\n";
  if (!py_exec(kNativeEqualityLoopSource,
               "<unijit-pocketpy-equality-loop>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref native_equality_loop_result =
      py_getglobal(py_name("equality_loop_result"));
  if (native_equality_loop_result == nullptr ||
      !py_isfloat(native_equality_loop_result) ||
      py_tofloat(native_equality_loop_result) != 5.0) {
    std::cerr << "PocketPy runtime did not execute an equality loop\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeNegateLoopSource[] =
      "native_negate_loop = unijit.compile('''def negate_loop(value):\n"
      "    result = value\n"
      "    for iteration in range(1.0):\n"
      "        result = -result\n"
      "    return result\n''')\n"
      "negate_loop_zero = native_negate_loop(0.0)\n"
      "negate_loop_positive_zero = native_negate_loop(-0.0)\n";
  if (!py_exec(kNativeNegateLoopSource,
               "<unijit-pocketpy-negate-loop>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref negate_loop_zero =
      py_getglobal(py_name("negate_loop_zero"));
  const py_Ref negate_loop_positive_zero =
      py_getglobal(py_name("negate_loop_positive_zero"));
  if (negate_loop_zero == nullptr || !py_isfloat(negate_loop_zero) ||
      negate_loop_positive_zero == nullptr ||
      !py_isfloat(negate_loop_positive_zero) ||
      unijit::ir::pack_float64(py_tofloat(negate_loop_zero)) !=
          unijit::ir::pack_float64(-0.0) ||
      unijit::ir::pack_float64(py_tofloat(negate_loop_positive_zero)) !=
          unijit::ir::pack_float64(0.0)) {
    std::cerr << "PocketPy runtime did not preserve unary loop signed zero\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeStridedLoopSource[] =
      "native_strided_loop = unijit.compile('''def descending_range():\n"
      "    sum = 0.0\n"
      "    for iteration in range(10.0, -1.0, -2.0):\n"
      "        if iteration < 4.0:\n"
      "            continue\n"
      "        sum += iteration\n"
      "    return sum\n''')\n"
      "strided_loop_result = native_strided_loop()\n";
  if (!py_exec(kNativeStridedLoopSource,
               "<unijit-pocketpy-strided-loop>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref strided_loop_result =
      py_getglobal(py_name("strided_loop_result"));
  if (strided_loop_result == nullptr || !py_isfloat(strided_loop_result) ||
      py_tofloat(strided_loop_result) != 28.0) {
    std::cerr << "PocketPy runtime did not execute a strided range loop\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("assert unijit.wait(native_loop, 5000)\n"
               "loop_tier = unijit.stats(native_loop)\n"
               "assert loop_tier['active_tier'] == 'optimized'\n"
               "assert loop_tier['tierable']\n"
               "assert loop_tier['invocations'] == 1\n"
               "assert loop_tier['backedges'] == 10000\n"
               "assert loop_tier['compilation_attempts'] == 1\n"
               "assert loop_tier['successful_compilations'] == 1\n"
               "assert loop_tier['failed_compilations'] == 0\n"
               "assert loop_tier['promotions'] == 1\n"
               "assert loop_tier['compilation_state'] == 'succeeded'\n"
               "assert loop_tier['active_ir_nodes'] < "
               "loop_tier['input_ir_nodes']\n",
               "<unijit-pocketpy-loop-tier>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeLoopDivisionSource[] =
      "native_loop_divide = unijit.compile('''def loop_divide(count, divisor):\n"
      "    quotient = 12.0\n"
      "    for iteration in range(count):\n"
      "        quotient /= divisor\n"
      "    return quotient\n''')\n"
      "loop_quotient = native_loop_divide(2, 2)\n"
      "skipped_quotient = native_loop_divide(0, 0)\n"
      "for loop_division_iteration in range(62):\n"
      "    loop_quotient = native_loop_divide(2, 2)\n"
      "assert unijit.wait(native_loop_divide, 5000)\n"
      "loop_division_tier = unijit.stats(native_loop_divide)\n"
      "assert loop_quotient == 3.0\n"
      "assert skipped_quotient == 12.0\n"
      "assert loop_division_tier['tierable']\n"
      "assert loop_division_tier['active_tier'] == 'optimized'\n"
      "assert loop_division_tier['compilation_attempts'] == 1\n"
      "assert loop_division_tier['successful_compilations'] == 1\n"
      "assert loop_division_tier['failed_compilations'] == 0\n"
      "assert loop_division_tier['promotions'] == 1\n"
      "assert loop_division_tier['compilation_state'] == 'succeeded'\n";
  if (!py_exec(kNativeLoopDivisionSource,
               "<unijit-pocketpy-counted-loop-division>", EXEC_MODE,
               nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr std::array<const char *, 2> kLoopZeroDivisions = {
      "native_loop_divide(2, 0.0)", "native_loop_divide(2, -0.0)"};
  for (const char *expression : kLoopZeroDivisions) {
    if (py_exec(expression, "<unijit-pocketpy-loop-zero-division>",
                EVAL_MODE, nullptr) ||
        !py_matchexc(tp_ZeroDivisionError)) {
      std::cerr << "PocketPy loop division did not raise ZeroDivisionError\n";
      py_finalize();
      return EXIT_FAILURE;
    }
    py_clearexc(nullptr);
  }
  (void)py_gc_collect();
  if (!py_exec("result = native(1.5, 4)", "<unijit-pocketpy-call>",
               EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref result = py_getglobal(py_name("result"));
  if (result == nullptr || !py_isfloat(result) || py_tofloat(result) != 28.0) {
    std::cerr << "PocketPy did not execute the compiled native callable\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("for tier_iteration in range(63):\n"
               "    result = native(1.5, 4)\n"
               "assert unijit.wait(native, 5000)\n"
               "hot_tier = unijit.stats(native)\n"
               "assert hot_tier['active_tier'] == 'optimized'\n"
               "assert hot_tier['invocations'] == 64\n"
               "assert hot_tier['backedges'] == 0\n"
               "assert hot_tier['compilation_attempts'] == 1\n"
               "assert hot_tier['successful_compilations'] == 1\n"
               "assert hot_tier['failed_compilations'] == 0\n"
               "assert hot_tier['promotions'] == 1\n"
               "assert hot_tier['compilation_state'] == 'succeeded'\n"
               "assert not hot_tier['cancellation_requested']\n"
               "assert hot_tier['scheduler_available']\n"
               "assert not unijit.cancel(native)\n"
               "assert hot_tier['active_ir_nodes'] < "
               "hot_tier['input_ir_nodes']\n",
               "<unijit-pocketpy-hot-tier>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }

  if (!py_exec("cached_result = native_cached(1.5, 4)",
               "<unijit-pocketpy-cached-call>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref cached_result = py_getglobal(py_name("cached_result"));
  if (cached_result == nullptr || !py_isfloat(cached_result) ||
      py_tofloat(cached_result) != 28.0) {
    std::cerr << "PocketPy did not execute a cached native callable\n";
    py_finalize();
    return EXIT_FAILURE;
  }

  if (!py_exec("quotient = divide(9, 3)", "<unijit-pocketpy-division>",
               EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref division_result = py_getglobal(py_name("quotient"));
  if (division_result == nullptr || !py_isfloat(division_result) ||
      py_tofloat(division_result) != 3.0) {
    std::cerr << "PocketPy did not execute checked native division\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("for division_tier_iteration in range(63):\n"
               "    quotient = divide(9, 3)\n"
               "assert unijit.wait(divide, 5000)\n"
               "division_tier = unijit.stats(divide)\n"
               "assert division_tier['active_tier'] == 'optimized'\n"
               "assert division_tier['compilation_state'] == 'succeeded'\n"
               "assert division_tier['promotions'] == 1\n",
               "<unijit-pocketpy-division-tier>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }

  constexpr std::array<const char *, 2> kZeroDivisions = {
      "divide(1, 0.0)", "divide(1, -0.0)"};
  for (const char *expression : kZeroDivisions) {
    if (py_exec(expression, "<unijit-pocketpy-zero-division>", EVAL_MODE,
                nullptr) ||
        !py_matchexc(tp_ZeroDivisionError)) {
      std::cerr << "PocketPy native division did not raise ZeroDivisionError\n";
      py_finalize();
      return EXIT_FAILURE;
    }
    py_clearexc(nullptr);
  }

  if (py_exec("native('1.5', 4)", "<unijit-pocketpy-guard>", EVAL_MODE,
              nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy native callable accepted a non-number argument\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("unijit.stats(1)", "<unijit-pocketpy-stats-type>", EVAL_MODE,
              nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy tier stats accepted a foreign object\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("unijit.wait(native, -1)", "<unijit-pocketpy-wait-range>",
              EVAL_MODE, nullptr) ||
      !py_matchexc(tp_ValueError)) {
    std::cerr << "PocketPy tier wait accepted a negative timeout\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("unijit.cancel(1)", "<unijit-pocketpy-cancel-type>", EVAL_MODE,
              nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy tier cancellation accepted a foreign object\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("unijit._CompiledFunction()", "<unijit-pocketpy-constructor>",
              EVAL_MODE, nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy exposed unsafe direct native construction\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("native(1.5)", "<unijit-pocketpy-arity>", EVAL_MODE, nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy native callable accepted the wrong arity\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);
  py_finalize();

  std::cout << "PocketPy numeric source translator test passed\n";
  return EXIT_SUCCESS;
}
