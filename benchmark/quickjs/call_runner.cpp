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

#include <quickjs.h>

#include "unijit_quickjs.h"

#ifndef UNIJIT_QUICKJS_BENCHMARK_SOURCE
#define UNIJIT_QUICKJS_BENCHMARK_SOURCE "benchmark/quickjs/numeric_call.js"
#endif

#ifndef UNIJIT_QUICKJS_BENCHMARK_VERSION
#define UNIJIT_QUICKJS_BENCHMARK_VERSION "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options final {
  std::size_t warmup{10000};
  std::size_t iterations{100000};
  std::size_t samples{7};
  std::string script{UNIJIT_QUICKJS_BENCHMARK_SOURCE};
};

struct Measurement final {
  double nanoseconds_per_iteration{-1.0};
  std::uint64_t checksum{0};
};

bool parse_positive_size(const char* text, std::size_t* value) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed);
    if (consumed != std::strlen(text) || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *value = static_cast<std::size_t>(parsed);
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

std::string read_file(const std::string& path) {
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

Measurement measure(JSContext* context, JSValueConst execute,
                    JSValueConst function, std::size_t iterations) {
  std::array<JSValue, 2> arguments = {
      JS_DupValue(context, function),
      JS_NewInt64(context, static_cast<std::int64_t>(iterations))};
  const auto started = Clock::now();
  JSValue result = JS_Call(context, execute, JS_UNDEFINED,
                           static_cast<int>(arguments.size()),
                           arguments.data());
  const auto elapsed = Clock::now() - started;
  for (JSValue argument : arguments) {
    JS_FreeValue(context, argument);
  }
  if (JS_IsException(result)) {
    JS_FreeValue(context, result);
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    return {};
  }
  double number = 0.0;
  if (JS_ToFloat64(context, &number, result) != 0) {
    JS_FreeValue(context, result);
    return {};
  }
  JS_FreeValue(context, result);
  const double nanoseconds =
      std::chrono::duration<double, std::nano>(elapsed).count();
  return {nanoseconds / static_cast<double>(iterations), bits(number)};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  return (values.size() & 1U) != 0
             ? values[middle]
             : (values[middle - 1] + values[middle]) / 2.0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_quickjs_benchmark [--script PATH] "
                 "[--warmup N] [--iterations N] [--samples N]\n";
    return EXIT_FAILURE;
  }
  const std::string source = read_file(options.script);
  if (source.empty()) {
    std::cerr << "unable to read the JavaScript benchmark source: "
              << options.script << '\n';
    return EXIT_FAILURE;
  }

  JSRuntime* runtime = JS_NewRuntime();
  JSContext* context = runtime == nullptr ? nullptr : JS_NewContext(runtime);
  if (context == nullptr || unijit_quickjs_install(context) != 0) {
    std::cerr << "unable to initialize the QuickJS benchmark runtime\n";
    if (context != nullptr) {
      JS_FreeContext(context);
    }
    if (runtime != nullptr) {
      JS_FreeRuntime(runtime);
    }
    return EXIT_FAILURE;
  }

  JSValue setup = JS_Eval(context, source.data(), source.size(),
                          options.script.c_str(), JS_EVAL_TYPE_GLOBAL);
  constexpr char kCompile[] =
      "unijit.compile(globalThis.unijitBenchmark.kernel)";
  JSValue native = JS_Eval(context, kCompile, std::strlen(kCompile),
                           "<unijit-quickjs-compile>", JS_EVAL_TYPE_GLOBAL);
  JSValue global = JS_GetGlobalObject(context);
  JSValue benchmark = JS_GetPropertyStr(context, global, "unijitBenchmark");
  JSValue stock = JS_GetPropertyStr(context, benchmark, "kernel");
  JSValue execute = JS_GetPropertyStr(context, benchmark, "execute");
  JS_FreeValue(context, benchmark);
  JS_FreeValue(context, global);
  if (JS_IsException(setup) || JS_IsFunction(context, stock) == 0 ||
      JS_IsFunction(context, native) == 0 ||
      JS_IsFunction(context, execute) == 0) {
    std::cerr << "unable to compile the QuickJS benchmark functions\n";
    JS_FreeValue(context, setup);
    JS_FreeValue(context, stock);
    JS_FreeValue(context, native);
    JS_FreeValue(context, execute);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, setup);

  if (measure(context, execute, stock, options.warmup)
              .nanoseconds_per_iteration < 0.0 ||
      measure(context, execute, native, options.warmup)
              .nanoseconds_per_iteration < 0.0) {
    std::cerr << "QuickJS benchmark warmup failed\n";
    JS_FreeValue(context, stock);
    JS_FreeValue(context, native);
    JS_FreeValue(context, execute);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return EXIT_FAILURE;
  }

  std::vector<double> stock_samples;
  std::vector<double> native_samples;
  stock_samples.reserve(options.samples);
  native_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  bool has_checksum = false;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement stock_measurement =
        measure(context, execute, stock, options.iterations);
    const Measurement native_measurement =
        measure(context, execute, native, options.iterations);
    if (stock_measurement.nanoseconds_per_iteration < 0.0 ||
        native_measurement.nanoseconds_per_iteration < 0.0 ||
        stock_measurement.checksum != native_measurement.checksum ||
        (has_checksum && checksum != native_measurement.checksum)) {
      std::cerr << "QuickJS benchmark engines or samples produced different "
                   "results\n";
      JS_FreeValue(context, stock);
      JS_FreeValue(context, native);
      JS_FreeValue(context, execute);
      JS_FreeContext(context);
      JS_FreeRuntime(runtime);
      return EXIT_FAILURE;
    }
    checksum = native_measurement.checksum;
    has_checksum = true;
    stock_samples.push_back(stock_measurement.nanoseconds_per_iteration);
    native_samples.push_back(native_measurement.nanoseconds_per_iteration);
  }

  const double stock_median = median(std::move(stock_samples));
  const double native_median = median(std::move(native_samples));
  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.quickjs-numeric-call.v2\",\n"
            << "  \"quickjs_version\": \""
            << UNIJIT_QUICKJS_BENCHMARK_VERSION << "\",\n"
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

  JS_FreeValue(context, stock);
  JS_FreeValue(context, native);
  JS_FreeValue(context, execute);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  return EXIT_SUCCESS;
}
