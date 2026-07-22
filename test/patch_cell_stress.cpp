#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/target.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::PatchCellKind;
using unijit::ir::Word;
using unijit::jit::CodeCache;
using unijit::jit::CodeHandle;
using unijit::jit::CompilationOptions;
using unijit::jit::Compiler;

struct Options final {
  std::size_t publication_iterations{512};
  std::size_t increments_per_writer{1000};
};

constexpr std::size_t kWriterThreads = 8;
constexpr std::size_t kReaderThreads = 4;

bool parse_positive(std::string_view text, std::size_t* output) {
  if (text.empty() || output == nullptr) {
    return false;
  }
  std::size_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0) {
    return false;
  }
  *output = value;
  return true;
}

bool parse_options(int argc, char** argv, Options* options) {
  if (options == nullptr) {
    return false;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (index + 1 >= argc) {
      return false;
    }
    const std::string_view value(argv[++index]);
    if (option == "--publication-iterations") {
      if (!parse_positive(value, &options->publication_iterations)) {
        return false;
      }
    } else if (option == "--increments") {
      if (!parse_positive(value, &options->increments_per_writer)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

Function build_patch_reader(PatchCellKind kind) {
  FunctionBuilder builder(0);
  const auto cell = builder.create_patch_cell(0, kind);
  if (!builder.set_return(builder.load_patch_cell(cell)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

bool run_release_acquire_publication(
    const unijit::jit::CompiledFunction& function, std::size_t iterations) {
  std::atomic<Word> payload{0};
  std::atomic<std::size_t> acknowledged{0};
  std::atomic<bool> failed{false};

  std::thread producer([&]() {
    for (std::size_t generation = 1; generation <= iterations; ++generation) {
      while (acknowledged.load(std::memory_order_acquire) != generation - 1) {
        if (failed.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::yield();
      }
      payload.store(static_cast<Word>(generation), std::memory_order_relaxed);
      if (!function
               .publish_patch_cell(0, static_cast<Word>(generation))
               .ok()) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });

  std::thread consumer([&]() {
    for (std::size_t generation = 1; generation <= iterations; ++generation) {
      unijit::ir::EvaluationResult observed;
      do {
        observed = function.invoke(nullptr, 0);
        if (!observed.ok()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        if (observed.value != static_cast<Word>(generation)) {
          std::this_thread::yield();
        }
      } while (observed.value != static_cast<Word>(generation));
      if (payload.load(std::memory_order_relaxed) !=
          static_cast<Word>(generation)) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      acknowledged.store(generation, std::memory_order_release);
    }
  });

  producer.join();
  consumer.join();
  return !failed.load(std::memory_order_relaxed) &&
         acknowledged.load(std::memory_order_relaxed) == iterations;
}

bool run_contended_counter(CodeHandle lease, std::size_t increments,
                           Word* final_value) {
  if (!lease || final_value == nullptr) {
    return false;
  }
  std::atomic<std::size_t> writers_done{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  threads.reserve(kWriterThreads + kReaderThreads);

  for (std::size_t writer = 0; writer < kWriterThreads; ++writer) {
    threads.emplace_back([&, lease]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t iteration = 0; iteration < increments; ++iteration) {
        if (!lease.fetch_add_patch_cell(0, 1).ok()) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
      }
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }
  for (std::size_t reader = 0; reader < kReaderThreads; ++reader) {
    threads.emplace_back([&, lease]() {
      Word previous = 0;
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      while (writers_done.load(std::memory_order_acquire) < kWriterThreads) {
        const auto observed = lease.invoke(nullptr, 0);
        if (!observed.ok() || observed.value < previous) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        previous = observed.value;
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto result = lease.invoke(nullptr, 0);
  if (!result.ok()) {
    return false;
  }
  *final_value = result.value;
  return !failed.load(std::memory_order_relaxed) &&
         result.value ==
             static_cast<Word>(kWriterThreads * increments);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_patch_cell_stress "
                 "[--publication-iterations N] [--increments N]\n";
    return 2;
  }

  CompilationOptions compilation_options;
  compilation_options.target_profile = unijit::jit::host_target_profile();
  auto publication_compilation = Compiler::compile(
      build_patch_reader(PatchCellKind::kGeneration), compilation_options);
  if (!publication_compilation.ok() ||
      !run_release_acquire_publication(*publication_compilation.function,
                                       options.publication_iterations)) {
    std::cerr << "patch-cell release/acquire publication failed\n";
    return 1;
  }

  auto counter_compilation = Compiler::compile(
      build_patch_reader(PatchCellKind::kCounter), compilation_options);
  CodeCache cache({1, 64U * 1024U}, compilation_options.target_profile);
  auto publication =
      counter_compilation.ok()
          ? cache.publish("patch-cell-stress", UINT64_C(0x504154434843454c),
                          std::move(counter_compilation.function))
          : unijit::jit::CodeCachePublication{
                counter_compilation.status, {}, false, false};
  Word final_counter = 0;
  if (!publication.ok() ||
      !run_contended_counter(publication.handle,
                             options.increments_per_writer, &final_counter)) {
    std::cerr << "patch-cell contention failed\n";
    return 1;
  }

  CodeHandle retained = publication.handle;
  if (!cache.invalidate("patch-cell-stress",
                        UINT64_C(0x504154434843454c)) ||
      cache.find("patch-cell-stress", UINT64_C(0x504154434843454c)) ||
      !retained.publish_patch_cell(0, 17).ok()) {
    std::cerr << "patch-cell invalidation lease failed\n";
    return 1;
  }
  const auto retained_result = retained.invoke(nullptr, 0);
  if (!retained_result.ok() || retained_result.value != 17) {
    std::cerr << "retained patch-cell generation became unsafe\n";
    return 1;
  }

  std::cout << "{\"schema\":\"unijit.patch-cell-stress.v1\","
            << "\"architecture\":\""
            << unijit::jit::target_architecture_name(
                   compilation_options.target_profile.architecture)
            << "\",\"publication_iterations\":"
            << options.publication_iterations << ",\"writer_threads\":"
            << kWriterThreads << ",\"reader_threads\":" << kReaderThreads
            << ",\"increments_per_writer\":"
            << options.increments_per_writer << ",\"final_counter\":"
            << final_counter << ",\"retained_after_invalidation\":true}\n";
  return 0;
}
