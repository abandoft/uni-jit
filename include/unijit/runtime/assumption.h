#ifndef UNIJIT_RUNTIME_ASSUMPTION_H
#define UNIJIT_RUNTIME_ASSUMPTION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "unijit/runtime/execution_context.h"
#include "unijit/status.h"

namespace unijit::runtime {

class AssumptionSet;

class Assumption final {
 public:
  Assumption() noexcept = default;

  Assumption(const Assumption&) = delete;
  Assumption& operator=(const Assumption&) = delete;

  bool valid() const noexcept {
    return valid_.load(std::memory_order_acquire) != 0;
  }

  bool invalidate();
  std::size_t active_invocations() const;

 private:
  friend class AssumptionActivation;

  enum class SubscriptionResult {
    kSubscribed,
    kInvalid,
    kResourceExhausted,
  };

  SubscriptionResult subscribe(ExecutionContext* context) const;
  void unsubscribe(ExecutionContext* context) const noexcept;

  std::atomic<std::uint64_t> valid_{1};
  mutable std::mutex mutex_;
  mutable std::condition_variable quiesced_;
  mutable std::vector<ExecutionContext*> active_contexts_;
};

struct AssumptionDependency final {
  std::shared_ptr<const Assumption> assumption;
  std::size_t site{0};
  std::size_t resume_offset{0};
};

class AssumptionActivation final {
 public:
  ~AssumptionActivation();

  AssumptionActivation(const AssumptionActivation&) = delete;
  AssumptionActivation& operator=(const AssumptionActivation&) = delete;
  AssumptionActivation(AssumptionActivation&&) = delete;
  AssumptionActivation& operator=(AssumptionActivation&&) = delete;

  const Status& status() const noexcept { return status_; }
  bool ok() const noexcept {
    return status_.ok() && invalid_dependency_ == nullptr;
  }
  const AssumptionDependency* invalid_dependency() const noexcept {
    return invalid_dependency_;
  }

 private:
  friend class AssumptionSet;

  AssumptionActivation(const AssumptionSet* assumptions,
                       ExecutionContext* context);

  const AssumptionSet* assumptions_{nullptr};
  ExecutionContext* context_{nullptr};
  std::size_t subscription_count_{0};
  const AssumptionDependency* invalid_dependency_{nullptr};
  Status status_;
};

class AssumptionSet final {
 public:
  Status add(std::shared_ptr<const Assumption> assumption, std::size_t site,
             std::size_t resume_offset);

  bool empty() const noexcept { return dependencies_.empty(); }
  std::size_t size() const noexcept { return dependencies_.size(); }
  const std::vector<AssumptionDependency>& dependencies() const noexcept {
    return dependencies_;
  }
  const AssumptionDependency* find(std::size_t site) const noexcept;
  const AssumptionDependency* first_invalid() const noexcept;

  AssumptionActivation activate(ExecutionContext* context) const {
    return AssumptionActivation(this, context);
  }

 private:
  friend class AssumptionActivation;
  std::vector<AssumptionDependency> dependencies_;
};

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_ASSUMPTION_H
