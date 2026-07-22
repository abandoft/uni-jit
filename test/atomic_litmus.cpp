#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/target.h"
#include "unijit/runtime/execution_context.h"

namespace {

using unijit::ir::AtomicAccessDescriptor;
using unijit::ir::AtomicMemoryOrder;
using unijit::ir::ControlFlowBuilder;
using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::MemoryWidth;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::CompilationOptions;
using unijit::jit::Compiler;
using unijit::jit::TargetArchitecture;
using unijit::jit::TargetFeature;
using unijit::runtime::ExecutionContext;
using unijit::runtime::MemoryRegion;

constexpr std::size_t kMaximumConsumerSpins = 1000000;
constexpr std::size_t kCasThreadCount = 4;

struct Options final {
  std::size_t iterations{512};
  std::size_t cas_successes{500};
};

AtomicAccessDescriptor access(AtomicMemoryOrder order) {
  AtomicAccessDescriptor descriptor;
  descriptor.memory.width = MemoryWidth::k64;
  descriptor.memory.alignment = sizeof(std::uint64_t);
  descriptor.order = order;
  return descriptor;
}

bool parse_positive(std::string_view text, std::size_t* value) {
  std::size_t parsed = 0;
  const auto conversion =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (conversion.ec != std::errc{} ||
      conversion.ptr != text.data() + text.size() || parsed == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if ((argument == "--iterations" || argument == "--cas-successes") &&
        index + 1 < argc) {
      std::size_t value = 0;
      if (!parse_positive(argv[++index], &value)) {
        return false;
      }
      if (argument == "--iterations") {
        options->iterations = value;
      } else {
        options->cas_successes = value;
      }
      continue;
    }
    return false;
  }
  return true;
}

Function build_message_producer() {
  FunctionBuilder builder(1, 1);
  const Value data_offset = builder.constant(0);
  const Value flag_offset = builder.constant(sizeof(std::uint64_t));
  builder.atomic_store(data_offset, builder.parameter(0),
                       access(AtomicMemoryOrder::kRelaxed), 1);
  const Value published = builder.atomic_store(
      flag_offset, builder.constant(1), access(AtomicMemoryOrder::kRelease), 2);
  if (!builder.set_return(published).ok()) {
    return {};
  }
  return std::move(builder).build();
}

unijit::ir::ControlFlowFunction build_message_consumer() {
  ControlFlowBuilder builder(0, 1);
  const auto ready = builder.create_block(0);
  const auto waiting = builder.create_block(0);
  const Value flag = builder.atomic_load(
      builder.constant(sizeof(std::uint64_t)),
      access(AtomicMemoryOrder::kAcquire), 3);
  if (!builder.branch(flag, ready, {}, waiting, {}).ok() ||
      !builder.set_insertion_block(ready).ok()) {
    return {};
  }
  const Value data = builder.atomic_load(
      builder.constant(0), access(AtomicMemoryOrder::kRelaxed), 4);
  if (!builder.set_return(data).ok() ||
      !builder.set_insertion_block(waiting).ok() ||
      !builder.set_return(builder.constant(0)).ok()) {
    return {};
  }
  return std::move(builder).build();
}

Function build_store_buffering_thread(std::size_t stored_offset,
                                      std::size_t loaded_offset,
                                      std::size_t site) {
  FunctionBuilder builder(0, 1);
  builder.atomic_store(builder.constant(static_cast<Word>(stored_offset)),
                       builder.constant(1),
                       access(AtomicMemoryOrder::kSequentiallyConsistent),
                       site);
  const Value observed = builder.atomic_load(
      builder.constant(static_cast<Word>(loaded_offset)),
      access(AtomicMemoryOrder::kSequentiallyConsistent), site + 1);
  if (!builder.set_return(observed).ok()) {
    return {};
  }
  return std::move(builder).build();
}

Function build_compare_exchange_increment() {
  FunctionBuilder builder(0, 1);
  const Value offset = builder.constant(0);
  const Value observed = builder.atomic_load(
      offset, access(AtomicMemoryOrder::kRelaxed), 9);
  const Value desired = builder.add(observed, builder.constant(1));
  AtomicAccessDescriptor compare =
      access(AtomicMemoryOrder::kSequentiallyConsistent);
  compare.failure_order = AtomicMemoryOrder::kAcquire;
  const auto result =
      builder.atomic_compare_exchange(offset, observed, desired, compare, 10);
  if (!builder.set_return(result.success).ok()) {
    return {};
  }
  return std::move(builder).build();
}

void wait_until_ready(const std::atomic<std::size_t>& ready,
                      std::size_t expected) {
  while (ready.load(std::memory_order_acquire) != expected) {
    std::this_thread::yield();
  }
}

bool bind_region(ExecutionContext* context, MemoryRegion* region,
                 std::uint64_t* cells, std::size_t count) {
  *region = MemoryRegion{cells, count * sizeof(*cells), true};
  return context->bind_memory_regions(region, 1).ok();
}

bool run_message_passing(
    const unijit::jit::CompiledFunction& producer,
    const unijit::jit::CompiledFunction& consumer, std::size_t iterations) {
  alignas(8) std::array<std::uint64_t, 2> cells{};
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    cells[0] = 0;
    cells[1] = 0;
    const Word token = static_cast<Word>(iteration + 1);
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::thread reader([&] {
      ExecutionContext context;
      MemoryRegion region;
      if (!bind_region(&context, &region, cells.data(), cells.size())) {
        failed.store(true, std::memory_order_relaxed);
        ready.store(true, std::memory_order_release);
        return;
      }
      ready.store(true, std::memory_order_release);
      for (std::size_t spin = 0; spin < kMaximumConsumerSpins; ++spin) {
        const auto result = consumer.invoke(nullptr, 0, &context);
        if (!result.ok()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        if (result.value != 0) {
          if (result.value != token) {
            failed.store(true, std::memory_order_relaxed);
          }
          return;
        }
      }
      failed.store(true, std::memory_order_relaxed);
    });
    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    ExecutionContext context;
    MemoryRegion region;
    const std::array<Word, 1> arguments = {token};
    const auto result =
        bind_region(&context, &region, cells.data(), cells.size())
            ? producer.invoke(arguments.data(), arguments.size(), &context)
            : unijit::ir::EvaluationResult{};
    reader.join();
    if (!result.ok() || failed.load(std::memory_order_relaxed) ||
        cells[0] != static_cast<std::uint64_t>(token) || cells[1] != 1) {
      return false;
    }
  }
  return true;
}

bool run_store_buffering(
    const unijit::jit::CompiledFunction& first,
    const unijit::jit::CompiledFunction& second, std::size_t iterations) {
  alignas(8) std::array<std::uint64_t, 2> cells{};
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    cells[0] = 0;
    cells[1] = 0;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::array<Word, 2> observations{};
    std::atomic<bool> failed{false};
    const auto invoke = [&](std::size_t index,
                            const unijit::jit::CompiledFunction& function) {
      ExecutionContext context;
      MemoryRegion region;
      if (!bind_region(&context, &region, cells.data(), cells.size())) {
        failed.store(true, std::memory_order_relaxed);
        ready.fetch_add(1, std::memory_order_release);
        return;
      }
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto result = function.invoke(nullptr, 0, &context);
      if (!result.ok()) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      observations[index] = result.value;
    };
    std::thread first_thread(invoke, 0, std::cref(first));
    std::thread second_thread(invoke, 1, std::cref(second));
    wait_until_ready(ready, 2);
    start.store(true, std::memory_order_release);
    first_thread.join();
    second_thread.join();
    if (failed.load(std::memory_order_relaxed) ||
        (observations[0] == 0 && observations[1] == 0) || cells[0] != 1 ||
        cells[1] != 1) {
      return false;
    }
  }
  return true;
}

bool run_compare_exchange_stress(
    const unijit::jit::CompiledFunction& increment,
    std::size_t successes_per_thread) {
  alignas(8) std::uint64_t cell = 0;
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  threads.reserve(kCasThreadCount);
  for (std::size_t index = 0; index < kCasThreadCount; ++index) {
    threads.emplace_back([&] {
      ExecutionContext context;
      MemoryRegion region;
      if (!bind_region(&context, &region, &cell, 1)) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      std::size_t successes = 0;
      while (successes < successes_per_thread) {
        const auto result = increment.invoke(nullptr, 0, &context);
        if (!result.ok()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        successes += result.value != 0 ? 1U : 0U;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return !failed.load(std::memory_order_relaxed) &&
         cell == kCasThreadCount * successes_per_thread;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_atomic_litmus [--iterations N] "
                 "[--cas-successes N]\n";
    return 2;
  }

  const auto host = unijit::jit::host_target_profile();
  if (host.architecture == TargetArchitecture::kRiscV64 &&
      !unijit::jit::has_target_feature(host, TargetFeature::kRiscVAtomic)) {
    std::cout << "atomic litmus skipped: RISC-V A extension unavailable\n";
    return 77;
  }

  CompilationOptions compilation_options;
  compilation_options.target_profile = host;
  const auto producer = Compiler::compile(build_message_producer(),
                                          compilation_options);
  const auto consumer = Compiler::compile(build_message_consumer(),
                                          compilation_options);
  const auto first = Compiler::compile(
      build_store_buffering_thread(0, sizeof(std::uint64_t), 5),
      compilation_options);
  const auto second = Compiler::compile(
      build_store_buffering_thread(sizeof(std::uint64_t), 0, 7),
      compilation_options);
  const auto increment = Compiler::compile(build_compare_exchange_increment(),
                                           compilation_options);
  if (!producer.ok() || !consumer.ok() || !first.ok() || !second.ok() ||
      !increment.ok()) {
    std::cerr << "atomic litmus compilation failed\n";
    return 1;
  }

  if (!run_message_passing(*producer.function, *consumer.function,
                           options.iterations)) {
    std::cerr << "release/acquire message-passing litmus failed\n";
    return 1;
  }
  if (!run_store_buffering(*first.function, *second.function,
                           options.iterations)) {
    std::cerr << "sequentially consistent store-buffering litmus failed\n";
    return 1;
  }
  if (!run_compare_exchange_stress(*increment.function,
                                   options.cas_successes)) {
    std::cerr << "contended compare-exchange stress failed\n";
    return 1;
  }

  std::cout << "{\"schema\":\"unijit.atomic-litmus.v1\","
            << "\"architecture\":\""
            << unijit::jit::target_architecture_name(host.architecture)
            << "\",\"message_passing_iterations\":" << options.iterations
            << ",\"store_buffering_iterations\":" << options.iterations
            << ",\"cas_successes\":"
            << kCasThreadCount * options.cas_successes << "}\n";
  return 0;
}
