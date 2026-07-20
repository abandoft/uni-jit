#include "unijit/jit/tiering.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace unijit::jit {
namespace {

void saturating_add(std::atomic<std::uint64_t>* target,
                    std::uint64_t increment) noexcept {
  std::uint64_t current = target->load(std::memory_order_relaxed);
  while (true) {
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t next =
        increment > maximum - current ? maximum : current + increment;
    if (target->compare_exchange_weak(current, next,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return;
    }
  }
}

std::uint64_t delayed_threshold(std::uint64_t current,
                                std::uint64_t delay) noexcept {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  return delay > maximum - current ? maximum : current + delay;
}

std::uint64_t next_generation(std::uint64_t current) noexcept {
  return current == std::numeric_limits<std::uint64_t>::max() ? 1
                                                              : current + 1;
}

ir::EvaluationResult invalid_tiered_code() {
  return {{StatusCode::kInvalidArgument,
           "tiered code has no published baseline"},
          0};
}

bool is_assumption_exit(const CodeHandle& handle,
                        const ir::EvaluationResult& result) noexcept {
  if (result.status.code() != StatusCode::kRuntimeExit) {
    return false;
  }
  const runtime::DeoptimizationRecord* record =
      handle.deoptimization_record(result.status.location());
  return record != nullptr &&
         record->reason ==
             runtime::DeoptimizationReason::kAssumptionInvalidated;
}

}  // namespace

struct HotnessProfile::Impl final {
  explicit Impl(TieringThresholds configured) noexcept
      : thresholds(configured),
        next_invocation(configured.invocations),
        next_backedge(configured.backedges) {}

  bool hot() const noexcept {
    if (optimized_active.load(std::memory_order_acquire)) {
      return false;
    }
    return invocations.load(std::memory_order_relaxed) >=
               next_invocation.load(std::memory_order_relaxed) ||
           backedges.load(std::memory_order_relaxed) >=
               next_backedge.load(std::memory_order_relaxed);
  }

  void rearm() noexcept {
    next_invocation.store(
        delayed_threshold(invocations.load(std::memory_order_relaxed),
                          thresholds.retry_delay),
        std::memory_order_relaxed);
    next_backedge.store(
        delayed_threshold(backedges.load(std::memory_order_relaxed),
                          thresholds.retry_delay),
        std::memory_order_relaxed);
  }

  TieringThresholds thresholds;
  std::atomic<std::uint64_t> invocations{0};
  std::atomic<std::uint64_t> backedges{0};
  std::atomic<std::uint64_t> next_invocation{0};
  std::atomic<std::uint64_t> next_backedge{0};
  std::atomic<std::uint64_t> compilation_attempts{0};
  std::atomic<std::uint64_t> successful_compilations{0};
  std::atomic<std::uint64_t> failed_compilations{0};
  std::atomic<bool> compilation_claimed{false};
  std::atomic<bool> optimized_active{false};
};

HotnessProfile::HotnessProfile(TieringThresholds thresholds)
    : impl_(std::make_unique<Impl>(thresholds)) {}

HotnessProfile::~HotnessProfile() = default;
HotnessProfile::HotnessProfile(HotnessProfile&&) noexcept = default;
HotnessProfile& HotnessProfile::operator=(HotnessProfile&&) noexcept =
    default;

void HotnessProfile::record_invocation() noexcept {
  if (impl_ != nullptr) {
    saturating_add(&impl_->invocations, 1);
  }
}

void HotnessProfile::record_backedges(std::uint64_t count) noexcept {
  if (impl_ != nullptr) {
    saturating_add(&impl_->backedges, count);
  }
}

bool HotnessProfile::hot() const noexcept {
  return impl_ != nullptr && impl_->hot();
}

bool HotnessProfile::try_begin_optimization() noexcept {
  if (impl_ == nullptr || !impl_->hot()) {
    return false;
  }
  bool expected = false;
  if (!impl_->compilation_claimed.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return false;
  }
  saturating_add(&impl_->compilation_attempts, 1);
  return true;
}

Status HotnessProfile::report_optimization_failure() {
  if (impl_ == nullptr) {
    return {StatusCode::kInvalidArgument, "hotness profile was moved from"};
  }
  bool expected = true;
  if (!impl_->compilation_claimed.compare_exchange_strong(
          expected, false, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return {StatusCode::kInvalidArgument,
            "no optimization compilation is currently claimed"};
  }
  saturating_add(&impl_->failed_compilations, 1);
  impl_->rearm();
  return Status::ok_status();
}

HotnessStats HotnessProfile::stats() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  return {impl_->invocations.load(std::memory_order_relaxed),
          impl_->backedges.load(std::memory_order_relaxed),
          impl_->compilation_attempts.load(std::memory_order_relaxed),
          impl_->successful_compilations.load(std::memory_order_relaxed),
          impl_->failed_compilations.load(std::memory_order_relaxed),
          impl_->compilation_claimed.load(std::memory_order_acquire),
          impl_->optimized_active.load(std::memory_order_acquire),
          impl_->hot()};
}

void HotnessProfile::reset_for_baseline() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->invocations.store(0, std::memory_order_relaxed);
  impl_->backedges.store(0, std::memory_order_relaxed);
  impl_->next_invocation.store(impl_->thresholds.invocations,
                               std::memory_order_relaxed);
  impl_->next_backedge.store(impl_->thresholds.backedges,
                             std::memory_order_relaxed);
  impl_->compilation_claimed.store(false, std::memory_order_release);
  impl_->optimized_active.store(false, std::memory_order_release);
}

void HotnessProfile::mark_optimized() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->compilation_claimed.exchange(false,
                                          std::memory_order_acq_rel)) {
    saturating_add(&impl_->successful_compilations, 1);
  }
  impl_->optimized_active.store(true, std::memory_order_release);
}

void HotnessProfile::mark_deoptimized() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->optimized_active.store(false, std::memory_order_release);
  impl_->compilation_claimed.store(false, std::memory_order_release);
  impl_->rearm();
}

struct TieredCode::Impl final {
  struct State final {
    CodeHandle baseline;
    CodeHandle active;
    CodeTier tier{CodeTier::kNone};
    std::uint64_t generation{0};
    std::shared_ptr<const State> fallback;
  };

  explicit Impl(TieringThresholds thresholds) : hotness(thresholds) {}

  mutable std::mutex publication_mutex;
  std::shared_ptr<const State> state;
  HotnessProfile hotness;
  std::atomic<std::uint64_t> promotions{0};
  std::atomic<std::uint64_t> withdrawals{0};
  std::atomic<std::uint64_t> assumption_deoptimizations{0};
  std::atomic<std::uint64_t> baseline_retries{0};
};

TieredCode::TieredCode(TieringThresholds thresholds)
    : impl_(std::make_unique<Impl>(thresholds)) {}

TieredCode::~TieredCode() = default;
TieredCode::TieredCode(TieredCode&&) noexcept = default;
TieredCode& TieredCode::operator=(TieredCode&&) noexcept = default;

Status TieredCode::publish_baseline(CodeHandle baseline) {
  if (impl_ == nullptr) {
    return {StatusCode::kInvalidArgument, "tiered code was moved from"};
  }
  if (!baseline.valid()) {
    return {StatusCode::kInvalidArgument,
            "tiered baseline must contain a valid code handle"};
  }
  if (baseline.assumption_count() != 0) {
    return {StatusCode::kInvalidArgument,
            "tiered baseline cannot depend on speculative assumptions"};
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->publication_mutex);
    const std::shared_ptr<const Impl::State> previous =
        std::atomic_load_explicit(&impl_->state, std::memory_order_acquire);
    const std::uint64_t generation =
        next_generation(previous == nullptr ? 0 : previous->generation);
    auto state = std::make_shared<Impl::State>();
    state->baseline = baseline;
    state->active = std::move(baseline);
    state->tier = CodeTier::kBaseline;
    state->generation = generation;
    std::atomic_store_explicit(
        &impl_->state, std::shared_ptr<const Impl::State>(std::move(state)),
        std::memory_order_release);
    impl_->hotness.reset_for_baseline();
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to publish tiered baseline state"};
  }
}

Status TieredCode::publish_optimized(CodeHandle optimized,
                                     std::uint64_t expected_generation) {
  if (impl_ == nullptr) {
    return {StatusCode::kInvalidArgument, "tiered code was moved from"};
  }
  if (!optimized.valid()) {
    return {StatusCode::kInvalidArgument,
            "optimized tier must contain a valid code handle"};
  }
  if (!optimized.assumptions_valid()) {
    return {StatusCode::kInvalidArgument,
            "optimized tier contains an invalidated assumption"};
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->publication_mutex);
    const std::shared_ptr<const Impl::State> previous =
        std::atomic_load_explicit(&impl_->state, std::memory_order_acquire);
    if (previous == nullptr || !previous->baseline.valid()) {
      return {StatusCode::kInvalidArgument,
              "an optimized tier requires a published baseline"};
    }
    if (expected_generation != 0 &&
        previous->generation != expected_generation) {
      return {StatusCode::kInvalidArgument,
              "tiered generation changed during optimized compilation"};
    }
    if (optimized.parameter_count() != previous->baseline.parameter_count()) {
      return {StatusCode::kInvalidArgument,
              "optimized and baseline signatures do not match"};
    }

    const std::uint64_t optimized_generation =
        next_generation(previous->generation);
    auto fallback = std::make_shared<Impl::State>();
    fallback->baseline = previous->baseline;
    fallback->active = previous->baseline;
    fallback->tier = CodeTier::kBaseline;
    fallback->generation = next_generation(optimized_generation);

    auto state = std::make_shared<Impl::State>();
    state->baseline = previous->baseline;
    state->active = std::move(optimized);
    state->tier = CodeTier::kOptimized;
    state->generation = optimized_generation;
    state->fallback = std::move(fallback);
    std::atomic_store_explicit(
        &impl_->state, std::shared_ptr<const Impl::State>(std::move(state)),
        std::memory_order_release);
    saturating_add(&impl_->promotions, 1);
    impl_->hotness.mark_optimized();
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to publish optimized tier state"};
  }
}

bool TieredCode::withdraw_optimized(
    std::uint64_t expected_generation) const {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->publication_mutex);
  const std::shared_ptr<const Impl::State> previous =
      std::atomic_load_explicit(&impl_->state, std::memory_order_acquire);
  if (previous == nullptr || previous->tier != CodeTier::kOptimized ||
      previous->fallback == nullptr ||
      (expected_generation != 0 &&
       previous->generation != expected_generation)) {
    return false;
  }
  std::atomic_store_explicit(&impl_->state, previous->fallback,
                             std::memory_order_release);
  saturating_add(&impl_->withdrawals, 1);
  impl_->hotness.mark_deoptimized();
  return true;
}

TieredCodeSnapshot TieredCode::snapshot() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  const std::shared_ptr<const Impl::State> state =
      std::atomic_load_explicit(&impl_->state, std::memory_order_acquire);
  return state == nullptr
             ? TieredCodeSnapshot{}
             : TieredCodeSnapshot{state->active, state->tier,
                                  state->generation};
}

TieredInvocationResult TieredCode::invoke(
    const ir::Word* args, std::size_t arg_count,
    runtime::ExecutionContext* context, DeoptimizationPolicy policy) const {
  if (impl_ == nullptr) {
    return {invalid_tiered_code(), {}, CodeTier::kNone, 0, false, false};
  }
  impl_->hotness.record_invocation();
  const std::shared_ptr<const Impl::State> state =
      std::atomic_load_explicit(&impl_->state, std::memory_order_acquire);
  if (state == nullptr || !state->active.valid()) {
    return {invalid_tiered_code(), {}, CodeTier::kNone, 0, false, false};
  }

  TieredInvocationResult invocation;
  invocation.attempted_handle = state->active;
  invocation.attempted_tier = state->tier;
  invocation.generation = state->generation;
  invocation.result = state->active.invoke(args, arg_count, context);
  if (state->tier != CodeTier::kOptimized ||
      !is_assumption_exit(state->active, invocation.result)) {
    return invocation;
  }

  invocation.deoptimized = true;
  saturating_add(&impl_->assumption_deoptimizations, 1);
  (void)withdraw_optimized(state->generation);
  if (policy == DeoptimizationPolicy::kRetryBaseline) {
    invocation.result = state->baseline.invoke(args, arg_count, context);
    invocation.retried_baseline = true;
    saturating_add(&impl_->baseline_retries, 1);
  }
  return invocation;
}

void TieredCode::record_backedges(std::uint64_t count) noexcept {
  if (impl_ != nullptr) {
    impl_->hotness.record_backedges(count);
  }
}

bool TieredCode::try_begin_optimization() noexcept {
  return impl_ != nullptr && impl_->hotness.try_begin_optimization();
}

Status TieredCode::report_optimization_failure() {
  return impl_ == nullptr
             ? Status{StatusCode::kInvalidArgument,
                      "tiered code was moved from"}
             : impl_->hotness.report_optimization_failure();
}

TieredCodeStats TieredCode::stats() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  const TieredCodeSnapshot current = snapshot();
  return {impl_->hotness.stats(),
          current.tier,
          current.generation,
          impl_->promotions.load(std::memory_order_relaxed),
          impl_->withdrawals.load(std::memory_order_relaxed),
          impl_->assumption_deoptimizations.load(std::memory_order_relaxed),
          impl_->baseline_retries.load(std::memory_order_relaxed)};
}

}  // namespace unijit::jit
