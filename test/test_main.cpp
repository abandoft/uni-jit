#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/ir/optimizer.h"
#include "unijit/jit/compiler.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::Compiler;

int failures = 0;
std::size_t runtime_call_count = 0;

Word sum_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  Word result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    result += arguments[index];
  }
  return result;
}

Word float_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  if (count != 2) {
    return unijit::ir::pack_float64(0.0);
  }
  return unijit::ir::pack_float64(
      unijit::ir::unpack_float64(arguments[0]) *
      unijit::ir::unpack_float64(arguments[1]));
}

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Function arithmetic_function() {
  FunctionBuilder builder(2);
  const Value sum = builder.add(builder.parameter(0), builder.parameter(1));
  const Value scaled = builder.multiply(sum, builder.constant(-7));
  const Value result = builder.subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "arithmetic return must be accepted");
  return std::move(builder).build();
}

void test_verifier_rejects_forward_reference() {
  FunctionBuilder builder(0);
  const Value invalid_binary = builder.add(Value{7}, Value{8});
  expect(builder.set_return(invalid_binary).ok(),
         "builder records a structurally present return value");
  const Function function = std::move(builder).build();
  const unijit::Status status = unijit::ir::verify(function);
  expect(!status.ok(), "verifier must reject non-dominating SSA operands");
  expect(status.location() == 0, "verifier must report the invalid node index");
}

void test_constant_native_function() {
  FunctionBuilder builder(0);
  const Value constant = builder.constant(std::numeric_limits<Word>::min());
  expect(builder.set_return(constant).ok(), "constant return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok(), "constant native function must execute");
  expect(result.value == std::numeric_limits<Word>::min(),
         "native constant materialization must preserve all 64 bits");
}

void test_execution_context_lifecycle() {
  unijit::runtime::ExecutionContext context;
  expect(!context.interrupt_requested(),
         "new execution contexts must not request interruption");
  context.request_interrupt();
  expect(context.interrupt_requested(),
         "execution contexts must publish interruption requests");
  context.clear_interrupt();
  expect(!context.interrupt_requested(),
         "execution contexts must clear interruption requests");

  context.record_exit(unijit::runtime::ExitReason::kRuntime, 91, -17);
  expect(context.exit_reason() == unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 91 && context.exit_value() == -17,
         "execution contexts must retain runtime-exit diagnostics");

  FunctionBuilder builder(0);
  expect(builder.set_return(builder.constant(73)).ok(),
         "execution-context fixture must have a return value");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "execution-context fixture must compile");
  if (compilation.ok()) {
    const auto result = compilation.function->invoke(nullptr, 0, &context);
    expect(result.ok() && result.value == 73,
           "invocation must clear stale exits before entering native code");
    expect(context.exit_reason() == unijit::runtime::ExitReason::kNone,
           "successful invocation must leave no exit reason");
  }
}

void test_safepoint_ir_and_interpreter() {
  FunctionBuilder builder(0);
  const Value base = builder.call(
      sum_runtime_helper, {builder.constant(70), builder.constant(3)});
  const Value safepoint = builder.safepoint(42);
  expect(safepoint.valid(), "safepoint must produce an effect value");
  const Value result = builder.add(base, safepoint);
  expect(builder.set_return(result).ok(),
         "safepoint fixture must have a return value");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "safepoint IR must verify");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "safepoint IR must optimize");
  if (optimization.ok()) {
    const bool preserved = std::any_of(
        optimization.function.nodes().begin(),
        optimization.function.nodes().end(), [](const unijit::ir::Node& node) {
          return node.opcode == unijit::ir::Opcode::kSafepoint;
        });
    expect(preserved, "optimizer must preserve a dead-result safepoint");
  }

  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interrupted = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(!interrupted.ok() &&
             interrupted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interrupted.status.location() == 42 &&
             context.exit_reason() ==
                 unijit::runtime::ExitReason::kSafepoint &&
             context.exit_site() == 42,
         "interpreter safepoints must report interruption and site identity");

  context.clear_interrupt();
  const auto completed = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(completed.ok() && completed.value == 73 &&
             context.exit_reason() == unijit::runtime::ExitReason::kNone,
         "clear safepoints must continue without changing the result");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "safepoint IR must compile to native code");
  if (compilation.ok()) {
    context.request_interrupt();
    const auto native_interrupted =
        compilation.function->invoke(nullptr, 0, &context);
    expect(!native_interrupted.ok() &&
               native_interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               native_interrupted.status.location() == 42 &&
               context.exit_reason() ==
                   unijit::runtime::ExitReason::kSafepoint,
           "native safepoints must exit with the matching site identity");

    context.clear_interrupt();
    const auto native_completed =
        compilation.function->invoke(nullptr, 0, &context);
    expect(native_completed.ok() && native_completed.value == 73,
           "native clear safepoints must continue with a zero effect value");
    expect(compilation.function->native_entry()(nullptr, nullptr) == 73,
           "a null execution context must bypass safepoint polling");
  }
}

void test_differential_arithmetic() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "arithmetic function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }

  std::mt19937_64 random(0x554E494A4954ULL);
  for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
    const std::array<Word, 2> args = {static_cast<Word>(random()),
                                      static_cast<Word>(random())};
    const auto interpreted =
        Interpreter::evaluate(function, args.data(), args.size());
    const auto native = compilation.function->invoke(args.data(), args.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      expect(false, "native arithmetic must match the interpreter oracle");
      return;
    }
  }
}

Function float64_function() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value sum =
      builder.float64_add(builder.parameter(0), builder.parameter(1));
  const Value scaled =
      builder.float64_multiply(sum, builder.float64_constant(0.5));
  const Value result = builder.float64_subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "Float64 return must be accepted");
  return std::move(builder).build();
}

void test_float64_ir_and_interpreter() {
  const Function function = float64_function();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 SSA must pass verification");
  expect(function.return_type() == unijit::ir::ValueType::kFloat64,
         "Float64 result type must remain visible in the function");

  const std::array<Word, 2> args = {unijit::ir::pack_float64(19.25),
                                    unijit::ir::pack_float64(-4.75)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok(), "Float64 interpreter execution must succeed");
  expect(unijit::ir::unpack_float64(interpreted.value) == 12.0,
         "Float64 interpreter must preserve IEEE-754 arithmetic");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             optimization.function.return_type() ==
                 unijit::ir::ValueType::kFloat64,
         "optimizer must preserve Float64 types and operations");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 SSA must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  const std::array<double, 8> samples = {0.0,      -0.0,   1.25,   -7.5,
                                         1024.75, -33.125, 1.0e12, -1.0e-9};
  for (std::size_t lhs = 0; lhs < samples.size(); ++lhs) {
    const std::array<Word, 2> native_args = {
        unijit::ir::pack_float64(samples[lhs]),
        unijit::ir::pack_float64(samples[samples.size() - lhs - 1])};
    const auto expected =
        Interpreter::evaluate(function, native_args.data(), native_args.size());
    const auto native =
        compilation.function->invoke(native_args.data(), native_args.size());
    expect(native.ok() && expected.ok() && native.value == expected.value,
           "native Float64 arithmetic must match the interpreter bits");
  }
}

void test_float64_division() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value quotient =
      builder.float64_divide(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(quotient).ok(),
         "Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 division must pass verification");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 division must compile to native code");
  if (!compilation.ok()) {
    return;
  }

  constexpr std::array<std::array<double, 2>, 6> kSamples = {{
      {{9.0, 3.0}},
      {{-7.5, 2.5}},
      {{0.0, -3.0}},
      {{-0.0, 7.0}},
      {{1.0e-300, 2.0}},
      {{std::numeric_limits<double>::max(), 2.0}},
  }};
  for (const auto& sample : kSamples) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(sample[0]),
        unijit::ir::pack_float64(sample[1])};
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(interpreted.ok() && native.ok() &&
               native.value == interpreted.value,
           "native Float64 division must match the interpreter bits");
  }
}

void test_float64_nonzero_guard() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value guard = builder.guard_float64_nonzero(builder.parameter(1), 77);
  expect(guard.valid(), "Float64 nonzero guard must produce an effect value");
  const Value quotient =
      builder.float64_divide(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(quotient).ok(),
         "guarded Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 nonzero guard must pass verification");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::any_of(optimization.function.nodes().begin(),
                         optimization.function.nodes().end(),
                         [](const unijit::ir::Node& node) {
                           return node.opcode ==
                                  unijit::ir::Opcode::kGuardFloatNonzero;
                         }),
         "optimizer must preserve a dead-result Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 nonzero guard must compile to native code");
  if (!compilation.ok()) {
    return;
  }

  const std::array<Word, 2> valid = {unijit::ir::pack_float64(9.0),
                                     unijit::ir::pack_float64(3.0)};
  const auto interpreted =
      Interpreter::evaluate(function, valid.data(), valid.size());
  const auto native = compilation.function->invoke(valid.data(), valid.size());
  expect(interpreted.ok() && native.ok() &&
             unijit::ir::unpack_float64(native.value) == 3.0,
         "a nonzero Float64 divisor must pass the guard");

  constexpr std::array<double, 2> kZeroes = {0.0, -0.0};
  for (double zero : kZeroes) {
    const std::array<Word, 2> invalid = {unijit::ir::pack_float64(9.0),
                                         unijit::ir::pack_float64(zero)};
    unijit::runtime::ExecutionContext interpreter_context;
    const auto rejected_by_interpreter = Interpreter::evaluate(
        function, invalid.data(), invalid.size(), &interpreter_context);
    expect(!rejected_by_interpreter.ok() &&
               rejected_by_interpreter.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_by_interpreter.status.location() == 77 &&
               interpreter_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime,
           "interpreter guard must reject both signed Float64 zeroes");

    const auto rejected_with_local_context =
        compilation.function->invoke(invalid.data(), invalid.size());
    expect(!rejected_with_local_context.ok() &&
               rejected_with_local_context.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_with_local_context.status.location() == 77,
           "managed invocation must diagnose a guard without caller context");

    unijit::runtime::ExecutionContext native_context;
    const auto rejected_by_native = compilation.function->invoke(
        invalid.data(), invalid.size(), &native_context);
    expect(!rejected_by_native.ok() &&
               native_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime &&
               native_context.exit_site() == 77,
           "native guard must publish its stable exit site");
  }
}

void test_constant_float64_nonzero_guard_elimination() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value divisor = builder.float64_constant(2.0);
  const Value guard = builder.guard_float64_nonzero(divisor, 81);
  expect(guard.valid(), "constant nonzero guard fixture must be constructible");
  const Value quotient = builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "constant nonzero guard fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::none_of(optimization.function.nodes().begin(),
                          optimization.function.nodes().end(),
                          [](const unijit::ir::Node& node) {
                            return node.opcode ==
                                   unijit::ir::Opcode::kGuardFloatNonzero;
                          }),
         "optimizer must eliminate a provably passing Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant-guard fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(!compilation.function->requires_context(),
         "eliminated guards must not require a managed execution context");
  const std::array<Word, 1> arguments = {unijit::ir::pack_float64(9.0)};
  const auto result =
      compilation.function->invoke(arguments.data(), arguments.size());
  expect(result.ok() && unijit::ir::unpack_float64(result.value) == 4.5,
         "constant-guard elimination must preserve the quotient");
}

void test_verifier_rejects_mixed_arithmetic() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value invalid =
      builder.add(builder.parameter(0), builder.float64_constant(1.0));
  expect(builder.set_return(invalid).ok(),
         "mixed-type fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject mixed Word and Float64 arithmetic");
}

void test_float64_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(std::vector<unijit::ir::ValueType>(
      kParameters, unijit::ir::ValueType::kFloat64));
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(
            builder.float64_add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "Float64 spill fixture must return its reduction");
  const Function function = std::move(builder).build();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 register pressure must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "Float64 register pressure must exercise spill slots");

  std::array<Word, kParameters> args{};
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = unijit::ir::pack_float64(
        static_cast<double>(index + 1) * 0.25);
  }
  const auto expected =
      Interpreter::evaluate(function, args.data(), args.size());
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && expected.ok() && native.value == expected.value,
         "spilled native Float64 values must match the interpreter bits");
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool preserves_host_float_registers(unijit::jit::NativeEntry entry) {
  double lhs = 1.25;
  double rhs = -7.5;
  for (std::size_t iteration = 0; iteration < 512; ++iteration) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const double expected = (lhs + rhs) * (lhs - 3.25) + rhs * 0.75;
    const double native =
        unijit::ir::unpack_float64(entry(arguments.data(), nullptr));
    if (native != expected) {
      return false;
    }
    lhs += 0.125;
    rhs -= 0.0625;
  }
  return lhs == 65.25 && rhs == -39.5;
}

void test_float64_preserves_host_abi() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value lhs = builder.parameter(0);
  const Value rhs = builder.parameter(1);
  const Value product = builder.float64_multiply(
      builder.float64_add(lhs, rhs),
      builder.float64_subtract(lhs, builder.float64_constant(3.25)));
  const Value result = builder.float64_add(
      product,
      builder.float64_multiply(rhs, builder.float64_constant(0.75)));
  expect(builder.set_return(result).ok(),
         "host-ABI fixture must return its Float64 expression");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "host-ABI Float64 fixture must compile");
  if (compilation.ok()) {
    expect(preserves_host_float_registers(
               compilation.function->native_entry()),
           "native Float64 code must preserve host callee-saved registers");
  }
}

void test_runtime_helper_call() {
  FunctionBuilder builder(2);
  const Value live = builder.add(builder.parameter(0), builder.constant(5));
  const Value called = builder.call(
      sum_runtime_helper, {live, builder.parameter(1), builder.constant(7)});
  const Value result = builder.multiply(live, called);
  expect(builder.set_return(result).ok(),
         "runtime-call fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "runtime helper call must satisfy SSA verification");

  const std::array<Word, 2> args = {3, 4};
  runtime_call_count = 0;
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 152 &&
             runtime_call_count == 1,
         "interpreter must execute runtime helpers with ordered arguments");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "runtime helper call must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native runtime call must preserve live values and helper effects");
}

void test_effectful_dead_runtime_call() {
  FunctionBuilder builder(0);
  const Value ignored = builder.call(sum_runtime_helper, {});
  (void)ignored;
  expect(builder.set_return(builder.constant(42)).ok(),
         "effectful-call fixture must record a return");
  const Function function = std::move(builder).build();
  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept an effectful runtime call");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "dead-result runtime call must remain compilable");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok() && result.value == 42 && runtime_call_count == 1,
         "optimizer must preserve runtime calls whose result is dead");
}

void test_float64_runtime_helper_call() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value live =
      builder.float64_add(builder.parameter(0), builder.float64_constant(0.5));
  const Value called = builder.call(
      float_runtime_helper, {live, builder.parameter(1)},
      unijit::ir::ValueType::kFloat64);
  const Value result = builder.float64_add(live, called);
  expect(builder.set_return(result).ok(),
         "Float64 runtime call must record its result");
  const Function function = std::move(builder).build();
  const std::array<Word, 2> args = {unijit::ir::pack_float64(3.5),
                                    unijit::ir::pack_float64(-2.0)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 runtime helper call must compile");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && interpreted.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native Float64 call must preserve live FP registers and result bits");
}

void test_verifier_rejects_null_runtime_helper() {
  FunctionBuilder builder(0);
  const Value invalid = builder.call(nullptr, {});
  expect(builder.set_return(invalid).ok(),
         "null-helper fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject a null runtime helper target");
}

void test_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(kParameters);
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(builder.add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "spill return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "register-pressure function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "register-pressure function must exercise spill slots");

  std::array<Word, kParameters> args{};
  Word expected = 0;
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = static_cast<Word>(index + 1);
    expected += args[index];
  }
  const auto result = compilation.function->invoke(args.data(), args.size());
  expect(result.ok(), "spilled native function must execute");
  expect(result.value == expected, "spilled values must survive allocation");
}

void test_argument_validation() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "validation fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  const std::array<Word, 1> too_few = {1};
  expect(!compilation.function->invoke(too_few.data(), too_few.size()).ok(),
         "compiled invocation must reject the wrong argument count");
  expect(!Interpreter::evaluate(function, nullptr, 2).ok(),
         "interpreter must reject null argument storage");
}

void test_optimization_pipeline() {
  FunctionBuilder builder(1);
  const Value zero = builder.constant(0);
  const Value dead_sum =
      builder.add(builder.parameter(0), builder.constant(99));
  const Value annihilated = builder.multiply(dead_sum, zero);
  const Value answer = builder.add(annihilated, builder.constant(42));
  expect(builder.set_return(answer).ok(), "optimized return must be accepted");
  const Function function = std::move(builder).build();

  auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimization pipeline must accept verified SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.input_nodes == 7,
         "optimizer must report the input SSA size");
  expect(optimization.stats.output_nodes == 2,
         "optimizer must fold the graph to one parameter and one constant");
  expect(optimization.stats.constants_folded > 0,
         "optimizer must exercise constant folding");
  expect(optimization.stats.algebraic_simplifications > 0,
         "optimizer must exercise algebraic simplification");

  const std::array<Word, 1> args = {std::numeric_limits<Word>::max()};
  const auto interpreted =
      Interpreter::evaluate(optimization.function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 42,
         "optimized SSA must preserve observable semantics");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "compiler must consume optimized SSA");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().input_ir_nodes == 7,
         "compiler must report input IR nodes");
  expect(compilation.function->stats().optimized_ir_nodes == 2,
         "compiler must report optimized IR nodes");
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == 42,
         "optimized native code must preserve observable semantics");
}

void test_float64_constant_folding() {
  FunctionBuilder builder(0);
  const Value numerator = builder.float64_add(
      builder.float64_constant(12.0), builder.float64_constant(5.0));
  const Value quotient =
      builder.float64_divide(numerator, builder.float64_constant(2.0));
  expect(builder.set_return(quotient).ok(),
         "Float64 folding fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept constant Float64 SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.output_nodes == 1 &&
             optimization.stats.constants_folded == 2,
         "optimizer must fold chained Float64 arithmetic to one constant");
  const auto interpreted =
      Interpreter::evaluate(optimization.function, nullptr, 0);
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(8.5),
         "folded Float64 constants must preserve result bits");
}

void test_control_flow_counted_loop() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);

  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {one, zero}).ok(),
         "entry must jump to the loop header");

  expect(builder.set_insertion_block(loop).ok(),
         "loop block must accept insertion");
  const Value index = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value next_sum = builder.add(sum, index);
  const Value next_index = builder.add(index, one);
  const Value continue_loop =
      builder.less_equal(next_index, builder.parameter(0));
  expect(
      builder
          .branch(continue_loop, loop, {next_index, next_sum}, exit, {next_sum})
          .ok(),
      "loop latch must branch with explicit block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "exit block must accept insertion");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "exit block must return its block parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "counted-loop CFG must satisfy dominance and edge invariants");
  const std::array<Word, 1> args = {100};
  const auto result = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(result.ok() && result.value == 5050,
         "CFG interpreter must execute loop-carried block parameters");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "counted-loop CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == result.value,
           "native counted loop must match the CFG interpreter");
  }
}

void test_control_flow_float64_loop() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block loop = builder.create_block(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block exit =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1)}).ok(),
         "typed CFG entry must pass Word and Float64 parameters");

  expect(builder.set_insertion_block(loop).ok(),
         "typed Float64 loop block must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value accumulator = builder.block_parameter(loop, 1);
  const Value scaled = builder.float64_multiply(
      accumulator, builder.float64_constant(1.5));
  const Value adjusted = builder.float64_subtract(
      scaled, builder.float64_constant(0.25));
  const Value divided =
      builder.float64_divide(adjusted, builder.float64_constant(2.0));
  const Value next_accumulator =
      builder.float64_add(divided, builder.float64_constant(0.5));
  const Value next_remaining = builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  expect(builder
             .branch(continues, loop,
                     {next_remaining, next_accumulator}, exit,
                     {next_accumulator})
             .ok(),
         "typed Float64 loop must preserve edge types");

  expect(builder.set_insertion_block(exit).ok(),
         "typed Float64 exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "typed Float64 exit must return its block parameter");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 CFG must satisfy verifier invariants");

  double expected = 1.0;
  for (int iteration = 0; iteration < 4; ++iteration) {
    expected = ((expected * 1.5) - 0.25) / 2.0 + 0.5;
  }
  const std::array<Word, 2> arguments = {
      4, unijit::ir::pack_float64(1.0)};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(expected),
         "typed CFG interpreter must preserve Float64 loop values");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "typed Float64 CFG must compile to native code");
  if (compilation.ok()) {
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native typed Float64 CFG must match the interpreter");
  }
}

void test_control_flow_rejects_mixed_edge_types() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(!builder.jump(target, {builder.constant(1)}).ok(),
         "CFG builder must reject a Word edge argument for Float64 parameter");
}

void test_control_flow_float64_comparisons() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value less =
      builder.float64_less_than(builder.parameter(0), builder.parameter(1));
  const Value less_equal =
      builder.float64_less_equal(builder.parameter(0), builder.parameter(1));
  const Value encoded =
      builder.add(builder.multiply(less, builder.constant(10)), less_equal);
  expect(builder.set_return(encoded).ok(),
         "Float64 CFG comparison fixture must return its encoded flags");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 CFG comparisons must satisfy type verification");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 CFG comparisons must compile");

  const auto check = [&](double lhs, double rhs, Word expected,
                         const char *message) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == expected, message);
    if (compilation.ok()) {
      const auto native =
          compilation.function->invoke(arguments.data(), arguments.size());
      expect(native.ok() && native.value == interpreted.value,
             "native Float64 CFG comparison must match the interpreter");
    }
  };
  check(1.0, 2.0, 11, "less operands must set both comparison flags");
  check(2.0, 2.0, 1, "equal operands must only set the inclusive flag");
  check(3.0, 2.0, 0, "greater operands must clear both comparison flags");
  check(std::numeric_limits<double>::quiet_NaN(), 2.0, 0,
        "unordered operands must clear both comparison flags");
}

void test_control_flow_merge() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block take_lhs = builder.create_block(0);
  const unijit::ir::Block take_rhs = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition =
      builder.less_than(builder.parameter(0), builder.parameter(1));
  expect(builder.branch(condition, take_rhs, {}, take_lhs, {}).ok(),
         "entry comparison must branch to both arms");

  expect(builder.set_insertion_block(take_lhs).ok(),
         "left arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(0)}).ok(),
         "left arm must pass its result to the merge");
  expect(builder.set_insertion_block(take_rhs).ok(),
         "right arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(1)}).ok(),
         "right arm must pass its result to the merge");
  expect(builder.set_insertion_block(merge).ok(),
         "merge block must accept insertion");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "merge must return its explicit SSA parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "diamond CFG must pass the dominance verifier");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {17, 91}).value ==
          91,
      "true branch must select the right operand");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {117, -4}).value ==
          117,
      "false branch must select the left operand");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "diamond CFG must compile to native code");
  if (compilation.ok()) {
    const std::array<Word, 2> true_args = {17, 91};
    const std::array<Word, 2> false_args = {117, -4};
    expect(compilation.function->invoke(true_args.data(), true_args.size())
                   .value == 91,
           "native true branch must select the right operand");
    expect(compilation.function->invoke(false_args.data(), false_args.size())
                   .value == 117,
           "native false branch must select the left operand");
  }
}

void test_control_flow_parallel_edge_copy() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block loop = builder.create_block(3);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  const Value two = builder.constant(2);
  const Value hundred = builder.constant(100);
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1), two})
             .ok(),
         "parallel-copy fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "parallel-copy loop block must exist");
  const Value lhs = builder.block_parameter(loop, 0);
  const Value rhs = builder.block_parameter(loop, 1);
  const Value remaining = builder.block_parameter(loop, 2);
  const Value encoded = builder.add(builder.multiply(lhs, hundred), rhs);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {rhs, lhs, next_remaining}, exit,
                     {encoded})
             .ok(),
         "backedge must support swapped block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "parallel-copy exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "parallel-copy exit must return its encoded pair");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  const std::array<Word, 2> args = {3, 7};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 703,
         "interpreter edge copies must be parallel");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "parallel-copy CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == interpreted.value,
           "native edge copies must preserve swapped loop parameters");
  }
}

void test_control_flow_rejects_non_dominating_value() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block left = builder.create_block(0);
  const unijit::ir::Block right = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition = builder.constant(1);
  expect(builder.branch(condition, left, {}, right, {}).ok(),
         "invalid-dominance fixture entry must be terminated");

  expect(builder.set_insertion_block(left).ok(),
         "invalid-dominance left block must exist");
  const Value leaked = builder.constant(7);
  expect(builder.jump(merge, {leaked}).ok(), "left block must reach the merge");

  expect(builder.set_insertion_block(right).ok(),
         "invalid-dominance right block must exist");
  const Value invalid = builder.add(leaked, builder.constant(1));
  expect(builder.jump(merge, {invalid}).ok(),
         "right block must reach the merge");

  expect(builder.set_insertion_block(merge).ok(),
         "invalid-dominance merge block must exist");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "invalid-dominance merge must be terminated");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(!unijit::ir::verify(function).ok(),
         "CFG verifier must reject a sibling-block SSA use");
}

void test_control_flow_safepoint() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0), zero}).ok(),
         "CFG safepoint fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "CFG safepoint loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value safepoint = builder.safepoint(314);
  expect(safepoint.valid(), "CFG safepoint must be inserted in the loop");
  const Value next_sum = builder.add(builder.add(sum, remaining), safepoint);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {next_remaining, next_sum}, exit,
                     {next_sum})
             .ok(),
         "CFG safepoint loop must branch to its backedge and exit");

  expect(builder.set_insertion_block(exit).ok(),
         "CFG safepoint exit must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "CFG safepoint exit must return its sum");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "CFG safepoint IR must verify");

  const std::array<Word, 1> args = {4};
  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size(), 100, &context);
  expect(!interpreted.ok() &&
             interpreted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interpreted.status.location() == 314,
         "CFG interpreter must exit at the requested loop safepoint");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "CFG safepoint loop must compile");
  if (compilation.ok()) {
    expect(compilation.function->requires_context(),
           "compiled CFG safepoints must require an execution context");
    context.request_interrupt();
    const auto interrupted =
        compilation.function->invoke(args.data(), args.size(), &context);
    expect(!interrupted.ok() &&
               interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               interrupted.status.location() == 314 &&
               context.exit_site() == 314,
           "native CFG loop must exit at the requested safepoint");

    context.clear_interrupt();
    const auto completed =
        compilation.function->invoke(args.data(), args.size(), &context);
    expect(completed.ok() && completed.value == 10,
           "native CFG loop must continue when interruption is clear");
    const auto completed_with_local_context =
        compilation.function->invoke(args.data(), args.size());
    expect(completed_with_local_context.ok() &&
               completed_with_local_context.value == 10,
           "CFG invocation must provision a local safepoint context");
    expect(compilation.function->native_entry()(args.data(), nullptr) == 10,
           "null execution contexts must bypass CFG safepoints");
  }
}

void test_control_flow_execution_budget() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block loop = builder.create_block(0);
  expect(builder.jump(loop, {}).ok(), "entry must reach the infinite loop");
  expect(builder.set_insertion_block(loop).ok(),
         "infinite-loop block must exist");
  expect(builder.jump(loop, {}).ok(), "loop must jump to itself");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "self-loop must be a valid control-flow graph");
  const auto result =
      unijit::ir::ControlFlowInterpreter::evaluate(function, nullptr, 0, 10);
  expect(!result.ok() &&
             result.status.code() == unijit::StatusCode::kResourceExhausted,
         "CFG interpreter must stop when its execution budget is exhausted");
}

void test_control_flow_builder_rejects_edge_arity() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target = builder.create_block(1);
  expect(!builder.jump(target, {}).ok(),
         "builder must reject an edge with missing block arguments");
}

}  // namespace

int main() {
  test_verifier_rejects_forward_reference();
  test_constant_native_function();
  test_execution_context_lifecycle();
  test_safepoint_ir_and_interpreter();
  test_differential_arithmetic();
  test_float64_ir_and_interpreter();
  test_float64_division();
  test_float64_nonzero_guard();
  test_constant_float64_nonzero_guard_elimination();
  test_verifier_rejects_mixed_arithmetic();
  test_float64_spill_path();
  test_float64_preserves_host_abi();
  test_runtime_helper_call();
  test_effectful_dead_runtime_call();
  test_float64_runtime_helper_call();
  test_verifier_rejects_null_runtime_helper();
  test_spill_path();
  test_argument_validation();
  test_optimization_pipeline();
  test_float64_constant_folding();
  test_control_flow_counted_loop();
  test_control_flow_float64_loop();
  test_control_flow_rejects_mixed_edge_types();
  test_control_flow_float64_comparisons();
  test_control_flow_merge();
  test_control_flow_parallel_edge_copy();
  test_control_flow_rejects_non_dominating_value();
  test_control_flow_safepoint();
  test_control_flow_execution_budget();
  test_control_flow_builder_rejects_edge_arity();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all UniJIT tests passed\n";
  return EXIT_SUCCESS;
}
