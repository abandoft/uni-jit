#include "unijit/runtime/assumption.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include "unijit/ir/function.h"

namespace unijit::runtime {

Assumption::SubscriptionResult Assumption::subscribe(
    ExecutionContext* context) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!valid()) {
    return SubscriptionResult::kInvalid;
  }
  try {
    active_contexts_.push_back(context);
    return SubscriptionResult::kSubscribed;
  } catch (const std::bad_alloc&) {
    return SubscriptionResult::kResourceExhausted;
  }
}

void Assumption::unsubscribe(ExecutionContext* context) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto active =
      std::find(active_contexts_.begin(), active_contexts_.end(), context);
  if (active != active_contexts_.end()) {
    active_contexts_.erase(active);
  }
  if (active_contexts_.empty()) {
    quiesced_.notify_all();
  }
}

bool Assumption::invalidate() {
  std::unique_lock<std::mutex> lock(mutex_);
  const bool changed = valid_.exchange(0, std::memory_order_acq_rel) != 0;
  for (ExecutionContext* context : active_contexts_) {
    context->request_deoptimization_wakeup();
  }
  quiesced_.wait(lock, [this] { return active_contexts_.empty(); });
  return changed;
}

std::size_t Assumption::active_invocations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_contexts_.size();
}

AssumptionActivation::AssumptionActivation(
    const AssumptionSet* assumptions, ExecutionContext* context)
    : assumptions_(assumptions), context_(context) {
  if (assumptions_ == nullptr || context_ == nullptr) {
    status_ = {StatusCode::kInvalidArgument,
               "assumption activation requires an execution context"};
    return;
  }
  for (const AssumptionDependency& dependency : assumptions_->dependencies_) {
    const Assumption::SubscriptionResult subscription =
        dependency.assumption->subscribe(context_);
    if (subscription == Assumption::SubscriptionResult::kInvalid) {
      invalid_dependency_ = &dependency;
      return;
    }
    if (subscription == Assumption::SubscriptionResult::kResourceExhausted) {
      status_ = {StatusCode::kResourceExhausted,
                 "unable to register an active assumption dependency",
                 dependency.site};
      return;
    }
    ++subscription_count_;
  }
}

AssumptionActivation::~AssumptionActivation() {
  if (assumptions_ == nullptr || context_ == nullptr) {
    return;
  }
  while (subscription_count_ != 0) {
    --subscription_count_;
    assumptions_->dependencies_[subscription_count_]
        .assumption->unsubscribe(context_);
  }
}

Status AssumptionSet::add(std::shared_ptr<const Assumption> assumption,
                          std::size_t site,
                          std::size_t resume_offset) {
  if (assumption == nullptr) {
    return {StatusCode::kInvalidArgument,
            "assumption dependency cannot contain a null token", site};
  }
  if (site > static_cast<std::size_t>(std::numeric_limits<ir::Word>::max())) {
    return {StatusCode::kInvalidArgument,
            "assumption dependency site exceeds the runtime ABI", site};
  }
  const auto duplicate = std::find_if(
      dependencies_.begin(), dependencies_.end(),
      [site, pointer = assumption.get()](const auto& dependency) {
        return dependency.site == site || dependency.assumption.get() == pointer;
      });
  if (duplicate != dependencies_.end()) {
    return {StatusCode::kInvalidArgument,
            "assumption dependency duplicates a site or token", site};
  }
  try {
    dependencies_.push_back(
        {std::move(assumption), site, resume_offset});
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate assumption dependency metadata", site};
  }
}

const AssumptionDependency* AssumptionSet::find(
    std::size_t site) const noexcept {
  const auto dependency = std::find_if(
      dependencies_.begin(), dependencies_.end(),
      [site](const auto& candidate) { return candidate.site == site; });
  return dependency == dependencies_.end() ? nullptr : &*dependency;
}

const AssumptionDependency* AssumptionSet::first_invalid() const noexcept {
  const auto dependency = std::find_if(
      dependencies_.begin(), dependencies_.end(), [](const auto& candidate) {
        return !candidate.assumption->valid();
      });
  return dependency == dependencies_.end() ? nullptr : &*dependency;
}

}  // namespace unijit::runtime
