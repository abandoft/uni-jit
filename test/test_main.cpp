#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

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
    const std::array<Word, 2> args = {
        static_cast<Word>(random()), static_cast<Word>(random())};
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
  expect(builder.set_return(values.front()).ok(), "spill return must be accepted");
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
  const auto interpreted = Interpreter::evaluate(optimization.function, args.data(),
                                                 args.size());
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

}  // namespace

int main() {
  test_verifier_rejects_forward_reference();
  test_constant_native_function();
  test_differential_arithmetic();
  test_spill_path();
  test_argument_validation();
  test_optimization_pipeline();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all UniJIT tests passed\n";
  return EXIT_SUCCESS;
}
