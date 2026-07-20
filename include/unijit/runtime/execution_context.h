#ifndef UNIJIT_RUNTIME_EXECUTION_CONTEXT_H
#define UNIJIT_RUNTIME_EXECUTION_CONTEXT_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "unijit/ir/function.h"

namespace unijit::jit {
class CompiledFunction;
}

namespace unijit::runtime {

class Assumption;

enum class ExitReason : std::uint64_t {
  kNone = 0,
  kSafepoint = 1,
  kRuntime = 2,
};

class ExecutionContext final {
 public:
  static constexpr std::size_t kMaximumCapturedValues = 64;

  ExecutionContext() noexcept = default;

  ExecutionContext(const ExecutionContext&) = delete;
  ExecutionContext& operator=(const ExecutionContext&) = delete;

  void request_interrupt() noexcept {
    interrupt_requested_.fetch_or(kInterruptRequested,
                                  std::memory_order_release);
  }

  void clear_interrupt() noexcept {
    interrupt_requested_.fetch_and(~kInterruptRequested,
                                   std::memory_order_release);
  }

  bool interrupt_requested() const noexcept {
    return (interrupt_requested_.load(std::memory_order_acquire) &
            kInterruptRequested) != 0;
  }

  bool exit_poll_requested() const noexcept {
    return interrupt_requested_.load(std::memory_order_acquire) != 0;
  }

  void clear_exit() noexcept {
    exit_reason_ = static_cast<std::uint64_t>(ExitReason::kNone);
    exit_site_ = 0;
    exit_value_ = 0;
    captured_value_count_ = 0;
  }

  void record_exit(ExitReason reason, std::size_t site,
                   ir::Word value = 0) noexcept {
    exit_value_ = value;
    exit_site_ = static_cast<std::uint64_t>(site);
    exit_reason_ = static_cast<std::uint64_t>(reason);
  }

  ExitReason exit_reason() const noexcept {
    return static_cast<ExitReason>(exit_reason_);
  }

  std::size_t exit_site() const noexcept {
    return static_cast<std::size_t>(exit_site_);
  }

  ir::Word exit_value() const noexcept { return exit_value_; }

  std::size_t captured_value_count() const noexcept {
    return static_cast<std::size_t>(captured_value_count_);
  }
  const ir::Word* captured_values() const noexcept {
    return captured_values_.data();
  }

  void* user_data() const noexcept { return user_data_; }
  void set_user_data(void* value) noexcept { user_data_ = value; }

  static constexpr std::size_t interrupt_requested_offset() noexcept;
  static constexpr std::size_t exit_reason_offset() noexcept;
  static constexpr std::size_t exit_site_offset() noexcept;
  static constexpr std::size_t exit_value_offset() noexcept;
  static constexpr std::size_t captured_value_count_offset() noexcept;
  static constexpr std::size_t captured_values_offset() noexcept;

 private:
  friend class Assumption;
  friend class jit::CompiledFunction;

  static constexpr std::uint64_t kInterruptRequested = 1;
  static constexpr std::uint64_t kDeoptimizationWakeup = 2;

  void request_deoptimization_wakeup() noexcept {
    interrupt_requested_.fetch_or(kDeoptimizationWakeup,
                                  std::memory_order_release);
  }

  void clear_deoptimization_wakeup() noexcept {
    interrupt_requested_.fetch_and(~kDeoptimizationWakeup,
                                   std::memory_order_release);
  }

  alignas(std::uint64_t) std::atomic<std::uint64_t> interrupt_requested_{0};
  std::uint64_t exit_reason_{static_cast<std::uint64_t>(ExitReason::kNone)};
  std::uint64_t exit_site_{0};
  ir::Word exit_value_{0};
  std::uint64_t captured_value_count_{0};
  std::array<ir::Word, kMaximumCapturedValues> captured_values_;
  void* user_data_{nullptr};
};

constexpr std::size_t ExecutionContext::interrupt_requested_offset() noexcept {
  return offsetof(ExecutionContext, interrupt_requested_);
}

constexpr std::size_t ExecutionContext::exit_reason_offset() noexcept {
  return offsetof(ExecutionContext, exit_reason_);
}

constexpr std::size_t ExecutionContext::exit_site_offset() noexcept {
  return offsetof(ExecutionContext, exit_site_);
}

constexpr std::size_t ExecutionContext::exit_value_offset() noexcept {
  return offsetof(ExecutionContext, exit_value_);
}

constexpr std::size_t
ExecutionContext::captured_value_count_offset() noexcept {
  return offsetof(ExecutionContext, captured_value_count_);
}

constexpr std::size_t ExecutionContext::captured_values_offset() noexcept {
  return offsetof(ExecutionContext, captured_values_);
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "native safepoint polling requires lock-free 64-bit atomics");
static_assert(std::is_standard_layout<ExecutionContext>::value,
              "execution-context offsets are part of the native ABI");

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_EXECUTION_CONTEXT_H
