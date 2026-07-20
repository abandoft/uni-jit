#include <array>
#include <cstdlib>
#include <iostream>
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
  if (division_record == nullptr ||
      division_record->reason !=
          unijit::runtime::DeoptimizationReason::kDivisionByZero ||
      !reconstructed_division.ok() ||
      reconstructed_division.frame.resume_offset != division_site ||
      recovered_lhs == nullptr ||
      recovered_lhs->value != zero_division_arguments[0] ||
      recovered_divisor == nullptr ||
      recovered_divisor->value != zero_division_arguments[1]) {
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
      "    checksum = 0.0\n"
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
  if (!counted_loop.ok() || counted_loop.parameter_count != 1 ||
      !counted_loop.function->requires_context() ||
      unijit::frontend::pocketpy::supports_tiered_translation(
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

  constexpr std::array<const char *, 6> kRejectedSources = {
      "lambda a: a + 1",
      "def f(a, a): return a",
      "def f(class): return class",
      "def f(a): return external + a",
      "def f(a):\n    b = a\n    return b",
      "def f(a): return a; return 0"};
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
  constexpr char kNativeSource[] =
      "import unijit\n"
      "native = unijit.compile(\"def affine(a, b): return (a + 2.5) * "
      "(b - -3)\")\n"
      "native_cached = unijit.compile(\"def affine(a, b): return (a + 2.5) * "
      "(b - -3)\")\n"
      "divide = unijit.compile(\"def divide(a, b): return a / b\")\n";
  if (!py_exec(kNativeSource, "<unijit-pocketpy-native>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  if (!py_exec("cold_tier = unijit.stats(native)\n"
               "assert cold_tier['active_tier'] == 'baseline'\n"
               "assert cold_tier['tierable']\n"
               "assert cold_tier['invocations'] == 0\n"
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
  constexpr char kNativeLoopSource[] =
      "native_loop = unijit.compile('''def numeric_workload(count):\n"
      "    lhs = 1.25\n"
      "    rhs = -7.5\n"
      "    checksum = 0.0\n"
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
  if (!py_exec("loop_tier = unijit.stats(native_loop)\n"
               "assert loop_tier['active_tier'] == 'baseline'\n"
               "assert not loop_tier['tierable']\n"
               "assert loop_tier['compilation_attempts'] == 0\n",
               "<unijit-pocketpy-loop-tier>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
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
