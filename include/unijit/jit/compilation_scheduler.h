#ifndef UNIJIT_JIT_COMPILATION_SCHEDULER_H
#define UNIJIT_JIT_COMPILATION_SCHEDULER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "unijit/status.h"

namespace unijit::jit {

namespace detail {
struct CompilationSchedulerState;
struct CompilationSchedulerAccess;
struct CompilationTask;
}  // namespace detail

enum class CompilationPriority : std::uint8_t {
  kBackground = 0,
  kNormal,
  kUrgent,
};

enum class CompilationTaskState : std::uint8_t {
  kInvalid = 0,
  kQueued,
  kRunning,
  kSucceeded,
  kFailed,
  kCancelled,
};

enum class SchedulerShutdownMode : std::uint8_t {
  kDrain = 0,
  kCancel,
};

class CompilationCancellation final {
 public:
  CompilationCancellation() noexcept = default;

  bool stop_requested() const noexcept;
  explicit operator bool() const noexcept { return task_ != nullptr; }

 private:
  friend class CompilationScheduler;
  friend struct detail::CompilationSchedulerAccess;
  explicit CompilationCancellation(
      std::shared_ptr<detail::CompilationTask> task) noexcept;

  std::shared_ptr<detail::CompilationTask> task_;
};

using CompilationJob =
    std::function<Status(const CompilationCancellation& cancellation)>;

class CompilationTicket final {
 public:
  CompilationTicket() noexcept = default;

  bool valid() const noexcept { return task_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }
  std::uint64_t id() const noexcept;
  CompilationTaskState state() const noexcept;
  bool cancellation_requested() const noexcept;

  bool cancel() const;
  Status result() const;
  Status wait() const;
  bool wait_for(std::chrono::milliseconds timeout) const;

 private:
  friend class CompilationScheduler;
  CompilationTicket(
      std::shared_ptr<detail::CompilationTask> task,
      std::weak_ptr<detail::CompilationSchedulerState> scheduler) noexcept;

  std::shared_ptr<detail::CompilationTask> task_;
  std::weak_ptr<detail::CompilationSchedulerState> scheduler_;
};

struct CompilationRequest final {
  std::string identity;
  std::uint64_t generation{0};
  std::size_t estimated_bytes{1};
  CompilationPriority priority{CompilationPriority::kNormal};
  CompilationJob job;
};

struct CompilationSubmission final {
  Status status;
  CompilationTicket ticket;
  bool enqueued{false};
  bool deduplicated{false};

  bool ok() const noexcept { return status.ok() && ticket.valid(); }
};

struct CompilationSchedulerOptions final {
  std::size_t worker_count{0};
  std::size_t maximum_queued_tasks{64};
  std::size_t maximum_queued_bytes{64U * 1024U * 1024U};
};

struct CompilationSchedulerStats final {
  std::size_t worker_count{0};
  std::size_t queued_tasks{0};
  std::size_t queued_bytes{0};
  std::size_t active_workers{0};
  std::size_t peak_queued_tasks{0};
  std::size_t peak_queued_bytes{0};
  std::size_t peak_active_workers{0};
  std::uint64_t submitted{0};
  std::uint64_t deduplicated{0};
  std::uint64_t started{0};
  std::uint64_t succeeded{0};
  std::uint64_t failed{0};
  std::uint64_t cancelled{0};
  std::uint64_t rejected_capacity{0};
  std::uint64_t rejected_closed{0};
  std::uint64_t submission_waits{0};
  std::uint64_t submission_timeouts{0};
  bool accepting{false};
};

struct CompilationSchedulerCreation;

class CompilationScheduler final {
 public:
  static CompilationSchedulerCreation create(
      CompilationSchedulerOptions options = {});

  ~CompilationScheduler();

  CompilationScheduler(const CompilationScheduler&) = delete;
  CompilationScheduler& operator=(const CompilationScheduler&) = delete;
  CompilationScheduler(CompilationScheduler&&) = delete;
  CompilationScheduler& operator=(CompilationScheduler&&) = delete;

  CompilationSubmission try_submit(CompilationRequest request);
  CompilationSubmission submit_for(CompilationRequest request,
                                   std::chrono::milliseconds timeout);

  void wait_idle() const;
  bool wait_idle_for(std::chrono::milliseconds timeout) const;
  Status shutdown(SchedulerShutdownMode mode = SchedulerShutdownMode::kDrain);

  bool accepting() const noexcept;
  CompilationSchedulerStats stats() const noexcept;

 private:
  struct Impl;
  explicit CompilationScheduler(std::unique_ptr<Impl> impl) noexcept;

  CompilationSubmission submit(CompilationRequest request, bool wait,
                               std::chrono::milliseconds timeout);

  std::unique_ptr<Impl> impl_;
};

struct CompilationSchedulerCreation final {
  Status status;
  std::unique_ptr<CompilationScheduler> scheduler;

  bool ok() const noexcept { return status.ok() && scheduler != nullptr; }
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_COMPILATION_SCHEDULER_H
