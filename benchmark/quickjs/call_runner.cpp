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

#include <quickjs.h>

#include "unijit/ir/function.h"
#include "unijit_quickjs.h"

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

std::uint64_t bits(double value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Measurement measure(JSContext* context, JSValueConst function,
                    std::size_t iterations) {
  double lhs = 1.25;
  double rhs = -7.5;
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    std::array<JSValue, 2> arguments = {
        JS_NewFloat64(context, lhs), JS_NewFloat64(context, rhs)};
    JSValue result = JS_Call(context, function, JS_UNDEFINED,
                             static_cast<int>(arguments.size()),
                             arguments.data());
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
    checksum ^= bits(number);
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

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_quickjs_benchmark [--warmup N] "
                 "[--iterations N] [--samples N]\n";
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

  constexpr char kSetup[] =
      "globalThis.stock = function(a, b) {"
      "  return (a + b) * (a - 3.25) + b * 0.75;"
      "};"
      "globalThis.native = unijit.compile(globalThis.stock);";
  JSValue setup = JS_Eval(context, kSetup, std::strlen(kSetup),
                          "<unijit-quickjs-benchmark>", JS_EVAL_TYPE_GLOBAL);
  JSValue global = JS_GetGlobalObject(context);
  JSValue stock = JS_GetPropertyStr(context, global, "stock");
  JSValue native = JS_GetPropertyStr(context, global, "native");
  JS_FreeValue(context, global);
  if (JS_IsException(setup) || JS_IsFunction(context, stock) == 0 ||
      JS_IsFunction(context, native) == 0) {
    std::cerr << "unable to compile the QuickJS benchmark functions\n";
    JS_FreeValue(context, setup);
    JS_FreeValue(context, stock);
    JS_FreeValue(context, native);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, setup);

  if (measure(context, stock, options.warmup).nanoseconds_per_call < 0.0 ||
      measure(context, native, options.warmup).nanoseconds_per_call < 0.0) {
    std::cerr << "QuickJS benchmark warmup failed\n";
    return EXIT_FAILURE;
  }

  std::vector<double> stock_samples;
  std::vector<double> native_samples;
  stock_samples.reserve(options.samples);
  native_samples.reserve(options.samples);
  std::uint64_t checksum = 0;
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    const Measurement stock_measurement =
        measure(context, stock, options.iterations);
    const Measurement native_measurement =
        measure(context, native, options.iterations);
    if (stock_measurement.nanoseconds_per_call < 0.0 ||
        native_measurement.nanoseconds_per_call < 0.0 ||
        stock_measurement.checksum != native_measurement.checksum) {
      std::cerr << "QuickJS benchmark engines produced different results: "
                << std::hex << stock_measurement.checksum << " != "
                << native_measurement.checksum << '\n';
      return EXIT_FAILURE;
    }
    stock_samples.push_back(stock_measurement.nanoseconds_per_call);
    native_samples.push_back(native_measurement.nanoseconds_per_call);
    checksum ^= native_measurement.checksum;
  }

  const double stock_median = median(std::move(stock_samples));
  const double native_median = median(std::move(native_samples));
  std::cout << std::fixed << std::setprecision(3)
            << "{\n"
            << "  \"schema\": \"unijit.quickjs-call.v1\",\n"
            << "  \"warmup_iterations\": " << options.warmup << ",\n"
            << "  \"measurement_iterations\": " << options.iterations
            << ",\n"
            << "  \"samples\": " << options.samples << ",\n"
            << "  \"stock_median_ns\": " << stock_median << ",\n"
            << "  \"unijit_median_ns\": " << native_median << ",\n"
            << "  \"speedup\": " << stock_median / native_median << ",\n"
            << "  \"checksum\": \"0x" << std::hex << checksum << "\"\n"
            << "}\n";

  JS_FreeValue(context, stock);
  JS_FreeValue(context, native);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  return EXIT_SUCCESS;
}
