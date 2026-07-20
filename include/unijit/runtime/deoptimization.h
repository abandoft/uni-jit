#ifndef UNIJIT_RUNTIME_DEOPTIMIZATION_H
#define UNIJIT_RUNTIME_DEOPTIMIZATION_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/runtime/execution_context.h"
#include "unijit/status.h"

namespace unijit::runtime {

enum class DeoptimizationReason : std::uint8_t {
  kGuardFailed = 0,
  kDivisionByZero,
  kTypeMismatch,
  kAssumptionInvalidated,
};

enum class RecoverySource : std::uint8_t {
  kArgument = 0,
  kConstant,
  kExitValue,
  kCapturedValue,
};

struct RecoveryOperation final {
  static constexpr std::size_t kUnresolvedCaptureIndex =
      std::numeric_limits<std::size_t>::max();

  std::size_t slot{0};
  ir::ValueType type{ir::ValueType::kWord};
  RecoverySource source{RecoverySource::kConstant};
  std::size_t argument_index{0};
  ir::Word constant{0};
  ir::Value source_value;
  std::size_t capture_index{kUnresolvedCaptureIndex};

  static RecoveryOperation argument(std::size_t slot, ir::ValueType type,
                                    std::size_t argument_index) noexcept;
  static RecoveryOperation constant_value(std::size_t slot,
                                          ir::ValueType type,
                                          ir::Word value) noexcept;
  static RecoveryOperation exit_value(std::size_t slot,
                                      ir::ValueType type) noexcept;
  static RecoveryOperation captured_value(std::size_t slot,
                                          ir::ValueType type,
                                          ir::Value value) noexcept;

  bool capture_resolved() const noexcept {
    return source == RecoverySource::kCapturedValue &&
           capture_index != kUnresolvedCaptureIndex;
  }
};

struct DeoptimizationRecord final {
  std::size_t site{0};
  std::size_t resume_offset{0};
  DeoptimizationReason reason{DeoptimizationReason::kGuardFailed};
  std::vector<RecoveryOperation> recovery;
};

struct RecoveredValue final {
  std::size_t slot{0};
  ir::ValueType type{ir::ValueType::kWord};
  ir::Word value{0};
};

struct ReconstructedFrame final {
  DeoptimizationReason reason{DeoptimizationReason::kGuardFailed};
  std::size_t site{0};
  std::size_t resume_offset{0};
  std::vector<RecoveredValue> values;

  const RecoveredValue* find(std::size_t slot) const noexcept;
};

struct ReconstructionResult final {
  Status status;
  ReconstructedFrame frame;

  bool ok() const noexcept { return status.ok(); }
};

class DeoptimizationTable final {
 public:
  Status add(const DeoptimizationRecord& record);
  Status validate(std::size_t parameter_count) const;

  const DeoptimizationRecord* find(std::size_t site) const noexcept;
  ReconstructionResult reconstruct(
      std::size_t site, const ir::Word* arguments, std::size_t argument_count,
      const ExecutionContext& context) const;

  bool empty() const noexcept { return records_.empty(); }
  std::size_t size() const noexcept { return records_.size(); }
  const std::vector<DeoptimizationRecord>& records() const noexcept {
    return records_;
  }

 private:
  std::vector<DeoptimizationRecord> records_;
};

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_DEOPTIMIZATION_H
