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
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/jit/compiler.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::ControlFlowBuilder;
using unijit::ir::ControlFlowFunction;
using unijit::ir::Value;
using unijit::ir::ValueType;
using unijit::ir::Word;

struct Options final {
  std::size_t loop_iterations{1000};
  std::size_t warmup{100};
  std::size_t invocations{1000};
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

ControlFlowFunction make_function() {
  ControlFlowBuilder builder({ValueType::kWord, ValueType::kFloat64,
                              ValueType::kFloat64, ValueType::kFloat64,
                              ValueType::kFloat64});
  const auto loop = builder.create_block(
      {ValueType::kWord, ValueType::kFloat64, ValueType::kFloat64,
       ValueType::kFloat64, ValueType::kFloat64});
  const auto exit = builder.create_block(
      {ValueType::kFloat64, ValueType::kFloat64, ValueType::kFloat64,
       ValueType::kFloat64});
  if (!builder
           .jump(loop, {builder.parameter(0), builder.parameter(1),
                        builder.parameter(2), builder.parameter(3),
                        builder.parameter(4)})
           .ok() ||
      !builder.set_insertion_block(loop).ok()) {
    return {};
  }

  const Value remaining = builder.block_parameter(loop, 0);
  const Value a = builder.block_parameter(loop, 1);
  const Value b = builder.block_parameter(loop, 2);
  const Value c = builder.block_parameter(loop, 3);
  const Value d = builder.block_parameter(loop, 4);
  const Value next_a =
      builder.float64_add(b, builder.float64_constant(0.125));
  const Value next_b =
      builder.float64_subtract(c, builder.float64_constant(0.25));
  const Value next_c =
      builder.float64_multiply(d, builder.float64_constant(0.999));
  const Value next_d =
      builder.float64_divide(a, builder.float64_constant(1.001));
  const Value next_remaining =
      builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  const std::vector<Value> next_state = {next_a, next_b, next_c, next_d};
  if (!builder
           .branch(continues, loop,
                   {next_remaining, next_a, next_b, next_c, next_d}, exit,
                   next_state)
           .ok() ||
      !builder.set_insertion_block(exit).ok()) {
    return {};
  }

  Value result = builder.block_parameter(exit, 0);
  for (std::size_t index = 1; index < 4; ++index) {
    result =
        builder.float64_add(result, builder.block_parameter(exit, index));
  }
  if (!builder.set_return(result).ok()) {
    return {};
  }
  return std::move(builder).build();
}

std::array<Word, 5> arguments(std::size_t loop_iterations) {
  return {static_cast<Word>(loop_iterations),
          unijit::ir::pack_float64(1.25),
          unijit::ir::pack_float64(-2.5),
          unijit::ir::pack_float64(3.75),
          unijit::ir::pack_float64(-4.5)};
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
    const std::uint64_t value = bits(result.value);
    checksum = (checksum << 7U) | (checksum >> (64U - 7U));
    checksum ^= value + static_cast<std::uint64_t>(iteration);
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
    std::cerr << "usage: unijit_cfg_float64_benchmark "
                 "[--loop-iterations N] [--warmup N] "
                 "[--invocations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  const ControlFlowFunction function = make_function();
  const auto compilation_started = Clock::now();
  auto compilation = unijit::jit::Compiler::compile(function);
  const auto compilation_elapsed = Clock::now() - compilation_started;
  if (!compilation.ok()) {
    std::cerr << "compilation failed: " << compilation.status.message() << '\n';
    return EXIT_FAILURE;
  }

  const auto native_invoke = [&](const Word* input, std::size_t count) {
    return compilation.function->invoke(input, count);
  };
  const auto interpreter_invoke = [&](const Word* input, std::size_t count) {
    return unijit::ir::ControlFlowInterpreter::evaluate(function, input,
                                                        count);
  };
  const Measurement native_warmup =
      measure(native_invoke, options.loop_iterations, options.warmup);
  const Measurement interpreter_warmup =
      measure(interpreter_invoke, options.loop_iterations, options.warmup);
  if (native_warmup.nanoseconds_per_loop_iteration < 0 ||
      interpreter_warmup.nanoseconds_per_loop_iteration < 0 ||
      native_warmup.checksum != interpreter_warmup.checksum) {
    std::cerr << "warmup execution mismatch\n";
    return EXIT_FAILURE;
  }

  std::vector<double> native_samples;
  std::vector<double> interpreter_samples;
  native_samples.reserve(options.samples);
  interpreter_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement native =
        measure(native_invoke, options.loop_iterations, options.invocations);
    const Measurement interpreted = measure(
        interpreter_invoke, options.loop_iterations, options.invocations);
    if (native.nanoseconds_per_loop_iteration < 0 ||
        interpreted.nanoseconds_per_loop_iteration < 0 ||
        native.checksum != interpreted.checksum) {
      std::cerr << "measurement execution mismatch\n";
      return EXIT_FAILURE;
    }
    checksum = native.checksum;
    native_samples.push_back(native.nanoseconds_per_loop_iteration);
    interpreter_samples.push_back(interpreted.nanoseconds_per_loop_iteration);
  }

  const double native_median = median(native_samples);
  const double interpreter_median = median(interpreter_samples);
  const auto& stats = compilation.function->stats();
  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.cfg-float64-benchmark.v1\",\n"
            << "  \"benchmark\": \"float64_register_residency\",\n"
            << "  \"measurement_boundary\": "
               "\"native_cfg_loop_iteration\",\n"
            << "  \"architecture\": \"" << architecture() << "\",\n"
            << "  \"loop_iterations\": " << options.loop_iterations << ",\n"
            << "  \"warmup_invocations\": " << options.warmup << ",\n"
            << "  \"measurement_invocations\": " << options.invocations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"compile_microseconds\": "
            << std::chrono::duration<double, std::micro>(compilation_elapsed)
                   .count()
            << ",\n"
            << "  \"input_ir_nodes\": " << stats.input_ir_nodes << ",\n"
            << "  \"native_code_bytes\": " << stats.code_size << ",\n"
            << "  \"spill_slots\": " << stats.spill_slots << ",\n"
            << "  \"native_median_ns_per_loop_iteration\": " << native_median
            << ",\n"
            << "  \"interpreter_median_ns_per_loop_iteration\": "
            << interpreter_median << ",\n"
            << "  \"speedup\": " << interpreter_median / native_median
            << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";
  return EXIT_SUCCESS;
}
