#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <pocketpy.h>

#include "unijit_pocketpy.h"

#ifndef UNIJIT_POCKETPY_BENCHMARK_SOURCE
#define UNIJIT_POCKETPY_BENCHMARK_SOURCE "benchmark/pocketpy/numeric_call.py"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{100000};
  std::size_t samples{7};
  std::string script{UNIJIT_POCKETPY_BENCHMARK_SOURCE};
};

struct Measurement final {
  double nanoseconds_per_iteration{-1.0};
  std::uint64_t checksum{0};
};

bool parse_positive_size(const char *text, std::size_t *value) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed);
    if (consumed != std::strlen(text) || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<py_i64>::max())) {
      return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_options(int argc, char **argv, Options *options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return false;
    }
    const std::string name = argv[index];
    if (name == "--script") {
      options->script = argv[index + 1];
    } else if (name == "--warmup") {
      if (!parse_positive_size(argv[index + 1], &options->warmup)) {
        return false;
      }
    } else if (name == "--iterations") {
      if (!parse_positive_size(argv[index + 1], &options->iterations)) {
        return false;
      }
    } else if (name == "--samples") {
      if (!parse_positive_size(argv[index + 1], &options->samples)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !options->script.empty();
}

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::uint64_t bits(double value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Measurement measure(py_Ref execute, py_Ref function, std::size_t iterations) {
  std::array<py_TValue, 2> arguments{};
  py_assign(&arguments[0], function);
  py_newint(&arguments[1], static_cast<py_i64>(iterations));
  const auto started = Clock::now();
  const bool succeeded =
      py_call(execute, static_cast<int>(arguments.size()), arguments.data());
  const auto elapsed = Clock::now() - started;
  if (!succeeded || !py_isfloat(py_retval())) {
    return {};
  }
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(elapsed).count();
  return {nanoseconds / static_cast<double>(iterations),
          bits(py_tofloat(py_retval()))};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  return (values.size() & 1U) != 0
             ? values[middle]
             : (values[middle - 1] + values[middle]) / 2.0;
}

int fail(const char *message) {
  std::cerr << message << '\n';
  if (py_checkexc()) {
    py_printexc();
  }
  py_finalize();
  return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_pocketpy_benchmark [--script PATH] "
                 "[--warmup N] [--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }
  const std::string source = read_file(options.script);
  if (source.empty()) {
    std::cerr << "unable to read the Python benchmark source: "
              << options.script << '\n';
    return EXIT_FAILURE;
  }

  py_initialize();
  if (unijit_pocketpy_install() != 0) {
    return fail("unable to install the UniJIT PocketPy module");
  }
  if (!py_exec(source.c_str(), options.script.c_str(), EXEC_MODE, nullptr) ||
      !py_exec("import unijit\nnative = unijit.compile(unijit_native_source)",
               "<unijit-pocketpy-compile>", EXEC_MODE, nullptr)) {
    return fail("unable to compile the PocketPy benchmark functions");
  }
  const py_Ref stock = py_getglobal(py_name("numeric_kernel"));
  const py_Ref native = py_getglobal(py_name("native"));
  const py_Ref execute = py_getglobal(py_name("execute_numeric_kernel"));
  if (stock == nullptr || native == nullptr || execute == nullptr) {
    return fail("unable to resolve the PocketPy benchmark functions");
  }

  if (measure(execute, stock, options.warmup).nanoseconds_per_iteration < 0.0 ||
      measure(execute, native, options.warmup).nanoseconds_per_iteration <
          0.0) {
    return fail("PocketPy benchmark warmup failed");
  }

  std::vector<double> stock_samples;
  std::vector<double> native_samples;
  stock_samples.reserve(options.samples);
  native_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  bool has_checksum = false;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement stock_measurement =
        measure(execute, stock, options.iterations);
    const Measurement native_measurement =
        measure(execute, native, options.iterations);
    if (stock_measurement.nanoseconds_per_iteration < 0.0 ||
        native_measurement.nanoseconds_per_iteration < 0.0 ||
        stock_measurement.checksum != native_measurement.checksum ||
        (has_checksum && checksum != native_measurement.checksum)) {
      return fail("PocketPy benchmark engines or samples produced different "
                  "results");
    }
    checksum = native_measurement.checksum;
    has_checksum = true;
    stock_samples.push_back(stock_measurement.nanoseconds_per_iteration);
    native_samples.push_back(native_measurement.nanoseconds_per_iteration);
  }

  const double stock_median = median(std::move(stock_samples));
  const double native_median = median(std::move(native_samples));
  std::cout << std::fixed << std::setprecision(3) << "{\n"
            << "  \"schema\": \"unijit.pocketpy-numeric-call.v2\",\n"
            << "  \"pocketpy_version\": \"2.1.8\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"stock_median_ns\": " << stock_median << ",\n"
            << "  \"unijit_median_ns\": " << native_median << ",\n"
            << "  \"speedup\": " << stock_median / native_median << ",\n"
            << "  \"checksum\": \"0x" << std::hex << std::setw(16)
            << std::setfill('0') << checksum << "\"\n"
            << "}\n";

  py_finalize();
  return EXIT_SUCCESS;
}
