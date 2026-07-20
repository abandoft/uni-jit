#ifndef UNIJIT_JIT_TIERING_H
#define UNIJIT_JIT_TIERING_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "unijit/jit/code_cache.h"
#include "unijit/status.h"

namespace unijit::jit {

struct TieringThresholds final {
  std::uint64_t invocations{1000};
  std::uint64_t backedges{10000};
  std::uint64_t retry_delay{1000};
};

struct HotnessStats final {
  std::uint64_t invocations{0};
  std::uint64_t backedges{0};
  std::uint64_t compilation_attempts{0};
  std::uint64_t successful_compilations{0};
  std::uint64_t failed_compilations{0};
  bool compilation_claimed{false};
  bool optimized_active{false};
  bool hot{false};
};

class HotnessProfile final {
 public:
  explicit HotnessProfile(TieringThresholds thresholds = {});
  ~HotnessProfile();

  HotnessProfile(HotnessProfile&&) noexcept;
  HotnessProfile& operator=(HotnessProfile&&) noexcept;

  HotnessProfile(const HotnessProfile&) = delete;
  HotnessProfile& operator=(const HotnessProfile&) = delete;

  void record_invocation() noexcept;
  void record_backedges(std::uint64_t count = 1) noexcept;
  bool hot() const noexcept;
  bool try_begin_optimization() noexcept;
  Status report_optimization_failure();
  HotnessStats stats() const noexcept;

 private:
  friend class TieredCode;
  struct Impl;

  void reset_for_baseline() noexcept;
  void mark_optimized() noexcept;
  void mark_deoptimized() noexcept;

  std::unique_ptr<Impl> impl_;
};

enum class CodeTier : std::uint8_t {
  kNone = 0,
  kBaseline,
  kOptimized,
};

enum class DeoptimizationPolicy : std::uint8_t {
  kReturnExit = 0,
  kRetryBaseline,
};

struct TieredCodeSnapshot final {
  CodeHandle handle;
  CodeTier tier{CodeTier::kNone};
  std::uint64_t generation{0};

  bool valid() const noexcept { return handle.valid(); }
};

struct TieredInvocationResult final {
  ir::EvaluationResult result;
  CodeHandle attempted_handle;
  CodeTier attempted_tier{CodeTier::kNone};
  std::uint64_t generation{0};
  bool deoptimized{false};
  bool retried_baseline{false};

  bool ok() const noexcept { return result.ok(); }
};

struct TieredOsrEntryResult final {
  runtime::OsrEntryResult entry;
  CodeHandle attempted_handle;
  CodeTier attempted_tier{CodeTier::kNone};
  std::uint64_t generation{0};
  bool deoptimized{false};

  bool entered() const noexcept { return entry.entered(); }
  bool ok() const noexcept { return entry.ok(); }
};

struct TieredCodeStats final {
  HotnessStats hotness;
  CodeTier active_tier{CodeTier::kNone};
  std::uint64_t generation{0};
  std::uint64_t promotions{0};
  std::uint64_t withdrawals{0};
  std::uint64_t assumption_deoptimizations{0};
  std::uint64_t baseline_retries{0};
  std::uint64_t osr_attempts{0};
  std::uint64_t osr_entries{0};
  std::uint64_t osr_exits{0};
};

class TieredCode final {
 public:
  explicit TieredCode(TieringThresholds thresholds = {});
  ~TieredCode();

  TieredCode(TieredCode&&) noexcept;
  TieredCode& operator=(TieredCode&&) noexcept;

  TieredCode(const TieredCode&) = delete;
  TieredCode& operator=(const TieredCode&) = delete;

  Status publish_baseline(CodeHandle baseline);
  Status publish_optimized(CodeHandle optimized,
                           std::uint64_t expected_generation = 0);
  bool withdraw_optimized(
      std::uint64_t expected_generation = 0) const;

  TieredCodeSnapshot snapshot() const noexcept;
  TieredInvocationResult invoke(
      const ir::Word* args, std::size_t arg_count,
      runtime::ExecutionContext* context = nullptr,
      DeoptimizationPolicy policy =
          DeoptimizationPolicy::kReturnExit) const;
  TieredOsrEntryResult enter_osr(
      const runtime::OsrFrame& frame, const runtime::OsrEntryPlan& plan,
      runtime::ExecutionContext* context = nullptr) const;

  void record_backedges(std::uint64_t count = 1) noexcept;
  bool try_begin_optimization() noexcept;
  Status report_optimization_failure();
  TieredCodeStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_TIERING_H
