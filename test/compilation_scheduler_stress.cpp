#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/compiler.h"

namespace {

struct Options final {
  std::size_t producers{8};
  std::size_t submissions{4096};
  std::size_t identities{128};
  std::size_t workers{4};
};

bool parse_size(const char* text, std::size_t* output) {
  try {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 0);
    if (consumed == 0 || text[consumed] != '\0' || value == 0 ||
        value > 1000000ULL) {
      return false;
    }
    *output = static_cast<std::size_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return false;
    }
    const std::string name = argv[index];
    std::size_t* target = nullptr;
    if (name == "--producers") {
      target = &options->producers;
    } else if (name == "--submissions") {
      target = &options->submissions;
    } else if (name == "--identities") {
      target = &options->identities;
    } else if (name == "--workers") {
      target = &options->workers;
    } else {
      return false;
    }
    if (!parse_size(argv[index + 1], target)) {
      return false;
    }
  }
  return options->identities <= options->submissions &&
         options->workers <= 256;
}

std::unique_ptr<unijit::jit::CompiledFunction> compile_value(
    unijit::ir::Word value) {
  unijit::ir::FunctionBuilder builder(0);
  if (!builder.set_return(builder.constant(value)).ok()) {
    return nullptr;
  }
  auto compilation =
      unijit::jit::Compiler::compile(std::move(builder).build());
  return compilation.ok() ? std::move(compilation.function) : nullptr;
}

unijit::jit::CompilationPriority priority(std::size_t identity) noexcept {
  switch (identity % 3) {
    case 0:
      return unijit::jit::CompilationPriority::kUrgent;
    case 1:
      return unijit::jit::CompilationPriority::kNormal;
    default:
      return unijit::jit::CompilationPriority::kBackground;
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::cerr << "usage: unijit_compilation_scheduler_stress "
                 "[--producers N] [--submissions N] [--identities N] "
                 "[--workers N]\n";
    return EXIT_FAILURE;
  }

  constexpr std::size_t kEstimatedTaskBytes = 64;
  const std::size_t queue_tasks = options.identities + options.workers;
  const std::size_t queue_bytes = queue_tasks * kEstimatedTaskBytes;
  auto creation = unijit::jit::CompilationScheduler::create(
      {options.workers, queue_tasks, queue_bytes});
  if (!creation.ok()) {
    std::cerr << "unable to create compilation scheduler: "
              << creation.status.message() << '\n';
    return EXIT_FAILURE;
  }

  unijit::jit::CodeCache cache(
      {options.identities, options.identities * 64U * 1024U});
  std::mutex start_mutex;
  std::condition_variable start_condition;
  bool start = false;
  std::atomic<std::size_t> next_submission{0};
  std::atomic<std::size_t> job_errors{0};
  std::mutex tickets_mutex;
  std::vector<unijit::jit::CompilationTicket> tickets;
  tickets.reserve(options.submissions);

  std::vector<std::thread> producers;
  producers.reserve(options.producers);
  for (std::size_t producer = 0; producer < options.producers; ++producer) {
    producers.emplace_back([&] {
      while (true) {
        const std::size_t submission =
            next_submission.fetch_add(1, std::memory_order_relaxed);
        if (submission >= options.submissions) {
          return;
        }
        const std::size_t identity = submission % options.identities;
        const std::string key = "scheduler-stress-" +
                                std::to_string(identity);
        auto result = creation.scheduler->submit_for(
            {key, 1, kEstimatedTaskBytes, priority(identity),
             [&, identity, key](
                 const unijit::jit::CompilationCancellation& cancellation) {
               {
                 std::unique_lock<std::mutex> lock(start_mutex);
                 start_condition.wait(lock, [&] {
                   return start || cancellation.stop_requested();
                 });
               }
               if (cancellation.stop_requested()) {
                 return unijit::Status{
                     unijit::StatusCode::kCancelled,
                     "scheduler stress task was cancelled"};
               }
               auto publication = cache.publish(
                   key, 1,
                   compile_value(static_cast<unijit::ir::Word>(identity + 1)));
               const auto invocation = publication.handle.invoke(nullptr, 0);
               if (!publication.ok() || !invocation.ok() ||
                   invocation.value !=
                       static_cast<unijit::ir::Word>(identity + 1)) {
                 job_errors.fetch_add(1, std::memory_order_relaxed);
                 return unijit::Status{
                     unijit::StatusCode::kCodeGenerationFailed,
                     "scheduler stress compilation produced a wrong result"};
               }
               return unijit::Status::ok_status();
             }},
            std::chrono::seconds(10));
        if (!result.ok()) {
          job_errors.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        std::lock_guard<std::mutex> lock(tickets_mutex);
        tickets.push_back(std::move(result.ticket));
      }
    });
  }
  for (std::thread& producer : producers) {
    producer.join();
  }

  if (tickets.size() != options.submissions) {
    std::cerr << "scheduler stress lost submissions\n";
    return EXIT_FAILURE;
  }
  const auto before_cancellation = creation.scheduler->stats();
  if (before_cancellation.submitted != options.identities ||
      before_cancellation.deduplicated !=
          options.submissions - options.identities) {
    std::cerr << "scheduler stress did not deduplicate its identity set\n";
    return EXIT_FAILURE;
  }

  std::unordered_set<std::uint64_t> cancelled_ids;
  for (const auto& ticket : tickets) {
    if ((ticket.id() % 17) == 0 && cancelled_ids.insert(ticket.id()).second) {
      (void)ticket.cancel();
    }
  }
  {
    std::lock_guard<std::mutex> lock(start_mutex);
    start = true;
  }
  start_condition.notify_all();

  std::size_t cancelled_observations = 0;
  std::size_t successful_observations = 0;
  for (const auto& ticket : tickets) {
    const unijit::Status outcome = ticket.wait();
    if (cancelled_ids.find(ticket.id()) != cancelled_ids.end()) {
      if (outcome.code() != unijit::StatusCode::kCancelled) {
        ++job_errors;
      }
      ++cancelled_observations;
    } else {
      if (!outcome.ok()) {
        ++job_errors;
      }
      ++successful_observations;
    }
  }
  creation.scheduler->wait_idle();
  if (!creation.scheduler->shutdown().ok()) {
    std::cerr << "scheduler stress could not drain workers\n";
    return EXIT_FAILURE;
  }

  const auto scheduler_stats = creation.scheduler->stats();
  const auto cache_stats = cache.stats();
  if (job_errors.load(std::memory_order_relaxed) != 0 ||
      cancelled_ids.empty() || cancelled_observations == 0 ||
      successful_observations == 0 || scheduler_stats.failed != 0 ||
      scheduler_stats.succeeded + scheduler_stats.cancelled !=
          scheduler_stats.submitted ||
      cache_stats.resident_entries != scheduler_stats.succeeded ||
      scheduler_stats.queued_tasks != 0 ||
      scheduler_stats.active_workers != 0 || scheduler_stats.accepting) {
    std::cerr << "scheduler stress lifecycle invariants failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "{\"schema\":\"unijit.compilation-scheduler-stress.v1\","
            << "\"producers\":" << options.producers << ','
            << "\"submissions\":" << options.submissions << ','
            << "\"identities\":" << options.identities << ','
            << "\"workers\":" << options.workers << ','
            << "\"deduplicated\":" << scheduler_stats.deduplicated << ','
            << "\"succeeded\":" << scheduler_stats.succeeded << ','
            << "\"cancelled\":" << scheduler_stats.cancelled << ','
            << "\"peak_queued_tasks\":"
            << scheduler_stats.peak_queued_tasks << "}\n";
  return EXIT_SUCCESS;
}
