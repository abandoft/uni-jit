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

#include <pocketpy.h>

#include "unijit_pocketpy.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{100000};
  std::size_t samples{7};
};

struct Measurement final {
  double nanoseconds_per_call{-1.0};
  std::uint64_t checksum{0};
};

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
  return options->warmup > 0 && options->iterations > 0 && options->samples > 0;
}

std::uint64_t bits(double value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Measurement measure(py_Ref function, std::size_t iterations) {
  double lhs = 1.25;
  double rhs = -7.5;
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    std::array<py_TValue, 2> arguments{};
    py_newfloat(&arguments[0], lhs);
    py_newfloat(&arguments[1], rhs);
    if (!py_call(function, static_cast<int>(arguments.size()),
                 arguments.data()) ||
        !py_isfloat(py_retval())) {
      return {};
    }
    checksum ^= bits(py_tofloat(py_retval()));
    lhs += 0.125;
    rhs -= 0.0625;
    if (lhs > 4096.0) {
      lhs = 1.25;
    }
    if (rhs < -4096.0) {
      rhs = -7.5;
    }
  }
  const auto elapsed = Clock::now() - started;
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(elapsed).count();
  return {nanoseconds / static_cast<double>(iterations), checksum};
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
    std::cerr << "usage: unijit_pocketpy_benchmark [--warmup N] "
                 "[--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }

  py_initialize();
  if (unijit_pocketpy_install() != 0) {
    return fail("unable to install the UniJIT PocketPy module");
  }
  constexpr char kSetup[] =
      "import unijit\n"
      "def stock(a, b):\n"
      "    return (a + b) * (a - 3.25) + b * 0.75\n"
      "native = unijit.compile(\"def native(a, b): return "
      "(a + b) * (a - 3.25) + b * 0.75\")\n";
  if (!py_exec(kSetup, "<unijit-pocketpy-benchmark>", EXEC_MODE, nullptr)) {
    return fail("unable to compile the PocketPy benchmark functions");
  }
  const py_Ref stock = py_getglobal(py_name("stock"));
  const py_Ref native = py_getglobal(py_name("native"));
  if (stock == nullptr || native == nullptr) {
    return fail("unable to resolve the PocketPy benchmark functions");
  }

  if (measure(stock, options.warmup).nanoseconds_per_call < 0.0 ||
      measure(native, options.warmup).nanoseconds_per_call < 0.0) {
    return fail("PocketPy benchmark warmup failed");
  }

  std::vector<double> stock_samples;
  std::vector<double> native_samples;
  stock_samples.reserve(options.samples);
  native_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement stock_measurement = measure(stock, options.iterations);
    const Measurement native_measurement = measure(native, options.iterations);
    if (stock_measurement.nanoseconds_per_call < 0.0 ||
        native_measurement.nanoseconds_per_call < 0.0 ||
        stock_measurement.checksum != native_measurement.checksum) {
      std::cerr << "PocketPy benchmark engines produced different results: "
                << std::hex << stock_measurement.checksum
                << " != " << native_measurement.checksum << '\n';
      if (py_checkexc()) {
        py_printexc();
      }
      py_finalize();
      return EXIT_FAILURE;
    }
    stock_samples.push_back(stock_measurement.nanoseconds_per_call);
    native_samples.push_back(native_measurement.nanoseconds_per_call);
    checksum ^= native_measurement.checksum;
  }

  const double stock_median = median(std::move(stock_samples));
  const double native_median = median(std::move(native_samples));
  std::cout << std::fixed << std::setprecision(3) << "{\n"
            << "  \"schema\": \"unijit.pocketpy-call.v1\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"stock_median_ns\": " << stock_median << ",\n"
            << "  \"unijit_median_ns\": " << native_median << ",\n"
            << "  \"speedup\": " << stock_median / native_median << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";

  py_finalize();
  return EXIT_SUCCESS;
}
