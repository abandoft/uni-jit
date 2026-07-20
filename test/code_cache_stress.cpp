#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"

namespace {

using unijit::ir::FunctionBuilder;
using unijit::ir::Word;
using unijit::jit::CodeCache;
using unijit::jit::CodeCacheLimits;
using unijit::jit::CodeHandle;

struct Options final {
  std::uint64_t seed{0x4341434845535452ULL};
  std::size_t readers{8};
  std::size_t writers{2};
  std::size_t reads{25000};
  std::size_t publications{2000};
  std::size_t keys{16};
};

struct RetainedLease final {
  CodeHandle handle;
  Word expected{0};
};

Word word_from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

Word expected_value(std::size_t key, std::uint64_t fingerprint) noexcept {
  const std::uint64_t bits =
      0x554E494A49544348ULL ^
      (static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ULL) ^
      (fingerprint * 0xD6E8FEB86659FD93ULL);
  return word_from_bits(bits);
}

std::unique_ptr<unijit::jit::CompiledFunction> compile_constant(Word value) {
  FunctionBuilder builder(0);
  if (!builder.set_return(builder.constant(value)).ok()) {
    return nullptr;
  }
  auto compilation =
      unijit::jit::Compiler::compile(std::move(builder).build());
  return compilation.ok() ? std::move(compilation.function) : nullptr;
}

bool invoke_matches(const CodeHandle& handle, Word expected) {
  const auto result = handle.invoke(nullptr, 0);
  return result.ok() && result.value == expected;
}

bool parse_size(const char* text, std::size_t* result) {
  try {
    const std::uint64_t value = std::stoull(text, nullptr, 0);
    if (value == 0 || value > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *result = static_cast<std::size_t>(value);
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
    if (name == "--seed") {
      try {
        options->seed = std::stoull(argv[index + 1], nullptr, 0);
      } catch (const std::exception&) {
        return false;
      }
    } else if (name == "--readers") {
      if (!parse_size(argv[index + 1], &options->readers)) {
        return false;
      }
    } else if (name == "--writers") {
      if (!parse_size(argv[index + 1], &options->writers)) {
        return false;
      }
    } else if (name == "--reads") {
      if (!parse_size(argv[index + 1], &options->reads)) {
        return false;
      }
    } else if (name == "--publications") {
      if (!parse_size(argv[index + 1], &options->publications)) {
        return false;
      }
    } else if (name == "--keys") {
      if (!parse_size(argv[index + 1], &options->keys)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return options->readers <= 128 && options->writers <= 32 &&
         options->reads <= 10000000 && options->publications <= 1000000 &&
         options->keys <= 4096;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_code_cache_stress [--seed N] "
                 "[--readers N] [--writers N] [--reads N] "
                 "[--publications N] [--keys N]\n";
    return EXIT_FAILURE;
  }

  std::vector<std::string> keys;
  keys.reserve(options.keys);
  for (std::size_t index = 0; index < options.keys; ++index) {
    keys.push_back("stress-key-" + std::to_string(index));
  }

  const std::size_t maximum_entries =
      std::max<std::size_t>(1, std::min<std::size_t>(8, options.keys / 2));
  CodeCache cache(CodeCacheLimits{maximum_entries, 8U * 1024U * 1024U});
  CodeHandle surviving_clear;
  for (std::size_t key = 0; key < options.keys; ++key) {
    auto publication = cache.publish(
        keys[key], 1, compile_constant(expected_value(key, 1)));
    if (!publication.ok() || !invoke_matches(publication.handle,
                                              expected_value(key, 1))) {
      std::cerr << "unable to initialize code-cache stress fixture\n";
      return EXIT_FAILURE;
    }
    if (key == 0) {
      surviving_clear = publication.handle;
    }
  }

  std::atomic<bool> start{false};
  std::atomic<std::uint64_t> errors{0};
  std::vector<std::thread> threads;
  threads.reserve(options.readers + options.writers);

  for (std::size_t reader = 0; reader < options.readers; ++reader) {
    threads.emplace_back([&, reader] {
      std::mt19937_64 random(options.seed ^
                             (0x9E3779B97F4A7C15ULL * (reader + 1)));
      std::array<RetainedLease, 64> retained{};
      std::size_t retained_cursor = 0;
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t operation = 0; operation < options.reads; ++operation) {
        const std::size_t key = random() % options.keys;
        const std::uint64_t fingerprint = 1 + (random() % 4ULL);
        CodeHandle handle = cache.find(keys[key], fingerprint);
        if (handle.valid()) {
          const Word expected = expected_value(key, fingerprint);
          if (!invoke_matches(handle, expected)) {
            errors.fetch_add(1, std::memory_order_relaxed);
          }
          retained[retained_cursor] = {std::move(handle), expected};
          retained_cursor = (retained_cursor + 1) % retained.size();
        }
        if ((operation % 127U) == 0) {
          const RetainedLease& lease = retained[random() % retained.size()];
          if (lease.handle.valid() &&
              !invoke_matches(lease.handle, lease.expected)) {
            errors.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      for (const RetainedLease& lease : retained) {
        if (lease.handle.valid() &&
            !invoke_matches(lease.handle, lease.expected)) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::size_t writer = 0; writer < options.writers; ++writer) {
    threads.emplace_back([&, writer] {
      std::mt19937_64 random(options.seed ^
                             (0xD6E8FEB86659FD93ULL * (writer + 1)));
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t operation = 0; operation < options.publications;
           ++operation) {
        const std::size_t key = random() % options.keys;
        const std::uint64_t fingerprint = 1 + (random() % 4ULL);
        const Word expected = expected_value(key, fingerprint);
        auto publication = cache.publish(keys[key], fingerprint,
                                         compile_constant(expected));
        if (!publication.ok() ||
            !invoke_matches(publication.handle, expected)) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        if ((operation % 31U) == 0) {
          (void)cache.invalidate(keys[key], fingerprint);
        }
        if ((operation % 97U) == 0) {
          (void)cache.invalidate(keys[random() % options.keys]);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }

  const auto before_clear = cache.stats();
  cache.clear();
  const auto after_clear = cache.stats();
  if (errors.load(std::memory_order_relaxed) != 0 ||
      before_clear.lookups != options.readers * options.reads ||
      before_clear.publications !=
          options.keys + options.writers * options.publications ||
      before_clear.evictions == 0 ||
      before_clear.resident_entries > maximum_entries ||
      after_clear.resident_entries != 0 ||
      after_clear.resident_code_bytes != 0 || after_clear.clears != 1 ||
      !invoke_matches(surviving_clear, expected_value(0, 1))) {
    std::cerr << "code-cache stress invariants failed: errors="
              << errors.load(std::memory_order_relaxed)
              << " lookups=" << before_clear.lookups
              << " publications=" << before_clear.publications
              << " evictions=" << before_clear.evictions
              << " resident_entries=" << before_clear.resident_entries
              << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "{\"schema\":\"unijit.code-cache-stress.v1\","
            << "\"seed\":\"0x" << std::hex << options.seed << std::dec
            << "\",\"readers\":" << options.readers
            << ",\"writers\":" << options.writers
            << ",\"lookups\":" << before_clear.lookups
            << ",\"hits\":" << before_clear.hits
            << ",\"misses\":" << before_clear.misses
            << ",\"publications\":" << before_clear.publications
            << ",\"replacements\":" << before_clear.replacements
            << ",\"invalidations\":" << before_clear.invalidations
            << ",\"evictions\":" << before_clear.evictions << "}\n";
  return EXIT_SUCCESS;
}
