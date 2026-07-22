#include <algorithm>
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

#include "unijit/ir/function.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/target.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::PatchCellKind;
using unijit::ir::Word;
using unijit::jit::CompiledFunction;
using unijit::jit::Compiler;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{200000};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_operation{0};
  std::uint64_t checksum{0};
};

Function make_constant_reader() {
  FunctionBuilder builder(0);
  if (!builder.set_return(builder.constant(41)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

Function make_patch_reader() {
  FunctionBuilder builder(0);
  const auto cell =
      builder.create_patch_cell(41, PatchCellKind::kGeneration);
  if (!builder.set_return(builder.load_patch_cell(cell)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

std::uint64_t mix(std::uint64_t checksum, Word value) noexcept {
  return (checksum * UINT64_C(0x9e3779b185ebca87)) ^
         static_cast<std::uint64_t>(value);
}

Measurement measure_invocation(const CompiledFunction& function,
                               std::size_t iterations) {
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const auto result = function.invoke(nullptr, 0);
    if (!result.ok()) {
      return {-1, 0};
    }
    checksum = mix(checksum, result.value);
  }
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  return {nanoseconds / static_cast<double>(iterations), checksum};
}

Measurement measure_mutation(const CompiledFunction& function,
                             std::size_t iterations) {
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const Word value = static_cast<Word>(41 + (iteration & 1U));
    if (!function.publish_patch_cell(0, value).ok()) {
      return {-1, 0};
    }
    const auto observed = function.read_patch_cell(0);
    if (!observed.ok() || observed.value != value) {
      return {-1, 0};
    }
    checksum = mix(checksum, observed.value);
  }
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  return {nanoseconds / static_cast<double>(iterations), checksum};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  return (values.size() & 1U) != 0
             ? values[middle]
             : (values[middle - 1] + values[middle]) / 2.0;
}

const char* compiler_name() noexcept {
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

bool parse_options(int argc, char** argv, Options* options) {
  if (options == nullptr) {
    return false;
  }
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
    const std::string option = argv[index];
    if (option == "--warmup") {
      options->warmup = value;
    } else if (option == "--iterations") {
      options->iterations = value;
    } else if (option == "--samples") {
      options->samples = value;
    } else {
      return false;
    }
  }
  return options->warmup != 0 && options->iterations != 0 &&
         options->samples >= 3;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_patch_cell_benchmark [--warmup N] "
                 "[--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  auto constant = Compiler::compile(make_constant_reader());
  auto patch = Compiler::compile(make_patch_reader());
  if (!constant.ok() || !patch.ok()) {
    std::cerr << "unable to compile patch-cell benchmark fixtures\n";
    return EXIT_FAILURE;
  }

  const Measurement constant_warmup =
      measure_invocation(*constant.function, options.warmup);
  const Measurement patch_warmup =
      measure_invocation(*patch.function, options.warmup);
  const Measurement mutation_warmup =
      measure_mutation(*patch.function, options.warmup);
  if (constant_warmup.nanoseconds_per_operation <= 0 ||
      patch_warmup.nanoseconds_per_operation <= 0 ||
      mutation_warmup.nanoseconds_per_operation <= 0 ||
      constant_warmup.checksum != patch_warmup.checksum ||
      !patch.function->publish_patch_cell(0, 41).ok()) {
    std::cerr << "patch-cell benchmark warmup failed\n";
    return EXIT_FAILURE;
  }

  std::vector<double> constant_samples;
  std::vector<double> patch_samples;
  std::vector<double> mutation_samples;
  constant_samples.reserve(options.samples);
  patch_samples.reserve(options.samples);
  mutation_samples.reserve(options.samples);
  std::uint64_t invocation_checksum = 0;
  std::uint64_t mutation_checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    Measurement constant_measurement;
    Measurement patch_measurement;
    if ((sample & 1U) == 0) {
      constant_measurement =
          measure_invocation(*constant.function, options.iterations);
      patch_measurement =
          measure_invocation(*patch.function, options.iterations);
    } else {
      patch_measurement =
          measure_invocation(*patch.function, options.iterations);
      constant_measurement =
          measure_invocation(*constant.function, options.iterations);
    }
    const Measurement mutation_measurement =
        measure_mutation(*patch.function, options.iterations);
    if (constant_measurement.nanoseconds_per_operation <= 0 ||
        patch_measurement.nanoseconds_per_operation <= 0 ||
        mutation_measurement.nanoseconds_per_operation <= 0 ||
        constant_measurement.checksum != patch_measurement.checksum) {
      std::cerr << "patch-cell benchmark measurement failed\n";
      return EXIT_FAILURE;
    }
    constant_samples.push_back(constant_measurement.nanoseconds_per_operation);
    patch_samples.push_back(patch_measurement.nanoseconds_per_operation);
    mutation_samples.push_back(mutation_measurement.nanoseconds_per_operation);
    invocation_checksum = patch_measurement.checksum;
    mutation_checksum = mutation_measurement.checksum;
    if (!patch.function->publish_patch_cell(0, 41).ok()) {
      std::cerr << "unable to reset patch-cell benchmark fixture\n";
      return EXIT_FAILURE;
    }
  }

  const double constant_median = median(std::move(constant_samples));
  const double patch_median = median(std::move(patch_samples));
  const double mutation_median = median(std::move(mutation_samples));
  const auto host = unijit::jit::host_target_profile();
  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.patch-cell-benchmark.v1\",\n"
            << "  \"benchmark\": \"managed_acquire_load\",\n"
            << "  \"measurement_boundary\": "
               "\"managed_compiled_invocation\",\n"
            << "  \"architecture\": \""
            << unijit::jit::target_architecture_name(host.architecture)
            << "\",\n"
            << "  \"abi\": \"" << unijit::jit::target_abi_name(host.abi)
            << "\",\n"
            << "  \"compiler\": \"" << compiler_name() << "\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"constant_native_code_bytes\": "
            << constant.function->stats().code_size << ",\n"
            << "  \"patch_native_code_bytes\": "
            << patch.function->stats().code_size << ",\n"
            << "  \"constant_managed_median_ns\": " << constant_median
            << ",\n"
            << "  \"patch_managed_median_ns\": " << patch_median << ",\n"
            << "  \"managed_overhead_ratio\": "
            << patch_median / constant_median << ",\n"
            << "  \"mutation_round_trip_median_ns\": " << mutation_median
            << ",\n"
            << "  \"invocation_checksum\": \"0x" << std::hex
            << invocation_checksum << "\",\n"
            << "  \"mutation_checksum\": \"0x" << mutation_checksum
            << "\"\n"
            << "}\n";
  return EXIT_SUCCESS;
}
