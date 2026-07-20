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
  test_differential_arithmetic();
  test_float64_ir_and_interpreter();
  test_verifier_rejects_mixed_arithmetic();
  test_float64_spill_path();
  test_spill_path();
  test_argument_validation();
  test_optimization_pipeline();
  test_control_flow_counted_loop();
  test_control_flow_merge();
  test_control_flow_parallel_edge_copy();
  test_control_flow_rejects_non_dominating_value();
  test_control_flow_execution_budget();
  test_control_flow_builder_rejects_edge_arity();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all UniJIT tests passed\n";
  return EXIT_SUCCESS;
}
