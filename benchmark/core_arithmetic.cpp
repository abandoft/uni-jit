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

#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/jit/compiler.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::Word;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{100000};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_iteration{0};
  std::uint64_t checksum{0};
};

Word from_bits(std::uint64_t bits) noexcept {
  Word result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

std::uint64_t to_bits(Word value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

void advance_arguments(std::array<Word, 2>* arguments) noexcept {
  const std::uint64_t first =
      to_bits((*arguments)[0]) * 6364136223846793005ULL + 1442695040888963407ULL;
  const std::uint64_t second = to_bits((*arguments)[1]) + 0x9E3779B97F4A7C15ULL;
  (*arguments)[0] = from_bits(first);
  (*arguments)[1] = from_bits(second ^ (first >> 17U));
}

Function make_function() {
  FunctionBuilder builder(2);
  const auto x = builder.parameter(0);
  const auto y = builder.parameter(1);
  auto value = builder.add(x, y);
  value = builder.multiply(value, builder.constant(0x1F123BB5));
  value = builder.add(value, x);
  value = builder.multiply(value, builder.constant(-0x102030405LL));
  value = builder.subtract(value, y);
  value = builder.multiply(value, builder.add(x, builder.constant(17)));
  value = builder.add(value, builder.constant(0x123456789ABCDEFLL));
  if (!builder.set_return(value).ok()) {
    return {};
  }
  return std::move(builder).build();
}

Measurement measure_native(unijit::jit::NativeEntry entry,
                           std::size_t iterations) {
  std::array<Word, 2> arguments = {from_bits(0x0123456789ABCDEFULL),
                                   from_bits(0xFEDCBA9876543210ULL)};
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    checksum ^= to_bits(entry(arguments.data(), nullptr));
    advance_arguments(&arguments);
  }
  const auto elapsed = Clock::now() - started;
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(elapsed).count();
  return {nanoseconds / static_cast<double>(iterations), checksum};
}

Measurement measure_interpreter(const Function& function,
                                std::size_t iterations) {
  std::array<Word, 2> arguments = {from_bits(0x0123456789ABCDEFULL),
                                   from_bits(0xFEDCBA9876543210ULL)};
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const auto result =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    if (!result.ok()) {
      return {-1, 0};
    }
    checksum ^= to_bits(result.value);
    advance_arguments(&arguments);
  }
  const auto elapsed = Clock::now() - started;
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(elapsed).count();
  return {nanoseconds / static_cast<double>(iterations), checksum};
}

double median(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const std::size_t middle = samples.size() / 2;
  if ((samples.size() & 1U) != 0) {
    return samples[middle];
  }
  return (samples[middle - 1] + samples[middle]) / 2.0;
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

const char* compiler() noexcept {
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
    if (name == "--warmup") {
      options->warmup = value;
    } else if (name == "--iterations") {
      options->iterations = value;
    } else if (name == "--samples") {
      options->samples = value;
    } else {
      return false;
    }
  }
  return options->warmup > 0 && options->iterations > 0 &&
         options->samples > 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_core_benchmark [--warmup N] "
                 "[--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  const Function function = make_function();
  const auto compilation_started = Clock::now();
  auto compilation = unijit::jit::Compiler::compile(function);
  const auto compilation_elapsed = Clock::now() - compilation_started;
  if (!compilation.ok()) {
    std::cerr << "compilation failed: " << compilation.status.message() << '\n';
    return EXIT_FAILURE;
  }
  const auto entry = compilation.function->native_entry();
  if (entry == nullptr) {
    std::cerr << "compilation produced no native entry\n";
    return EXIT_FAILURE;
  }

  const Measurement native_warmup = measure_native(entry, options.warmup);
  const Measurement interpreter_warmup =
      measure_interpreter(function, options.warmup);
  if (native_warmup.checksum != interpreter_warmup.checksum) {
    std::cerr << "warmup checksum mismatch\n";
    return EXIT_FAILURE;
  }

  std::vector<double> native_samples;
  std::vector<double> interpreter_samples;
  native_samples.reserve(options.samples);
  interpreter_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement native = measure_native(entry, options.iterations);
    const Measurement interpreted =
        measure_interpreter(function, options.iterations);
    if (native.checksum != interpreted.checksum) {
      std::cerr << "measurement checksum mismatch\n";
      return EXIT_FAILURE;
    }
    checksum = native.checksum;
    native_samples.push_back(native.nanoseconds_per_iteration);
    interpreter_samples.push_back(interpreted.nanoseconds_per_iteration);
  }

  const double native_median = median(native_samples);
  const double interpreter_median = median(interpreter_samples);
  const double compilation_microseconds =
      std::chrono::duration<double, std::micro>(compilation_elapsed).count();
  const auto& stats = compilation.function->stats();

  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.core-benchmark.v1\",\n"
            << "  \"benchmark\": \"integer_arithmetic\",\n"
            << "  \"os\": \"" << operating_system() << "\",\n"
            << "  \"architecture\": \"" << architecture() << "\",\n"
            << "  \"compiler\": \"" << compiler() << "\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"compile_microseconds\": " << compilation_microseconds
            << ",\n"
            << "  \"input_ir_nodes\": " << stats.input_ir_nodes << ",\n"
            << "  \"optimized_ir_nodes\": " << stats.optimized_ir_nodes
            << ",\n"
            << "  \"native_code_bytes\": " << stats.code_size << ",\n"
            << "  \"spill_slots\": " << stats.spill_slots << ",\n"
            << "  \"native_median_ns\": " << native_median << ",\n"
            << "  \"interpreter_median_ns\": " << interpreter_median
            << ",\n"
            << "  \"speedup\": " << interpreter_median / native_median
            << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";
  return EXIT_SUCCESS;
}
