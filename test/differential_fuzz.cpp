#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
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

using unijit::ir::ControlFlowBuilder;
using unijit::ir::ControlFlowFunction;
using unijit::ir::ControlFlowInterpreter;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::Value;
using unijit::ir::ValueType;
using unijit::ir::VectorBinaryOperation;
using unijit::ir::VectorComparison;
using unijit::ir::VectorExtension;
using unijit::ir::VectorHalf;
using unijit::ir::VectorUnaryOperation;
using unijit::ir::Word;

struct Options final {
  std::uint64_t seed{0x554E494A4954465AULL};
  std::size_t programs{128};
  std::size_t inputs{64};
  std::size_t nodes{48};
};

Word word_from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint64_t word_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool report_mismatch(const char* tier, std::uint64_t seed,
                     std::size_t program, std::size_t input,
                     const unijit::ir::EvaluationResult& interpreted,
                     const unijit::ir::EvaluationResult& native) {
  std::cerr << tier << " mismatch: seed=0x" << std::hex << seed << std::dec
            << " program=" << program << " input=" << input
            << " interpreted_status="
            << static_cast<unsigned>(interpreted.status.code())
            << " native_status=" << static_cast<unsigned>(native.status.code())
            << " interpreted_bits=0x" << std::hex
            << word_bits(interpreted.value) << " native_bits=0x"
            << word_bits(native.value) << std::dec << '\n';
  return false;
}

double random_input_double(std::mt19937_64* random) {
  const auto scaled = static_cast<std::int64_t>((*random)() % 200001ULL) -
                      100000;
  return static_cast<double>(scaled) / 1000.0;
}

double random_constant_double(std::mt19937_64* random) {
  constexpr std::array<double, 10> kConstants = {
      -2.0, -1.5, -1.0, -0.5, -0.25, 0.25, 0.5, 1.0, 1.5, 2.0};
  return kConstants[(*random)() % kConstants.size()];
}

bool fuzz_word_function(std::mt19937_64* random, const Options& options,
                        std::size_t program) {
  constexpr std::size_t kParameterCount = 4;
  FunctionBuilder builder(kParameterCount);
  std::vector<Value> values;
  values.reserve(kParameterCount + options.nodes * 2);
  for (std::size_t index = 0; index < kParameterCount; ++index) {
    values.push_back(builder.parameter(index));
  }

  for (std::size_t index = 0; index < options.nodes; ++index) {
    if (((*random)() & 7ULL) == 0) {
      values.push_back(builder.constant(word_from_bits((*random)())));
      continue;
    }
    const Value lhs = values[(*random)() % values.size()];
    const Value rhs = values[(*random)() % values.size()];
    switch ((*random)() % 15ULL) {
      case 0:
        values.push_back(builder.add(lhs, rhs));
        break;
      case 1:
        values.push_back(builder.subtract(lhs, rhs));
        break;
      case 2:
        values.push_back(builder.multiply(lhs, rhs));
        break;
      case 3:
        values.push_back(builder.negate(lhs));
        break;
      case 4:
        values.push_back(builder.bitwise_not(lhs));
        break;
      case 5:
        values.push_back(builder.bitwise_and(lhs, rhs));
        break;
      case 6:
        values.push_back(builder.bitwise_or(lhs, rhs));
        break;
      case 7:
        values.push_back(builder.bitwise_xor(lhs, rhs));
        break;
      case 8:
        values.push_back(builder.shift_left(lhs, rhs));
        break;
      case 9:
        values.push_back(builder.floor_divide(lhs, rhs));
        break;
      case 10:
        values.push_back(builder.floor_modulo(lhs, rhs));
        break;
      case 11:
        values.push_back(builder.less_than(lhs, rhs));
        break;
      case 12:
        values.push_back(builder.less_equal(lhs, rhs));
        break;
      case 13:
        values.push_back(builder.equal(lhs, rhs));
        break;
      default:
        values.push_back(builder.not_equal(lhs, rhs));
        break;
    }
  }
  if (!builder.set_return(values.back()).ok()) {
    std::cerr << "unable to terminate generated Word function\n";
    return false;
  }
  const Function function = std::move(builder).build();
  const auto verification = unijit::ir::verify(function);
  if (!verification.ok()) {
    std::cerr << "generated Word function failed verification: "
              << verification.message() << '\n';
    return false;
  }
  auto compilation = unijit::jit::Compiler::compile(function);
  if (!compilation.ok()) {
    std::cerr << "generated Word function failed compilation: "
              << compilation.status.message() << '\n';
    return false;
  }

  for (std::size_t input = 0; input < options.inputs; ++input) {
    std::array<Word, kParameterCount> arguments{};
    for (Word& argument : arguments) {
      argument = word_from_bits((*random)());
    }
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      return report_mismatch("straight-line Word", options.seed, program,
                             input, interpreted, native);
    }
  }
  return true;
}

bool fuzz_float64_function(std::mt19937_64* random, const Options& options,
                           std::size_t program) {
  constexpr std::size_t kParameterCount = 3;
  FunctionBuilder builder(
      std::vector<ValueType>(kParameterCount, ValueType::kFloat64));
  std::vector<Value> values;
  values.reserve(kParameterCount + options.nodes);
  for (std::size_t index = 0; index < kParameterCount; ++index) {
    values.push_back(builder.parameter(index));
  }
  for (std::size_t index = 0; index < options.nodes; ++index) {
    const Value lhs = values[(*random)() % values.size()];
    const Value rhs = builder.float64_constant(random_constant_double(random));
    switch ((*random)() % 5ULL) {
      case 0:
        values.push_back(builder.float64_add(lhs, rhs));
        break;
      case 1:
        values.push_back(builder.float64_subtract(lhs, rhs));
        break;
      case 2:
        values.push_back(builder.float64_multiply(lhs, rhs));
        break;
      case 3:
        values.push_back(builder.float64_divide(lhs, rhs));
        break;
      default:
        values.push_back(builder.float64_negate(lhs));
        break;
    }
  }
  if (!builder.set_return(values.back()).ok()) {
    std::cerr << "unable to terminate generated Float64 function\n";
    return false;
  }
  const Function function = std::move(builder).build();
  const auto verification = unijit::ir::verify(function);
  if (!verification.ok()) {
    std::cerr << "generated Float64 function failed verification: "
              << verification.message() << '\n';
    return false;
  }
  auto compilation = unijit::jit::Compiler::compile(function);
  if (!compilation.ok()) {
    std::cerr << "generated Float64 function failed compilation: "
              << compilation.status.message() << '\n';
    return false;
  }

  for (std::size_t input = 0; input < options.inputs; ++input) {
    std::array<Word, kParameterCount> arguments{};
    for (Word& argument : arguments) {
      argument = unijit::ir::pack_float64(random_input_double(random));
    }
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      return report_mismatch("straight-line Float64", options.seed, program,
                             input, interpreted, native);
    }
  }
  return true;
}

std::vector<Value> make_cfg_arm(ControlFlowBuilder* builder,
                                const std::vector<Value>& state,
                                bool float64, std::mt19937_64* random) {
  std::vector<Value> result;
  result.reserve(state.size());
  const std::size_t shift = 1 + ((*random)() % state.size());
  for (std::size_t index = 0; index < state.size(); ++index) {
    const Value source = state[(index + shift) % state.size()];
    if (float64) {
      const Value constant =
          builder->float64_constant(random_constant_double(random));
      switch ((*random)() % 3ULL) {
      case 0:
        result.push_back(builder->float64_add(source, constant));
        break;
      case 1:
        result.push_back(builder->float64_multiply(source, constant));
        break;
      default:
        result.push_back(builder->float64_negate(source));
        break;
      }
    } else {
      const Word constant_value =
          static_cast<Word>(static_cast<std::int64_t>((*random)() % 17ULL) - 8);
      const Value constant = builder->constant(constant_value);
      switch ((*random)() % 15ULL) {
        case 0:
          result.push_back(builder->add(source, constant));
          break;
        case 1:
          result.push_back(builder->subtract(source, constant));
          break;
        case 2:
          result.push_back(builder->multiply(source, constant));
          break;
        case 3:
          result.push_back(builder->negate(source));
          break;
        case 4:
          result.push_back(builder->bitwise_not(source));
          break;
        case 5:
          result.push_back(builder->bitwise_and(source, constant));
          break;
        case 6:
          result.push_back(builder->bitwise_or(source, constant));
          break;
        case 7:
          result.push_back(builder->bitwise_xor(source, constant));
          break;
        case 8:
          result.push_back(builder->shift_left(source, constant));
          break;
        case 9:
          result.push_back(builder->floor_divide(source, constant));
          break;
        case 10:
          result.push_back(builder->floor_modulo(source, constant));
          break;
        case 11:
          result.push_back(builder->less_than(source, constant));
          break;
        case 12:
          result.push_back(builder->less_equal(source, constant));
          break;
        case 13:
          result.push_back(builder->equal(source, constant));
          break;
        default:
          result.push_back(builder->not_equal(source, constant));
          break;
      }
    }
  }
  return result;
}

bool fuzz_cfg_function(std::mt19937_64* random, const Options& options,
                       std::size_t program) {
  const bool float64 = (program & 1U) != 0;
  const std::size_t state_count = 1 + ((*random)() % 12ULL);
  std::vector<ValueType> parameter_types(1 + state_count,
                                         float64 ? ValueType::kFloat64
                                                 : ValueType::kWord);
  parameter_types[0] = ValueType::kWord;
  ControlFlowBuilder builder(parameter_types);
  const auto loop = builder.create_block(parameter_types);
  const auto true_arm = builder.create_block(0);
  const auto false_arm = builder.create_block(0);
  const auto merge = builder.create_block(parameter_types);
  const auto exit = builder.create_block(
      std::vector<ValueType>{float64 ? ValueType::kFloat64
                                    : ValueType::kWord});

  std::vector<Value> entry_arguments;
  entry_arguments.reserve(parameter_types.size());
  for (std::size_t index = 0; index < parameter_types.size(); ++index) {
    entry_arguments.push_back(builder.parameter(index));
  }
  if (!builder.jump(loop, entry_arguments).ok() ||
      !builder.set_insertion_block(loop).ok()) {
    std::cerr << "unable to create generated CFG entry\n";
    return false;
  }

  const Value remaining = builder.block_parameter(loop, 0);
  std::vector<Value> state;
  state.reserve(state_count);
  for (std::size_t index = 0; index < state_count; ++index) {
    state.push_back(builder.block_parameter(loop, index + 1));
  }
  Value branch_condition;
  if (float64) {
    const Value comparison =
        builder.float64_constant(random_constant_double(random));
    switch ((*random)() % 4ULL) {
      case 0:
        branch_condition = builder.float64_less_than(state[0], comparison);
        break;
      case 1:
        branch_condition = builder.float64_less_equal(state[0], comparison);
        break;
      case 2:
        branch_condition = builder.float64_equal(state[0], comparison);
        break;
      default:
        branch_condition = builder.float64_not_equal(state[0], comparison);
        break;
    }
  } else {
    branch_condition = builder.less_than(
        state[0], builder.constant(static_cast<Word>((*random)() % 257ULL)));
  }
  if (!builder.branch(branch_condition, true_arm, {}, false_arm, {}).ok()) {
    std::cerr << "unable to branch generated CFG loop\n";
    return false;
  }

  if (!builder.set_insertion_block(true_arm).ok()) {
    return false;
  }
  std::vector<Value> true_arguments{remaining};
  auto true_state = make_cfg_arm(&builder, state, float64, random);
  true_arguments.insert(true_arguments.end(), true_state.begin(),
                        true_state.end());
  if (!builder.jump(merge, true_arguments).ok() ||
      !builder.set_insertion_block(false_arm).ok()) {
    std::cerr << "unable to create generated CFG true edge\n";
    return false;
  }
  std::vector<Value> false_arguments{remaining};
  auto false_state = make_cfg_arm(&builder, state, float64, random);
  false_arguments.insert(false_arguments.end(), false_state.begin(),
                         false_state.end());
  if (!builder.jump(merge, false_arguments).ok() ||
      !builder.set_insertion_block(merge).ok()) {
    std::cerr << "unable to create generated CFG false edge\n";
    return false;
  }

  const Value next_remaining = builder.subtract(
      builder.block_parameter(merge, 0), builder.constant(1));
  (void)builder.safepoint(0xF000U + program);
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  std::vector<Value> backedge_arguments{next_remaining};
  for (std::size_t index = 0; index < state_count; ++index) {
    backedge_arguments.push_back(builder.block_parameter(merge, index + 1));
  }
  const std::size_t result_index = (*random)() % state_count;
  const Value result = builder.block_parameter(merge, result_index + 1);
  if (!builder
           .branch(continues, loop, backedge_arguments, exit, {result})
           .ok() ||
      !builder.set_insertion_block(exit).ok() ||
      !builder.set_return(builder.block_parameter(exit, 0)).ok()) {
    std::cerr << "unable to terminate generated CFG\n";
    return false;
  }

  const ControlFlowFunction function = std::move(builder).build();
  const auto verification = unijit::ir::verify(function);
  if (!verification.ok()) {
    std::cerr << "generated CFG failed verification: "
              << verification.message() << '\n';
    return false;
  }
  auto compilation = unijit::jit::Compiler::compile(function);
  if (!compilation.ok()) {
    std::cerr << "generated CFG failed compilation: "
              << compilation.status.message() << '\n';
    return false;
  }
  if (!compilation.function->requires_context()) {
    std::cerr << "generated CFG lost mandatory safepoint context metadata\n";
    return false;
  }

  for (std::size_t input = 0; input < options.inputs; ++input) {
    std::vector<Word> arguments(parameter_types.size());
    arguments[0] = static_cast<Word>((*random)() % 25ULL);
    for (std::size_t index = 0; index < state_count; ++index) {
      arguments[index + 1] =
          float64 ? unijit::ir::pack_float64(random_input_double(random))
                  : word_from_bits((*random)());
    }
    const auto interpreted = ControlFlowInterpreter::evaluate(
        function, arguments.data(), arguments.size(), 1024);
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      return report_mismatch(float64 ? "CFG Float64" : "CFG Word",
                             options.seed, program, input, interpreted, native);
    }
  }
  return true;
}

struct VectorProgram final {
  ValueType type{ValueType::kI8x16};
  VectorBinaryOperation arithmetic{VectorBinaryOperation::kAdd};
  VectorComparison comparison{VectorComparison::kEqual};
  VectorExtension extension{VectorExtension::kZero};
  VectorHalf half{VectorHalf::kLow};
  std::size_t lane{0};
  bool sign_extend{false};
  std::vector<std::uint8_t> shuffle;
};

ValueType widened_type(ValueType type) noexcept {
  switch (type) {
    case ValueType::kI8x16:
      return ValueType::kI16x8;
    case ValueType::kI16x8:
      return ValueType::kI32x4;
    case ValueType::kI32x4:
      return ValueType::kI64x2;
    case ValueType::kWord:
    case ValueType::kFloat64:
    case ValueType::kI64x2:
    case ValueType::kF32x4:
    case ValueType::kF64x2:
    case ValueType::kMask8x16:
    case ValueType::kMask16x8:
    case ValueType::kMask32x4:
    case ValueType::kMask64x2:
      return ValueType::kWord;
  }
  return ValueType::kWord;
}

VectorProgram random_vector_program(std::mt19937_64* random) {
  constexpr std::array<ValueType, 6> kTypes = {
      ValueType::kI8x16, ValueType::kI16x8, ValueType::kI32x4,
      ValueType::kI64x2, ValueType::kF32x4, ValueType::kF64x2};
  constexpr std::array<VectorBinaryOperation, 4> kFloatArithmetic = {
      VectorBinaryOperation::kAdd, VectorBinaryOperation::kSubtract,
      VectorBinaryOperation::kMultiply, VectorBinaryOperation::kDivide};
  constexpr std::array<VectorBinaryOperation, 3> kIntegerArithmetic = {
      VectorBinaryOperation::kAdd, VectorBinaryOperation::kSubtract,
      VectorBinaryOperation::kMultiply};
  constexpr std::array<VectorComparison, 3> kFloatComparisons = {
      VectorComparison::kOrderedFloatEqual,
      VectorComparison::kOrderedFloatLessThan,
      VectorComparison::kOrderedFloatLessEqual};
  constexpr std::array<VectorComparison, 5> kIntegerComparisons = {
      VectorComparison::kEqual, VectorComparison::kSignedLessThan,
      VectorComparison::kSignedLessEqual,
      VectorComparison::kUnsignedLessThan,
      VectorComparison::kUnsignedLessEqual};

  VectorProgram result;
  result.type = kTypes[(*random)() % kTypes.size()];
  if (unijit::ir::is_float_vector_type(result.type)) {
    result.arithmetic =
        kFloatArithmetic[(*random)() % kFloatArithmetic.size()];
    result.comparison =
        kFloatComparisons[(*random)() % kFloatComparisons.size()];
  } else {
    result.arithmetic =
        kIntegerArithmetic[(*random)() % kIntegerArithmetic.size()];
    result.comparison =
        kIntegerComparisons[(*random)() % kIntegerComparisons.size()];
  }
  const std::size_t lanes = unijit::ir::vector_lane_count(result.type);
  result.lane = (*random)() % lanes;
  result.sign_extend =
      unijit::ir::is_integer_vector_type(result.type) &&
      (((*random)() & 1ULL) != 0);
  result.extension = ((*random)() & 1ULL) == 0
                         ? VectorExtension::kZero
                         : VectorExtension::kSign;
  result.half = ((*random)() & 1ULL) == 0 ? VectorHalf::kLow
                                          : VectorHalf::kHigh;
  result.shuffle.resize(lanes);
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    result.shuffle[lane] = static_cast<std::uint8_t>(lane);
  }
  std::shuffle(result.shuffle.begin(), result.shuffle.end(), *random);
  return result;
}

template <typename Builder>
Value build_vector_projection(Builder* builder, const VectorProgram& program,
                              Value lhs, Value rhs_lane) {
  const Value rhs = builder->vector_splat(program.type, rhs_lane);
  const Value arithmetic =
      builder->vector_binary(program.arithmetic, lhs, rhs);
  const Value inserted =
      builder->vector_insert_lane(arithmetic, program.lane, rhs_lane);
  const Value mask =
      builder->vector_compare(program.comparison, inserted, lhs);
  const Value inverted_mask = builder->vector_unary(
      VectorUnaryOperation::kBitwiseNot, mask);
  const Value roundtrip_mask = builder->vector_unary(
      VectorUnaryOperation::kBitwiseNot, inverted_mask);
  const Value selected_mask = builder->vector_binary(
      VectorBinaryOperation::kBitwiseAnd, mask, roundtrip_mask);
  const Value selected =
      builder->vector_select(selected_mask, inserted, rhs);
  const Value zero = builder->vector_zero(program.type);
  const Value bitwise = builder->vector_binary(
      VectorBinaryOperation::kBitwiseXor, selected, zero);
  const Value shuffled = builder->vector_shuffle(bitwise, program.shuffle);
  Value projection = builder->vector_extract_lane(
      shuffled, program.lane, program.sign_extend);
  projection =
      builder->add(projection, builder->vector_lane_sign_mask(shuffled));

  const ValueType widened = widened_type(program.type);
  if (widened != ValueType::kWord) {
    const Value extended = builder->vector_widen(
        selected, widened, program.extension, program.half);
    projection = builder->add(
        projection, builder->vector_extract_lane(extended, 0, false));
  }
  return projection;
}

bool fuzz_vector_function(std::mt19937_64* random, const Options& options,
                          std::size_t program_index) {
  const VectorProgram program = random_vector_program(random);

  FunctionBuilder straight_builder(2);
  const Value straight_lhs = straight_builder.vector_splat(
      program.type, straight_builder.parameter(0));
  const Value straight_result = build_vector_projection(
      &straight_builder, program, straight_lhs, straight_builder.parameter(1));
  if (!straight_builder.set_return(straight_result).ok()) {
    std::cerr << "unable to terminate generated straight-line vector IR\n";
    return false;
  }
  const Function straight = std::move(straight_builder).build();
  const auto straight_verification = unijit::ir::verify(straight);
  if (!straight_verification.ok()) {
    std::cerr << "generated straight-line vector IR failed verification: "
              << straight_verification.message() << '\n';
    return false;
  }
  const auto optimized_straight = unijit::ir::Optimizer::run(straight);
  if (!optimized_straight.ok()) {
    std::cerr << "generated straight-line vector IR failed optimization: "
              << optimized_straight.status.message() << '\n';
    return false;
  }

  ControlFlowBuilder cfg_builder(2);
  const Value entry_vector =
      cfg_builder.vector_splat(program.type, cfg_builder.parameter(0));
  const auto merge = cfg_builder.create_block({program.type});
  if (!cfg_builder.jump(merge, {entry_vector}).ok() ||
      !cfg_builder.set_insertion_block(merge).ok()) {
    std::cerr << "unable to create generated vector CFG edge\n";
    return false;
  }
  const Value cfg_result = build_vector_projection(
      &cfg_builder, program, cfg_builder.block_parameter(merge, 0),
      cfg_builder.parameter(1));
  if (!cfg_builder.set_return(cfg_result).ok()) {
    std::cerr << "unable to terminate generated vector CFG\n";
    return false;
  }
  const ControlFlowFunction cfg = std::move(cfg_builder).build();
  const auto cfg_verification = unijit::ir::verify(cfg);
  if (!cfg_verification.ok()) {
    std::cerr << "generated vector CFG failed verification: "
              << cfg_verification.message() << '\n';
    return false;
  }
  const auto optimized_cfg = unijit::ir::Optimizer::run(cfg);
  if (!optimized_cfg.ok()) {
    std::cerr << "generated vector CFG failed optimization: "
              << optimized_cfg.status.message() << '\n';
    return false;
  }

  for (std::size_t input = 0; input < options.inputs; ++input) {
    const std::array<Word, 2> arguments = {
        word_from_bits((*random)()), word_from_bits((*random)())};
    const auto straight_result_value =
        Interpreter::evaluate(straight, arguments.data(), arguments.size());
    const auto optimized_straight_value = Interpreter::evaluate(
        optimized_straight.function, arguments.data(), arguments.size());
    if (!straight_result_value.ok() || !optimized_straight_value.ok() ||
        straight_result_value.value != optimized_straight_value.value) {
      return report_mismatch("straight-line SIMD optimizer", options.seed,
                             program_index, input, straight_result_value,
                             optimized_straight_value);
    }

    const auto cfg_result_value = ControlFlowInterpreter::evaluate(
        cfg, arguments.data(), arguments.size());
    const auto optimized_cfg_value = ControlFlowInterpreter::evaluate(
        optimized_cfg.function, arguments.data(), arguments.size());
    if (!cfg_result_value.ok() || !optimized_cfg_value.ok() ||
        cfg_result_value.value != optimized_cfg_value.value) {
      return report_mismatch("CFG SIMD optimizer", options.seed,
                             program_index, input, cfg_result_value,
                             optimized_cfg_value);
    }
    if (straight_result_value.value != cfg_result_value.value) {
      return report_mismatch("straight-line/CFG SIMD", options.seed,
                             program_index, input, straight_result_value,
                             cfg_result_value);
    }
  }

  FunctionBuilder constant_builder(0);
  const Value lhs_bits = constant_builder.constant(word_from_bits((*random)()));
  const Value rhs_bits = constant_builder.constant(word_from_bits((*random)()));
  const Value constant_lhs =
      constant_builder.vector_splat(program.type, lhs_bits);
  const Value constant_result = build_vector_projection(
      &constant_builder, program, constant_lhs, rhs_bits);
  if (!constant_builder.set_return(constant_result).ok()) {
    std::cerr << "unable to terminate generated constant vector IR\n";
    return false;
  }
  const Function constant_function = std::move(constant_builder).build();
  const auto folded = unijit::ir::Optimizer::run(constant_function);
  const auto unfolded_value =
      Interpreter::evaluate(constant_function, nullptr, 0);
  const auto folded_value =
      folded.ok() ? Interpreter::evaluate(folded.function, nullptr, 0)
                  : unijit::ir::EvaluationResult{};
  if (!folded.ok() || folded.stats.constants_folded == 0 ||
      !unfolded_value.ok() || !folded_value.ok() ||
      unfolded_value.value != folded_value.value) {
    return report_mismatch("constant SIMD folding", options.seed,
                           program_index, 0, unfolded_value, folded_value);
  }
  return true;
}

bool parse_size(const char* text, std::size_t* result) {
  try {
    const std::uint64_t value = std::stoull(text, nullptr, 0);
    if (value == 0 || value > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *result = static_cast<std::size_t>(value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return false;
    }
    const std::string name = argv[index];
    if (name == "--seed") {
      try {
        options->seed = std::stoull(argv[index + 1], nullptr, 0);
      } catch (const std::exception&) {
        return false;
      }
    } else if (name == "--programs") {
      if (!parse_size(argv[index + 1], &options->programs)) {
        return false;
      }
    } else if (name == "--inputs") {
      if (!parse_size(argv[index + 1], &options->inputs)) {
        return false;
      }
    } else if (name == "--nodes") {
      if (!parse_size(argv[index + 1], &options->nodes)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return options->programs <= 10000 && options->inputs <= 100000 &&
         options->nodes <= 10000;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_differential_fuzz [--seed N] "
                 "[--programs N] [--inputs N] [--nodes N]\n";
    return EXIT_FAILURE;
  }

  std::mt19937_64 random(options.seed);
  for (std::size_t program = 0; program < options.programs; ++program) {
    if (!fuzz_word_function(&random, options, program) ||
        !fuzz_float64_function(&random, options, program) ||
        !fuzz_cfg_function(&random, options, program) ||
        !fuzz_vector_function(&random, options, program)) {
      return EXIT_FAILURE;
    }
  }
  std::cout << "{\"schema\":\"unijit.differential-fuzz.v2\","
            << "\"seed\":\"0x" << std::hex << options.seed << std::dec
            << "\",\"programs\":" << options.programs
            << ",\"inputs_per_program\":" << options.inputs
            << ",\"nodes_per_straight_line_program\":" << options.nodes
            << ",\"strict_simd\":true"
            << "}\n";
  return EXIT_SUCCESS;
}
