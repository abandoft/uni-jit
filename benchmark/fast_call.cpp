#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/target.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::ControlFlowBuilder;
using unijit::ir::ControlFlowFunction;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Value;
using unijit::ir::ValueType;
using unijit::ir::Word;
using unijit::jit::CodeCache;
using unijit::jit::CodeHandle;
using unijit::jit::CompiledFunction;
using unijit::jit::Compiler;

struct Options final {
  std::size_t loop_iterations{1000};
  std::size_t warmup{100};
  std::size_t invocations{500};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_dispatch{0};
  std::uint64_t checksum{0};
};

Word add_runtime_helper(const Word *arguments, std::size_t count) {
  return count == 2 ? arguments[0] + arguments[1] : 0;
}

Function make_target() {
  FunctionBuilder builder({ValueType::kWord, ValueType::kWord});
  if (!builder
           .set_return(builder.add(builder.parameter(0), builder.parameter(1)))
           .ok()) {
    return {};
  }
  return std::move(builder).build();
}

ControlFlowFunction make_caller(bool use_fast_call) {
  ControlFlowBuilder builder({ValueType::kWord, ValueType::kWord});
  const auto fast_target =
      use_fast_call
          ? builder.create_fast_call({ValueType::kWord, ValueType::kWord},
                                     ValueType::kWord)
          : unijit::ir::FastCallSlot{};
  const auto loop = builder.create_block({ValueType::kWord, ValueType::kWord});
  const auto exit = builder.create_block({ValueType::kWord});
  if (!builder.jump(loop, {builder.parameter(0), builder.parameter(1)}).ok() ||
      !builder.set_insertion_block(loop).ok()) {
    return {};
  }

  const Value remaining = builder.block_parameter(loop, 0);
  const Value accumulator = builder.block_parameter(loop, 1);
  const Value increment = builder.constant(1);
  const Value next_accumulator =
      use_fast_call
          ? builder.fast_call(fast_target, {accumulator, increment})
          : builder.call(add_runtime_helper, {accumulator, increment});
  const Value next_remaining = builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  if (!builder
           .branch(continues, loop, {next_remaining, next_accumulator}, exit,
                   {next_accumulator})
           .ok() ||
      !builder.set_insertion_block(exit).ok() ||
      !builder.set_return(builder.block_parameter(exit, 0)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

std::uint64_t mix(std::uint64_t checksum, Word value,
                  std::size_t iteration) noexcept {
  checksum = (checksum << 7U) | (checksum >> (64U - 7U));
  return checksum ^ (static_cast<std::uint64_t>(value) + iteration);
}

template <typename Invoke>
Measurement measure(Invoke &&invoke, const std::array<Word, 2> &arguments,
                    std::size_t invocations) {
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < invocations; ++iteration) {
    const auto result = invoke(arguments.data(), arguments.size());
    if (!result.ok()) {
      return {-1, 0};
    }
    checksum = mix(checksum, result.value, iteration);
  }
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  return {elapsed / static_cast<double>(invocations) /
              static_cast<double>(arguments[0]),
          checksum};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

const char *compiler_name() noexcept {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#elif defined(_MSC_VER)
  return "msvc";
#else
  return "unknown";
#endif
}

bool parse_options(int argc, char **argv, Options *options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return false;
    }
    std::size_t value = 0;
    try {
      value = static_cast<std::size_t>(std::stoull(argv[index + 1]));
    } catch (const std::exception &) {
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
         options->invocations > 0 && options->samples >= 3;
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_fast_call_benchmark "
                 "[--loop-iterations N] [--warmup N] "
                 "[--invocations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  auto target_compilation = Compiler::compile(make_target());
  auto fast_compilation = Compiler::compile(make_caller(true));
  auto runtime_compilation = Compiler::compile(make_caller(false));
  if (!target_compilation.ok() || !fast_compilation.ok() ||
      !runtime_compilation.ok()) {
    std::cerr << "unable to compile fast-call benchmark fixtures\n";
    return EXIT_FAILURE;
  }
  const std::size_t target_code_bytes =
      target_compilation.function->stats().code_size;
  const std::size_t fast_code_bytes =
      fast_compilation.function->stats().code_size;
  const std::size_t runtime_code_bytes =
      runtime_compilation.function->stats().code_size;

  CodeCache cache;
  auto target_publication = cache.publish(
      "fast-call-benchmark-target", 1, std::move(target_compilation.function));
  auto fast_publication = cache.publish("fast-call-benchmark-caller", 1,
                                        std::move(fast_compilation.function));
  if (!target_publication.ok() || !fast_publication.ok() ||
      !fast_publication.handle.bind_fast_call(0, target_publication.handle)
           .ok()) {
    std::cerr << "unable to publish fast-call benchmark generations\n";
    return EXIT_FAILURE;
  }

  const CodeHandle fast = fast_publication.handle;
  const CompiledFunction &runtime = *runtime_compilation.function;
  const std::array<Word, 2> arguments = {
      static_cast<Word>(options.loop_iterations), 7};
  const auto fast_invoke = [&](const Word *values, std::size_t count) {
    return fast.invoke(values, count);
  };
  const auto runtime_invoke = [&](const Word *values, std::size_t count) {
    return runtime.invoke(values, count);
  };

  const Measurement fast_warmup =
      measure(fast_invoke, arguments, options.warmup);
  const Measurement runtime_warmup =
      measure(runtime_invoke, arguments, options.warmup);
  if (fast_warmup.nanoseconds_per_dispatch <= 0 ||
      runtime_warmup.nanoseconds_per_dispatch <= 0 ||
      fast_warmup.checksum != runtime_warmup.checksum) {
    std::cerr << "fast-call benchmark warmup failed\n";
    return EXIT_FAILURE;
  }

  std::vector<double> fast_samples;
  std::vector<double> runtime_samples;
  fast_samples.reserve(options.samples);
  runtime_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    Measurement fast_measurement;
    Measurement runtime_measurement;
    if ((sample & 1U) == 0U) {
      fast_measurement = measure(fast_invoke, arguments, options.invocations);
      runtime_measurement =
          measure(runtime_invoke, arguments, options.invocations);
    } else {
      runtime_measurement =
          measure(runtime_invoke, arguments, options.invocations);
      fast_measurement = measure(fast_invoke, arguments, options.invocations);
    }
    if (fast_measurement.nanoseconds_per_dispatch <= 0 ||
        runtime_measurement.nanoseconds_per_dispatch <= 0 ||
        fast_measurement.checksum != runtime_measurement.checksum) {
      std::cerr << "fast-call benchmark measurement failed\n";
      return EXIT_FAILURE;
    }
    fast_samples.push_back(fast_measurement.nanoseconds_per_dispatch);
    runtime_samples.push_back(runtime_measurement.nanoseconds_per_dispatch);
    checksum = fast_measurement.checksum;
  }

  const double fast_median = median(std::move(fast_samples));
  const double runtime_median = median(std::move(runtime_samples));
  const auto host = unijit::jit::host_target_profile();
  std::cout << std::fixed << std::setprecision(3) << "{\n"
            << "  \"schema\": \"unijit.fast-call-benchmark.v1\",\n"
            << "  \"benchmark\": \"generation_safe_cfg_dispatch\",\n"
            << "  \"measurement_boundary\": "
               "\"complete_managed_cfg_invocation\",\n"
            << "  \"architecture\": \""
            << unijit::jit::target_architecture_name(host.architecture)
            << "\",\n"
            << "  \"abi\": \"" << unijit::jit::target_abi_name(host.abi)
            << "\",\n"
            << "  \"compiler\": \"" << compiler_name() << "\",\n"
            << "  \"loop_iterations\": " << options.loop_iterations << ",\n"
            << "  \"warmup_invocations\": " << options.warmup << ",\n"
            << "  \"measurement_invocations\": " << options.invocations << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"target_native_code_bytes\": " << target_code_bytes << ",\n"
            << "  \"fast_caller_native_code_bytes\": " << fast_code_bytes
            << ",\n"
            << "  \"runtime_caller_native_code_bytes\": " << runtime_code_bytes
            << ",\n"
            << "  \"fast_call_median_ns_per_dispatch\": " << fast_median
            << ",\n"
            << "  \"runtime_helper_median_ns_per_dispatch\": " << runtime_median
            << ",\n"
            << "  \"fast_call_overhead_ratio\": "
            << fast_median / runtime_median << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";
  return EXIT_SUCCESS;
}
