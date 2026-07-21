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
#include "unijit/runtime/execution_context.h"

namespace {

using Clock = std::chrono::steady_clock;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::MemoryAccessDescriptor;
using unijit::ir::MemoryByteOrder;
using unijit::ir::MemoryWidth;
using unijit::ir::Word;
using unijit::runtime::ExecutionContext;
using unijit::runtime::MemoryRegion;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{100000};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_iteration{0};
  std::uint64_t checksum{0};
};

struct CaseResult final {
  const char* name{nullptr};
  std::size_t native_code_bytes{0};
  double compile_microseconds{0};
  double native_median_ns{0};
  double interpreter_median_ns{0};
  std::uint64_t checksum{0};
};

std::uint64_t bits(Word value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Word word(std::uint64_t value) noexcept {
  Word result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

void advance(std::array<Word, 2>* arguments) noexcept {
  const std::uint64_t next =
      bits((*arguments)[1]) * UINT64_C(6364136223846793005) +
      UINT64_C(1442695040888963407);
  (*arguments)[1] = word(next);
}

Function make_function(MemoryByteOrder order, std::uint8_t alignment) {
  FunctionBuilder builder(2, 1);
  MemoryAccessDescriptor access;
  access.width = MemoryWidth::k64;
  access.alignment = alignment;
  access.byte_order = order;
  builder.store_word(builder.parameter(0), builder.parameter(1), access, 10);
  const auto loaded = builder.load_word(builder.parameter(0), access, 11);
  if (!builder.set_return(loaded).ok()) {
    return {};
  }
  return std::move(builder).build();
}

template <typename Invoke>
Measurement measure(Invoke&& invoke, Word offset, std::size_t iterations) {
  std::array<Word, 2> arguments = {
      offset, word(UINT64_C(0x0123456789ABCDEF))};
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const auto result = invoke(arguments.data(), arguments.size());
    if (!result.ok()) {
      return {-1.0, 0};
    }
    checksum = (checksum << 9U) | (checksum >> (64U - 9U));
    checksum ^= bits(result.value) + static_cast<std::uint64_t>(iteration);
    advance(&arguments);
  }
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - started).count();
  return {elapsed / static_cast<double>(iterations), checksum};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

bool run_case(const char* name, MemoryByteOrder order, std::uint8_t alignment,
              Word offset, const Options& options, CaseResult* result) {
  const Function function = make_function(order, alignment);
  const auto compilation_started = Clock::now();
  auto compilation = unijit::jit::Compiler::compile(function);
  const auto compilation_elapsed = Clock::now() - compilation_started;
  if (!compilation.ok()) {
    std::cerr << name << " compilation failed: "
              << compilation.status.message() << '\n';
    return false;
  }

  alignas(8) std::array<std::uint8_t, 32> native_bytes{};
  alignas(8) std::array<std::uint8_t, 32> interpreter_bytes{};
  MemoryRegion native_region{native_bytes.data(), native_bytes.size(), true};
  MemoryRegion interpreter_region{interpreter_bytes.data(),
                                  interpreter_bytes.size(), true};
  ExecutionContext native_context;
  ExecutionContext interpreter_context;
  if (!native_context.bind_memory_regions(&native_region, 1).ok() ||
      !interpreter_context.bind_memory_regions(&interpreter_region, 1).ok()) {
    std::cerr << name << " could not bind benchmark memory\n";
    return false;
  }

  const auto native_invoke = [&](const Word* arguments, std::size_t) {
    const Word value =
        compilation.function->native_entry()(arguments, &native_context);
    return unijit::ir::EvaluationResult{unijit::Status::ok_status(), value};
  };
  const auto interpreter_invoke = [&](const Word* arguments,
                                      std::size_t count) {
    return Interpreter::evaluate(function, arguments, count,
                                 &interpreter_context);
  };

  const Measurement native_warmup =
      measure(native_invoke, offset, options.warmup);
  const Measurement interpreter_warmup =
      measure(interpreter_invoke, offset, options.warmup);
  if (native_warmup.nanoseconds_per_iteration < 0 ||
      interpreter_warmup.nanoseconds_per_iteration < 0 ||
      native_warmup.checksum != interpreter_warmup.checksum ||
      native_bytes != interpreter_bytes) {
    std::cerr << name << " warmup execution mismatch\n";
    return false;
  }

  std::vector<double> native_samples;
  std::vector<double> interpreter_samples;
  native_samples.reserve(options.samples);
  interpreter_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement native = measure(native_invoke, offset, options.iterations);
    const Measurement interpreted =
        measure(interpreter_invoke, offset, options.iterations);
    if (native.nanoseconds_per_iteration < 0 ||
        interpreted.nanoseconds_per_iteration < 0 ||
        native.checksum != interpreted.checksum ||
        native_bytes != interpreter_bytes) {
      std::cerr << name << " measurement execution mismatch\n";
      return false;
    }
    checksum = native.checksum;
    native_samples.push_back(native.nanoseconds_per_iteration);
    interpreter_samples.push_back(interpreted.nanoseconds_per_iteration);
  }

  result->name = name;
  result->native_code_bytes = compilation.function->stats().code_size;
  result->compile_microseconds =
      std::chrono::duration<double, std::micro>(compilation_elapsed).count();
  result->native_median_ns = median(std::move(native_samples));
  result->interpreter_median_ns = median(std::move(interpreter_samples));
  result->checksum = checksum;
  return true;
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
    std::cerr << "usage: unijit_bounded_memory_benchmark [--warmup N] "
                 "[--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  CaseResult aligned;
  CaseResult unaligned;
  if (!run_case("aligned_native_u64", MemoryByteOrder::kNative, 8, 0,
                options, &aligned) ||
      !run_case("unaligned_big_u64", MemoryByteOrder::kBigEndian, 1, 1,
                options, &unaligned)) {
    return EXIT_FAILURE;
  }

  const auto print_case = [](const CaseResult& result, bool trailing_comma) {
    std::cout << "    {\n"
              << "      \"name\": \"" << result.name << "\",\n"
              << "      \"compile_microseconds\": "
              << result.compile_microseconds << ",\n"
              << "      \"native_code_bytes\": "
              << result.native_code_bytes << ",\n"
              << "      \"native_median_ns\": " << result.native_median_ns
              << ",\n"
              << "      \"interpreter_median_ns\": "
              << result.interpreter_median_ns << ",\n"
              << "      \"speedup\": "
              << result.interpreter_median_ns / result.native_median_ns
              << ",\n"
              << "      \"checksum\": \"0x" << std::hex << result.checksum
              << std::dec << "\"\n"
              << "    }" << (trailing_comma ? "," : "") << '\n';
  };

  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.bounded-memory-benchmark.v1\",\n"
            << "  \"benchmark\": \"bounded_word_store_load\",\n"
            << "  \"os\": \"" << operating_system() << "\",\n"
            << "  \"architecture\": \"" << architecture() << "\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"cases\": [\n";
  print_case(aligned, true);
  print_case(unaligned, false);
  std::cout << "  ]\n}\n";
  return EXIT_SUCCESS;
}
