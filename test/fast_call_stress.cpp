#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::ValueType;
using unijit::ir::Word;
using unijit::jit::CodeCache;
using unijit::jit::CodeHandle;
using unijit::jit::Compiler;

struct Options final {
  std::size_t readers{4};
  std::size_t invocations{20000};
  std::size_t retargets{4096};
};

bool parse_size(const char* text, std::size_t* value) {
  if (text == nullptr || value == nullptr || *text == '\0' || *text == '-') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || parsed == 0) {
    return false;
  }
  *value = static_cast<std::size_t>(parsed);
  return static_cast<unsigned long long>(*value) == parsed;
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (index + 1 >= argc) {
      return false;
    }
    std::size_t* destination = nullptr;
    if (argument == "--readers") {
      destination = &options->readers;
    } else if (argument == "--invocations") {
      destination = &options->invocations;
    } else if (argument == "--retargets") {
      destination = &options->retargets;
    } else {
      return false;
    }
    if (!parse_size(argv[++index], destination)) {
      return false;
    }
  }
  return true;
}

Function make_target(Word offset) {
  FunctionBuilder builder({ValueType::kWord, ValueType::kWord});
  const auto sum = builder.add(builder.parameter(0), builder.parameter(1));
  const auto result = offset == 0 ? sum : builder.add(sum, builder.constant(offset));
  if (!builder.set_return(result).ok()) {
    return {};
  }
  return std::move(builder).build();
}

Function make_caller() {
  FunctionBuilder builder({ValueType::kWord, ValueType::kWord});
  const auto target = builder.create_fast_call(
      {ValueType::kWord, ValueType::kWord}, ValueType::kWord);
  const auto result = builder.fast_call(
      target, {builder.parameter(0), builder.parameter(1)});
  if (!builder.set_return(result).ok()) {
    return {};
  }
  return std::move(builder).build();
}

bool publish(CodeCache* cache, const char* key, std::uint64_t fingerprint,
             Function function, CodeHandle* handle) {
  auto compilation = Compiler::compile(function);
  if (!compilation.ok()) {
    std::cerr << "compilation failed for " << key << ": "
              << compilation.status.message() << '\n';
    return false;
  }
  auto publication = cache->publish(key, fingerprint,
                                    std::move(compilation.function));
  if (!publication.ok()) {
    std::cerr << "publication failed for " << key << ": "
              << publication.status.message() << '\n';
    return false;
  }
  *handle = std::move(publication.handle);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_fast_call_stress [--readers N] "
                 "[--invocations N] [--retargets N]\n";
    return 2;
  }

  constexpr Word kOffset = 1000;
  constexpr Word kFirstResult = 42;
  constexpr Word kSecondResult = kFirstResult + kOffset;
  const Word arguments[2] = {19, 23};

  CodeCache cache;
  CodeHandle first;
  CodeHandle second;
  CodeHandle caller;
  if (!publish(&cache, "fast-stress-first", 1, make_target(0), &first) ||
      !publish(&cache, "fast-stress-second", 1, make_target(kOffset),
               &second) ||
      !publish(&cache, "fast-stress-caller", 1, make_caller(), &caller)) {
    return 1;
  }
  if (!caller.bind_fast_call(0, first).ok()) {
    std::cerr << "initial fast-call binding failed\n";
    return 1;
  }

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> completed{0};
  std::vector<std::thread> readers;
  readers.reserve(options.readers);
  for (std::size_t thread_index = 0; thread_index < options.readers;
       ++thread_index) {
    readers.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t invocation = 0; invocation < options.invocations;
           ++invocation) {
        const auto result = caller.invoke(arguments, 2);
        if (!result.ok() ||
            (result.value != kFirstResult && result.value != kSecondResult)) {
          failed.store(true, std::memory_order_release);
          return;
        }
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::size_t generation = 0; generation < options.retargets;
       ++generation) {
    const CodeHandle& target = (generation & 1U) == 0U ? second : first;
    if (!caller.bind_fast_call(0, target).ok()) {
      failed.store(true, std::memory_order_release);
      break;
    }
  }
  for (std::thread& reader : readers) {
    reader.join();
  }

  const bool invalidated_first = cache.invalidate("fast-stress-first", 1);
  const bool invalidated_second = cache.invalidate("fast-stress-second", 1);
  first = {};
  second = {};
  const auto retained = caller.invoke(arguments, 2);
  const bool clear_ok = caller.clear_fast_call(0).ok();
  const auto cleared = caller.invoke(arguments, 2);
  const std::size_t expected = options.readers * options.invocations;
  if (failed.load(std::memory_order_acquire) || completed.load() != expected ||
      !invalidated_first || !invalidated_second || !retained.ok() ||
      (retained.value != kFirstResult && retained.value != kSecondResult) ||
      !clear_ok || cleared.ok()) {
    std::cerr << "fast-call snapshot stress failed after " << completed.load()
              << " invocations\n";
    return 1;
  }

  std::cout << "{\"readers\":" << options.readers
            << ",\"invocations_per_reader\":" << options.invocations
            << ",\"retargets\":" << options.retargets
            << ",\"completed\":" << completed.load()
            << ",\"status\":\"passed\"}\n";
  return 0;
}
