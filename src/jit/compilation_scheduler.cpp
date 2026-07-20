#include "unijit/jit/compilation_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unijit::jit {
namespace {

constexpr std::size_t kMaximumWorkers = 256;
constexpr std::size_t kMaximumIdentityBytes = 4096;
constexpr std::size_t kUrgentBurst = 4;
constexpr std::size_t kNormalBurst = 2;

void saturating_increment(std::uint64_t* value) noexcept {
  if (*value != std::numeric_limits<std::uint64_t>::max()) {
    ++*value;
  }
}

bool terminal(CompilationTaskState state) noexcept {
  return state == CompilationTaskState::kSucceeded ||
         state == CompilationTaskState::kFailed ||
         state == CompilationTaskState::kCancelled;
}

std::size_t priority_lane(CompilationPriority priority) noexcept {
  return static_cast<std::size_t>(priority);
}

Status cancelled_status() {
  return {StatusCode::kCancelled, "compilation task was cancelled"};
}

}  // namespace

namespace detail {

struct CompilationTask final {
  CompilationTask(std::uint64_t task_id, std::string task_identity,
                  std::uint64_t task_generation, std::size_t task_bytes,
                  CompilationPriority task_priority, CompilationJob task_job)
      : id(task_id),
        identity(std::move(task_identity)),
        generation(task_generation),
        estimated_bytes(task_bytes),
        priority(task_priority),
        job(std::move(task_job)) {}

  std::uint64_t id{0};
  std::string identity;
  std::uint64_t generation{0};
  std::size_t estimated_bytes{0};
  CompilationPriority priority{CompilationPriority::kNormal};
  CompilationJob job;
  std::atomic<CompilationTaskState> state{CompilationTaskState::kQueued};
  std::atomic<bool> cancellation_requested{false};
  mutable std::mutex completion_mutex;
  mutable std::condition_variable completion;
  Status outcome{StatusCode::kUnavailable,
                 "compilation task has not completed"};
  bool completed{false};
};

struct CompilationSchedulerState final {
  explicit CompilationSchedulerState(CompilationSchedulerOptions configured,
                                     std::size_t workers)
      : options(configured) {
    statistics.worker_count = workers;
    statistics.accepting = true;
  }

  CompilationSchedulerOptions options;
  mutable std::mutex mutex;
  std::condition_variable work_available;
  std::condition_variable queue_space;
  std::condition_variable idle;
  std::array<std::deque<std::shared_ptr<CompilationTask>>, 3> queues;
  std::unordered_map<
      std::string,
      std::unordered_map<std::uint64_t, std::shared_ptr<CompilationTask>>>
      identities;
  CompilationSchedulerStats statistics;
  std::uint64_t next_task_id{1};
  std::size_t urgent_burst{0};
  std::size_t normal_burst{0};
};

struct CompilationSchedulerAccess final {
  static CompilationCancellation cancellation(
      const std::shared_ptr<CompilationTask>& task) {
    return CompilationCancellation(task);
  }
};

}  // namespace detail

namespace {

using SharedState = detail::CompilationSchedulerState;
using Task = detail::CompilationTask;

std::shared_ptr<Task> find_task_locked(const SharedState& scheduler,
                                       const std::string& identity,
                                       std::uint64_t generation) {
  const auto group = scheduler.identities.find(identity);
  if (group == scheduler.identities.end()) {
    return {};
  }
  const auto task = group->second.find(generation);
  return task == group->second.end() ? std::shared_ptr<Task>{} : task->second;
}

void erase_task_locked(SharedState* scheduler,
                       const std::shared_ptr<Task>& task) {
  const auto group = scheduler->identities.find(task->identity);
  if (group == scheduler->identities.end()) {
    return;
  }
  const auto candidate = group->second.find(task->generation);
  if (candidate != group->second.end() && candidate->second == task) {
    group->second.erase(candidate);
  }
  if (group->second.empty()) {
    scheduler->identities.erase(group);
  }
}

void complete_task_locked(const std::shared_ptr<Task>& task,
                          CompilationTaskState state, Status outcome) {
  {
    std::lock_guard<std::mutex> completion_lock(task->completion_mutex);
    task->outcome = std::move(outcome);
    task->state.store(state, std::memory_order_release);
    task->completed = true;
  }
  task->completion.notify_all();
}

bool queue_has_capacity(const SharedState& scheduler,
                        std::size_t estimated_bytes) noexcept {
  return scheduler.statistics.queued_tasks <
             scheduler.options.maximum_queued_tasks &&
         estimated_bytes <= scheduler.options.maximum_queued_bytes -
                                scheduler.statistics.queued_bytes;
}

std::shared_ptr<Task> pop_task_locked(SharedState* scheduler) {
  std::size_t lane = 0;
  if (!scheduler->queues[2].empty() &&
      (scheduler->urgent_burst < kUrgentBurst ||
       (scheduler->queues[1].empty() && scheduler->queues[0].empty()))) {
    lane = 2;
    ++scheduler->urgent_burst;
  } else if (!scheduler->queues[1].empty() &&
             (scheduler->normal_burst < kNormalBurst ||
              scheduler->queues[0].empty())) {
    lane = 1;
    scheduler->urgent_burst = 0;
    ++scheduler->normal_burst;
  } else if (!scheduler->queues[0].empty()) {
    lane = 0;
    scheduler->urgent_burst = 0;
    scheduler->normal_burst = 0;
  } else if (!scheduler->queues[2].empty()) {
    lane = 2;
    scheduler->urgent_burst = 1;
  } else if (!scheduler->queues[1].empty()) {
    lane = 1;
    scheduler->urgent_burst = 0;
    scheduler->normal_burst = 1;
  } else {
    return {};
  }
  std::shared_ptr<Task> task = std::move(scheduler->queues[lane].front());
  scheduler->queues[lane].pop_front();
  --scheduler->statistics.queued_tasks;
  scheduler->statistics.queued_bytes -= task->estimated_bytes;
  ++scheduler->statistics.active_workers;
  scheduler->statistics.peak_active_workers =
      std::max(scheduler->statistics.peak_active_workers,
               scheduler->statistics.active_workers);
  saturating_increment(&scheduler->statistics.started);
  task->state.store(CompilationTaskState::kRunning,
                    std::memory_order_release);
  scheduler->queue_space.notify_all();
  return task;
}

void run_worker(const std::shared_ptr<SharedState>& scheduler) {
  while (true) {
    std::shared_ptr<Task> task;
    {
      std::unique_lock<std::mutex> lock(scheduler->mutex);
      scheduler->work_available.wait(lock, [&] {
        return scheduler->statistics.queued_tasks != 0 ||
               !scheduler->statistics.accepting;
      });
      if (scheduler->statistics.queued_tasks == 0) {
        return;
      }
      task = pop_task_locked(scheduler.get());
    }

    Status outcome;
    try {
      outcome = task->job(
          detail::CompilationSchedulerAccess::cancellation(task));
    } catch (const std::bad_alloc&) {
      outcome = {StatusCode::kResourceExhausted, {}};
    } catch (const std::exception&) {
      outcome = {StatusCode::kCodeGenerationFailed,
                 "compilation job threw an exception"};
    } catch (...) {
      outcome = {StatusCode::kCodeGenerationFailed,
                 "compilation job threw a non-standard exception"};
    }

    const bool cancelled =
        task->cancellation_requested.load(std::memory_order_acquire) ||
        outcome.code() == StatusCode::kCancelled;
    const CompilationTaskState final_state =
        cancelled ? CompilationTaskState::kCancelled
                  : outcome.ok() ? CompilationTaskState::kSucceeded
                                 : CompilationTaskState::kFailed;
    if (cancelled && outcome.code() != StatusCode::kCancelled) {
      outcome = cancelled_status();
    }

    CompilationJob completed_job;
    {
      std::lock_guard<std::mutex> lock(scheduler->mutex);
      --scheduler->statistics.active_workers;
      erase_task_locked(scheduler.get(), task);
      if (final_state == CompilationTaskState::kSucceeded) {
        saturating_increment(&scheduler->statistics.succeeded);
      } else if (final_state == CompilationTaskState::kCancelled) {
        saturating_increment(&scheduler->statistics.cancelled);
      } else {
        saturating_increment(&scheduler->statistics.failed);
      }
      completed_job = std::move(task->job);
      complete_task_locked(task, final_state, std::move(outcome));
      if (scheduler->statistics.queued_tasks == 0 &&
          scheduler->statistics.active_workers == 0) {
        scheduler->idle.notify_all();
      }
      scheduler->queue_space.notify_all();
    }
  }
}

std::size_t resolved_worker_count(std::size_t requested) noexcept {
  if (requested != 0) {
    return requested;
  }
  const unsigned int detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1 : std::min<std::size_t>(detected, kMaximumWorkers);
}

Status validate_options(const CompilationSchedulerOptions& options,
                        std::size_t workers) {
  if (workers == 0 || workers > kMaximumWorkers) {
    return {StatusCode::kInvalidArgument,
            "compilation scheduler worker count is outside its limit"};
  }
  if (options.maximum_queued_tasks == 0 ||
      options.maximum_queued_bytes == 0) {
    return {StatusCode::kInvalidArgument,
            "compilation scheduler queue limits must be nonzero"};
  }
  return Status::ok_status();
}

Status validate_request(const CompilationRequest& request,
                        const CompilationSchedulerOptions& options) {
  if (request.identity.empty() ||
      request.identity.size() > kMaximumIdentityBytes) {
    return {StatusCode::kInvalidArgument,
            "compilation identity is empty or exceeds 4096 bytes"};
  }
  if (request.estimated_bytes == 0) {
    return {StatusCode::kInvalidArgument,
            "compilation request must estimate a nonzero byte cost"};
  }
  if (request.estimated_bytes > options.maximum_queued_bytes) {
    return {StatusCode::kResourceExhausted,
            "compilation request exceeds the scheduler byte budget"};
  }
  if (!request.job) {
    return {StatusCode::kInvalidArgument,
            "compilation request has no job"};
  }
  switch (request.priority) {
    case CompilationPriority::kBackground:
    case CompilationPriority::kNormal:
    case CompilationPriority::kUrgent:
      return Status::ok_status();
    default:
      return {StatusCode::kInvalidArgument,
              "compilation request has an unknown priority"};
  }
}

}  // namespace

struct CompilationScheduler::Impl final {
  explicit Impl(std::shared_ptr<SharedState> shared)
      : state(std::move(shared)) {}

  std::shared_ptr<SharedState> state;
  std::vector<std::thread> workers;
  std::mutex shutdown_mutex;
  bool joined{false};
};

CompilationCancellation::CompilationCancellation(
    std::shared_ptr<detail::CompilationTask> task) noexcept
    : task_(std::move(task)) {}

bool CompilationCancellation::stop_requested() const noexcept {
  return task_ != nullptr &&
         task_->cancellation_requested.load(std::memory_order_acquire);
}

CompilationTicket::CompilationTicket(
    std::shared_ptr<detail::CompilationTask> task,
    std::weak_ptr<detail::CompilationSchedulerState> scheduler) noexcept
    : task_(std::move(task)), scheduler_(std::move(scheduler)) {}

std::uint64_t CompilationTicket::id() const noexcept {
  return task_ == nullptr ? 0 : task_->id;
}

CompilationTaskState CompilationTicket::state() const noexcept {
  return task_ == nullptr
             ? CompilationTaskState::kInvalid
             : task_->state.load(std::memory_order_acquire);
}

bool CompilationTicket::cancellation_requested() const noexcept {
  return task_ != nullptr &&
         task_->cancellation_requested.load(std::memory_order_acquire);
}

bool CompilationTicket::cancel() const {
  if (task_ == nullptr) {
    return false;
  }
  const std::shared_ptr<SharedState> scheduler = scheduler_.lock();
  if (scheduler == nullptr) {
    if (terminal(state())) {
      return false;
    }
    return !task_->cancellation_requested.exchange(
        true, std::memory_order_acq_rel);
  }

  CompilationJob cancelled_job;
  std::lock_guard<std::mutex> lock(scheduler->mutex);
  if (terminal(task_->state.load(std::memory_order_acquire))) {
    return false;
  }
  const bool changed = !task_->cancellation_requested.exchange(
      true, std::memory_order_acq_rel);
  if (task_->state.load(std::memory_order_acquire) !=
      CompilationTaskState::kQueued) {
    return changed;
  }

  auto& queue = scheduler->queues[priority_lane(task_->priority)];
  const auto queued = std::find(queue.begin(), queue.end(), task_);
  if (queued == queue.end()) {
    return changed;
  }
  cancelled_job = std::move(task_->job);
  queue.erase(queued);
  --scheduler->statistics.queued_tasks;
  scheduler->statistics.queued_bytes -= task_->estimated_bytes;
  erase_task_locked(scheduler.get(), task_);
  saturating_increment(&scheduler->statistics.cancelled);
  complete_task_locked(task_, CompilationTaskState::kCancelled,
                       cancelled_status());
  scheduler->queue_space.notify_all();
  if (scheduler->statistics.queued_tasks == 0 &&
      scheduler->statistics.active_workers == 0) {
    scheduler->idle.notify_all();
  }
  return changed;
}

Status CompilationTicket::result() const {
  if (task_ == nullptr) {
    return {StatusCode::kInvalidArgument,
            "invalid compilation ticket"};
  }
  std::lock_guard<std::mutex> lock(task_->completion_mutex);
  return task_->outcome;
}

Status CompilationTicket::wait() const {
  if (task_ == nullptr) {
    return {StatusCode::kInvalidArgument,
            "invalid compilation ticket"};
  }
  std::unique_lock<std::mutex> lock(task_->completion_mutex);
  task_->completion.wait(lock, [&] { return task_->completed; });
  return task_->outcome;
}

bool CompilationTicket::wait_for(std::chrono::milliseconds timeout) const {
  if (task_ == nullptr || timeout.count() < 0) {
    return false;
  }
  std::unique_lock<std::mutex> lock(task_->completion_mutex);
  return task_->completion.wait_for(lock, timeout,
                                    [&] { return task_->completed; });
}

CompilationScheduler::CompilationScheduler(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

CompilationSchedulerCreation CompilationScheduler::create(
    CompilationSchedulerOptions options) {
  const std::size_t workers = resolved_worker_count(options.worker_count);
  const Status validation = validate_options(options, workers);
  if (!validation.ok()) {
    return {validation, nullptr};
  }
  try {
    options.worker_count = workers;
    auto state = std::make_shared<SharedState>(options, workers);
    auto scheduler = std::unique_ptr<CompilationScheduler>(
        new CompilationScheduler(std::make_unique<Impl>(state)));
    try {
      scheduler->impl_->workers.reserve(workers);
      for (std::size_t index = 0; index < workers; ++index) {
        scheduler->impl_->workers.emplace_back(run_worker, state);
      }
    } catch (const std::system_error&) {
      (void)scheduler->shutdown(SchedulerShutdownMode::kCancel);
      return {{StatusCode::kUnavailable,
               "unable to start compilation scheduler workers"},
              nullptr};
    }
    return {Status::ok_status(), std::move(scheduler)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate compilation scheduler state"},
            nullptr};
  }
}

CompilationScheduler::~CompilationScheduler() {
  (void)shutdown(SchedulerShutdownMode::kCancel);
}

CompilationSubmission CompilationScheduler::try_submit(
    CompilationRequest request) {
  return submit(std::move(request), false, std::chrono::milliseconds(0));
}

CompilationSubmission CompilationScheduler::submit_for(
    CompilationRequest request, std::chrono::milliseconds timeout) {
  return submit(std::move(request), true, timeout);
}

CompilationSubmission CompilationScheduler::submit(
    CompilationRequest request, bool wait,
    std::chrono::milliseconds timeout) {
  if (impl_ == nullptr) {
    return {{StatusCode::kUnavailable,
             "compilation scheduler was moved from"},
            {}, false, false};
  }
  if (wait && timeout.count() < 0) {
    return {{StatusCode::kInvalidArgument,
             "compilation submission timeout cannot be negative"},
            {}, false, false};
  }
  const Status validation = validate_request(request, impl_->state->options);
  if (!validation.ok()) {
    if (validation.code() == StatusCode::kResourceExhausted) {
      std::lock_guard<std::mutex> lock(impl_->state->mutex);
      saturating_increment(&impl_->state->statistics.rejected_capacity);
    }
    return {validation, {}, false, false};
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock<std::mutex> lock(impl_->state->mutex);
  bool recorded_wait = false;
  while (true) {
    std::shared_ptr<Task> existing = find_task_locked(
        *impl_->state, request.identity, request.generation);
    if (existing != nullptr) {
      saturating_increment(&impl_->state->statistics.deduplicated);
      return {Status::ok_status(),
              CompilationTicket(existing, impl_->state), false, true};
    }
    if (!impl_->state->statistics.accepting) {
      saturating_increment(&impl_->state->statistics.rejected_closed);
      return {{StatusCode::kUnavailable,
               "compilation scheduler is closed"},
              {}, false, false};
    }
    if (queue_has_capacity(*impl_->state, request.estimated_bytes)) {
      break;
    }
    if (!wait) {
      saturating_increment(&impl_->state->statistics.rejected_capacity);
      return {{StatusCode::kResourceExhausted,
               "compilation scheduler queue is full"},
              {}, false, false};
    }
    if (!recorded_wait) {
      saturating_increment(&impl_->state->statistics.submission_waits);
      recorded_wait = true;
    }
    if (impl_->state->queue_space.wait_until(lock, deadline) ==
        std::cv_status::timeout) {
      saturating_increment(&impl_->state->statistics.submission_timeouts);
      return {{StatusCode::kDeadlineExceeded,
               "timed out waiting for compilation queue capacity"},
              {}, false, false};
    }
  }

  try {
    const std::uint64_t task_id = impl_->state->next_task_id;
    impl_->state->next_task_id =
        task_id == std::numeric_limits<std::uint64_t>::max() ? 1
                                                             : task_id + 1;
    auto task = std::make_shared<Task>(
        task_id, request.identity, request.generation,
        request.estimated_bytes, request.priority, std::move(request.job));
    auto group = impl_->state->identities.find(request.identity);
    if (group == impl_->state->identities.end()) {
      group = impl_->state->identities
                  .emplace(request.identity,
                           std::unordered_map<std::uint64_t,
                                              std::shared_ptr<Task>>{})
                  .first;
    }
    try {
      group->second.emplace(request.generation, task);
      impl_->state->queues[priority_lane(request.priority)].push_back(task);
    } catch (...) {
      group->second.erase(request.generation);
      if (group->second.empty()) {
        impl_->state->identities.erase(group);
      }
      throw;
    }
    ++impl_->state->statistics.queued_tasks;
    impl_->state->statistics.queued_bytes += request.estimated_bytes;
    impl_->state->statistics.peak_queued_tasks =
        std::max(impl_->state->statistics.peak_queued_tasks,
                 impl_->state->statistics.queued_tasks);
    impl_->state->statistics.peak_queued_bytes =
        std::max(impl_->state->statistics.peak_queued_bytes,
                 impl_->state->statistics.queued_bytes);
    saturating_increment(&impl_->state->statistics.submitted);
    impl_->state->work_available.notify_one();
    return {Status::ok_status(), CompilationTicket(task, impl_->state), true,
            false};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate compilation queue state"},
            {}, false, false};
  }
}

void CompilationScheduler::wait_idle() const {
  if (impl_ == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(impl_->state->mutex);
  impl_->state->idle.wait(lock, [&] {
    return impl_->state->statistics.queued_tasks == 0 &&
           impl_->state->statistics.active_workers == 0;
  });
}

bool CompilationScheduler::wait_idle_for(
    std::chrono::milliseconds timeout) const {
  if (impl_ == nullptr || timeout.count() < 0) {
    return false;
  }
  std::unique_lock<std::mutex> lock(impl_->state->mutex);
  return impl_->state->idle.wait_for(lock, timeout, [&] {
    return impl_->state->statistics.queued_tasks == 0 &&
           impl_->state->statistics.active_workers == 0;
  });
}

Status CompilationScheduler::shutdown(SchedulerShutdownMode mode) {
  if (impl_ == nullptr) {
    return {StatusCode::kUnavailable,
            "compilation scheduler was moved from"};
  }
  if (mode != SchedulerShutdownMode::kDrain &&
      mode != SchedulerShutdownMode::kCancel) {
    return {StatusCode::kInvalidArgument,
            "unknown compilation scheduler shutdown mode"};
  }

  std::lock_guard<std::mutex> shutdown_lock(impl_->shutdown_mutex);
  if (impl_->joined) {
    return Status::ok_status();
  }

  std::array<std::deque<std::shared_ptr<Task>>, 3> cancelled_tasks;
  {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    impl_->state->statistics.accepting = false;
    if (mode == SchedulerShutdownMode::kCancel) {
      for (auto& identity : impl_->state->identities) {
        for (auto& generation : identity.second) {
          generation.second->cancellation_requested.store(
              true, std::memory_order_release);
        }
      }
      for (std::size_t lane = 0; lane < impl_->state->queues.size(); ++lane) {
        cancelled_tasks[lane].swap(impl_->state->queues[lane]);
        for (const std::shared_ptr<Task>& task : cancelled_tasks[lane]) {
          erase_task_locked(impl_->state.get(), task);
          saturating_increment(&impl_->state->statistics.cancelled);
          complete_task_locked(task, CompilationTaskState::kCancelled,
                               cancelled_status());
        }
      }
      impl_->state->statistics.queued_tasks = 0;
      impl_->state->statistics.queued_bytes = 0;
    }
    impl_->state->work_available.notify_all();
    impl_->state->queue_space.notify_all();
    if (impl_->state->statistics.queued_tasks == 0 &&
        impl_->state->statistics.active_workers == 0) {
      impl_->state->idle.notify_all();
    }
  }

  for (auto& queue : cancelled_tasks) {
    for (const std::shared_ptr<Task>& task : queue) {
      task->job = {};
    }
  }

  bool detached_current_worker = false;
  const std::thread::id current = std::this_thread::get_id();
  for (std::thread& worker : impl_->workers) {
    if (!worker.joinable()) {
      continue;
    }
    if (worker.get_id() == current) {
      worker.detach();
      detached_current_worker = true;
    } else {
      worker.join();
    }
  }
  impl_->joined = true;
  return detached_current_worker
             ? Status{StatusCode::kUnavailable,
                      "scheduler shutdown from a worker detached that worker"}
             : Status::ok_status();
}

bool CompilationScheduler::accepting() const noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return impl_->state->statistics.accepting;
}

CompilationSchedulerStats CompilationScheduler::stats() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return impl_->state->statistics;
}

}  // namespace unijit::jit
