#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/jit/compiler.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::ControlFlowBuilder;
using unijit::ir::ControlFlowFunction;
using unijit::ir::Value;
using unijit::ir::ValueType;
using unijit::ir::Vector128;
using unijit::ir::VectorBinaryOperation;
using unijit::ir::Word;

struct Options final {
  std::size_t loop_iterations{1000};
  std::size_t warmup{100};
  std::size_t invocations{500};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_loop_iteration{0};
  std::uint64_t checksum{0};
};

std::uint64_t bits(Word value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Value combine_lanes(ControlFlowBuilder* builder,
                    const std::array<Value, 4>& lanes) {
  Value result = lanes[0];
  result = builder->bitwise_xor(
      result, builder->shift_left(lanes[1], builder->constant(13)));
  result = builder->bitwise_xor(
      result, builder->shift_left(lanes[2], builder->constant(27)));
  return builder->bitwise_xor(
      result, builder->shift_left(lanes[3], builder->constant(41)));
}

ControlFlowFunction make_vector_function() {
  ControlFlowBuilder builder(5);
  const auto loop =
      builder.create_block({ValueType::kWord, ValueType::kI32x4});
  const auto exit = builder.create_block({ValueType::kI32x4});

  Value initial =
      builder.vector_splat(ValueType::kI32x4, builder.parameter(1));
  for (std::size_t lane = 1; lane < 4; ++lane) {
    initial = builder.vector_insert_lane(initial, lane,
                                         builder.parameter(lane + 1));
  }

  Vector128 increment_bits;
  constexpr std::array<Word, 4> kIncrements = {1, 3, 5, 7};
  for (std::size_t lane = 0; lane < kIncrements.size(); ++lane) {
    increment_bits = unijit::ir::vector_insert_lane_bits(
        increment_bits, ValueType::kI32x4, lane, kIncrements[lane]);
  }
  const Value increments =
      builder.vector_constant(ValueType::kI32x4, increment_bits);
  if (!builder.jump(loop, {builder.parameter(0), initial}).ok() ||
      !builder.set_insertion_block(loop).ok()) {
    return {};
  }

  const Value remaining = builder.block_parameter(loop, 0);
  const Value state = builder.block_parameter(loop, 1);
  const Value added = builder.vector_binary(VectorBinaryOperation::kAdd,
                                             state, increments);
  const Value rotated = builder.vector_shuffle(added, {1, 2, 3, 0});
  const Value mixed = builder.vector_binary(VectorBinaryOperation::kBitwiseXor,
                                             added, rotated);
  const Value next = builder.vector_binary(VectorBinaryOperation::kAdd, mixed,
                                            increments);
  const Value next_remaining =
      builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  if (!builder.branch(continues, loop, {next_remaining, next}, exit, {next})
           .ok() ||
      !builder.set_insertion_block(exit).ok()) {
    return {};
  }

  const Value result = builder.block_parameter(exit, 0);
  std::array<Value, 4> lanes;
  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    lanes[lane] = builder.vector_extract_lane(result, lane, false);
  }
  if (!builder.set_return(combine_lanes(&builder, lanes)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

ControlFlowFunction make_scalar_function() {
  ControlFlowBuilder builder(5);
  const auto loop = builder.create_block(5);
  const auto exit = builder.create_block(4);
  if (!builder
           .jump(loop, {builder.parameter(0), builder.parameter(1),
                        builder.parameter(2), builder.parameter(3),
                        builder.parameter(4)})
           .ok() ||
      !builder.set_insertion_block(loop).ok()) {
    return {};
  }

  const Value remaining = builder.block_parameter(loop, 0);
  const Value mask = builder.constant(static_cast<Word>(UINT64_C(0xffffffff)));
  constexpr std::array<Word, 4> kIncrements = {1, 3, 5, 7};
  std::array<Value, 4> added;
  for (std::size_t lane = 0; lane < added.size(); ++lane) {
    added[lane] = builder.bitwise_and(
        builder.add(builder.block_parameter(loop, lane + 1),
                    builder.constant(kIncrements[lane])),
        mask);
  }

  std::array<Value, 4> next;
  for (std::size_t lane = 0; lane < next.size(); ++lane) {
    const Value mixed =
        builder.bitwise_xor(added[lane], added[(lane + 1) % added.size()]);
    next[lane] = builder.bitwise_and(
        builder.add(mixed, builder.constant(kIncrements[lane])), mask);
  }
  const Value next_remaining =
      builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  if (!builder
           .branch(continues, loop,
                   {next_remaining, next[0], next[1], next[2], next[3]}, exit,
                   {next[0], next[1], next[2], next[3]})
           .ok() ||
      !builder.set_insertion_block(exit).ok()) {
    return {};
  }

  std::array<Value, 4> lanes;
  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    lanes[lane] = builder.block_parameter(exit, lane);
  }
  if (!builder.set_return(combine_lanes(&builder, lanes)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

std::array<Word, 5> arguments(std::size_t loop_iterations) {
  return {static_cast<Word>(loop_iterations), INT64_C(0x12345678),
          INT64_C(0x23456789), INT64_C(0x3456789a), INT64_C(0x456789ab)};
}

template <typename Invoke>
Measurement measure(Invoke&& invoke, std::size_t loop_iterations,
                    std::size_t invocations) {
  const auto input = arguments(loop_iterations);
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < invocations; ++iteration) {
    const auto result = invoke(input.data(), input.size());
    if (!result.ok()) {
      return {-1.0, 0};
    }
    checksum = (checksum << 11U) | (checksum >> (64U - 11U));
    checksum ^= bits(result.value) + static_cast<std::uint64_t>(iteration);
  }
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  const double completed = static_cast<double>(loop_iterations) *
                           static_cast<double>(invocations);
  return {elapsed / completed, checksum};
}

double median(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

const char* architecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__riscv) && __riscv_xlen == 64
  return "riscv64";
#else
  return "unknown";
#endif
}

const char* operating_system() noexcept {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

const char* lowering_mode() noexcept {
#if defined(__riscv) && __riscv_xlen == 64
  return "scalarized";
#else
  return "native";
#endif
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return false;
    }
    std::size_t value = 0;
    try {
      value = static_cast<std::size_t>(std::stoull(argv[index + 1]));
    } catch (const std::exception&) {
      return false;
    }
    const std::string name = argv[index];
    if (name == "--loop-iterations") {
      options->loop_iterations = value;
    } else if (name == "--warmup") {
      options->warmup = value;
    } else if (name == "--invocations") {
      options->invocations = value;
    } else if (name == "--samples") {
      options->samples = value;
    } else {
      return false;
    }
  }
  return options->loop_iterations > 0 && options->warmup > 0 &&
         options->invocations > 0 && options->samples > 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_cfg_simd_benchmark "
                 "[--loop-iterations N] [--warmup N] "
                 "[--invocations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  const ControlFlowFunction vector_function = make_vector_function();
  const ControlFlowFunction scalar_function = make_scalar_function();
  const auto vector_compile_started = Clock::now();
  auto vector_compilation = unijit::jit::Compiler::compile(vector_function);
  const auto vector_compile_elapsed = Clock::now() - vector_compile_started;
  const auto scalar_compile_started = Clock::now();
  auto scalar_compilation = unijit::jit::Compiler::compile(scalar_function);
  const auto scalar_compile_elapsed = Clock::now() - scalar_compile_started;
  if (!vector_compilation.ok() || !scalar_compilation.ok()) {
    std::cerr << "compilation failed: vector="
              << vector_compilation.status.message()
              << ", scalar=" << scalar_compilation.status.message() << '\n';
    return EXIT_FAILURE;
  }

  const auto vector_native_invoke = [&](const Word* input, std::size_t count) {
    return vector_compilation.function->invoke(input, count);
  };
  const auto scalar_native_invoke = [&](const Word* input, std::size_t count) {
    return scalar_compilation.function->invoke(input, count);
  };
  const auto vector_interpreter_invoke = [&](const Word* input,
                                             std::size_t count) {
    return unijit::ir::ControlFlowInterpreter::evaluate(
        vector_function, input, count);
  };

  const Measurement vector_warmup = measure(
      vector_native_invoke, options.loop_iterations, options.warmup);
  const Measurement scalar_warmup = measure(
      scalar_native_invoke, options.loop_iterations, options.warmup);
  const Measurement interpreter_warmup = measure(
      vector_interpreter_invoke, options.loop_iterations, options.warmup);
  if (vector_warmup.nanoseconds_per_loop_iteration < 0 ||
      scalar_warmup.nanoseconds_per_loop_iteration < 0 ||
      interpreter_warmup.nanoseconds_per_loop_iteration < 0 ||
      vector_warmup.checksum != scalar_warmup.checksum ||
      vector_warmup.checksum != interpreter_warmup.checksum) {
    std::cerr << "warmup execution mismatch\n";
    return EXIT_FAILURE;
  }

  std::vector<double> vector_samples;
  std::vector<double> scalar_samples;
  std::vector<double> interpreter_samples;
  vector_samples.reserve(options.samples);
  scalar_samples.reserve(options.samples);
  interpreter_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement vector = measure(
        vector_native_invoke, options.loop_iterations, options.invocations);
    const Measurement scalar = measure(
        scalar_native_invoke, options.loop_iterations, options.invocations);
    const Measurement interpreted = measure(
        vector_interpreter_invoke, options.loop_iterations,
        options.invocations);
    if (vector.nanoseconds_per_loop_iteration < 0 ||
        scalar.nanoseconds_per_loop_iteration < 0 ||
        interpreted.nanoseconds_per_loop_iteration < 0 ||
        vector.checksum != scalar.checksum ||
        vector.checksum != interpreted.checksum) {
      std::cerr << "measurement execution mismatch\n";
      return EXIT_FAILURE;
    }
    checksum = vector.checksum;
    vector_samples.push_back(vector.nanoseconds_per_loop_iteration);
    scalar_samples.push_back(scalar.nanoseconds_per_loop_iteration);
    interpreter_samples.push_back(interpreted.nanoseconds_per_loop_iteration);
  }

  const double vector_median = median(std::move(vector_samples));
  const double scalar_median = median(std::move(scalar_samples));
  const double interpreter_median = median(std::move(interpreter_samples));
  const auto& vector_stats = vector_compilation.function->stats();
  const auto& scalar_stats = scalar_compilation.function->stats();
  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.cfg-simd-benchmark.v1\",\n"
            << "  \"benchmark\": \"strict_i32x4_recurrence\",\n"
            << "  \"measurement_boundary\": "
               "\"native_cfg_loop_iteration\",\n"
            << "  \"architecture\": \"" << architecture() << "\",\n"
            << "  \"operating_system\": \"" << operating_system()
            << "\",\n"
            << "  \"lowering_mode\": \"" << lowering_mode() << "\",\n"
            << "  \"vector_bits\": 128,\n"
            << "  \"lanes\": 4,\n"
            << "  \"loop_iterations\": " << options.loop_iterations << ",\n"
            << "  \"warmup_invocations\": " << options.warmup << ",\n"
            << "  \"measurement_invocations\": " << options.invocations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"vector_compile_microseconds\": "
            << std::chrono::duration<double, std::micro>(vector_compile_elapsed)
                   .count()
            << ",\n"
            << "  \"scalar_compile_microseconds\": "
            << std::chrono::duration<double, std::micro>(scalar_compile_elapsed)
                   .count()
            << ",\n"
            << "  \"vector_native_code_bytes\": " << vector_stats.code_size
            << ",\n"
            << "  \"scalar_native_code_bytes\": " << scalar_stats.code_size
            << ",\n"
            << "  \"vector_spill_slots\": " << vector_stats.spill_slots
            << ",\n"
            << "  \"scalar_spill_slots\": " << scalar_stats.spill_slots
            << ",\n"
            << "  \"vector_native_median_ns_per_loop_iteration\": "
            << vector_median << ",\n"
            << "  \"scalar_native_median_ns_per_loop_iteration\": "
            << scalar_median << ",\n"
            << "  \"vector_interpreter_median_ns_per_loop_iteration\": "
            << interpreter_median << ",\n"
            << "  \"vector_speedup_over_scalar\": "
            << scalar_median / vector_median << ",\n"
            << "  \"vector_speedup_over_interpreter\": "
            << interpreter_median / vector_median << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";
  return EXIT_SUCCESS;
}
