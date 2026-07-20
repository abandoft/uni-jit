#include "unijit/runtime/deoptimization.h"

#include <algorithm>
#include <new>
#include <utility>

namespace unijit::runtime {
namespace {

bool valid_value_type(ir::ValueType type) noexcept {
  return type == ir::ValueType::kWord || type == ir::ValueType::kFloat64;
}

bool valid_recovery_source(RecoverySource source) noexcept {
  return source == RecoverySource::kArgument ||
         source == RecoverySource::kConstant ||
         source == RecoverySource::kExitValue ||
         source == RecoverySource::kCapturedValue;
}

bool valid_deoptimization_reason(DeoptimizationReason reason) noexcept {
  return reason == DeoptimizationReason::kGuardFailed ||
         reason == DeoptimizationReason::kDivisionByZero ||
         reason == DeoptimizationReason::kTypeMismatch ||
         reason == DeoptimizationReason::kAssumptionInvalidated;
}

}  // namespace

RecoveryOperation RecoveryOperation::argument(
    std::size_t slot, ir::ValueType type,
    std::size_t argument_index) noexcept {
  RecoveryOperation operation;
  operation.slot = slot;
  operation.type = type;
  operation.source = RecoverySource::kArgument;
  operation.argument_index = argument_index;
  return operation;
}

RecoveryOperation RecoveryOperation::constant_value(
    std::size_t slot, ir::ValueType type, ir::Word value) noexcept {
  RecoveryOperation operation;
  operation.slot = slot;
  operation.type = type;
  operation.source = RecoverySource::kConstant;
  operation.constant = value;
  return operation;
}

RecoveryOperation RecoveryOperation::exit_value(
    std::size_t slot, ir::ValueType type) noexcept {
  RecoveryOperation operation;
  operation.slot = slot;
  operation.type = type;
  operation.source = RecoverySource::kExitValue;
  return operation;
}

RecoveryOperation RecoveryOperation::captured_value(
    std::size_t slot, ir::ValueType type, ir::Value value) noexcept {
  RecoveryOperation operation;
  operation.slot = slot;
  operation.type = type;
  operation.source = RecoverySource::kCapturedValue;
  operation.source_value = value;
  return operation;
}

const RecoveredValue* ReconstructedFrame::find(
    std::size_t slot) const noexcept {
  const auto value =
      std::find_if(values.begin(), values.end(), [slot](const auto& candidate) {
        return candidate.slot == slot;
      });
  return value == values.end() ? nullptr : &*value;
}

Status DeoptimizationTable::add(const DeoptimizationRecord& record) {
  if (!valid_deoptimization_reason(record.reason)) {
    return {StatusCode::kInvalidArgument,
            "deoptimization record has an unknown reason", record.site};
  }
  if (find(record.site) != nullptr) {
    return {StatusCode::kInvalidArgument,
            "deoptimization site is already registered", record.site};
  }
  for (std::size_t index = 0; index < record.recovery.size(); ++index) {
    const RecoveryOperation& operation = record.recovery[index];
    if (!valid_value_type(operation.type) ||
        !valid_recovery_source(operation.source) ||
        (operation.source == RecoverySource::kCapturedValue &&
         !operation.source_value.valid())) {
      return {StatusCode::kInvalidArgument,
              "deoptimization recovery operation is malformed", index};
    }
    const auto duplicate = std::find_if(
        record.recovery.begin(), record.recovery.begin() + index,
        [&operation](const auto& candidate) {
          return candidate.slot == operation.slot;
        });
    if (duplicate != record.recovery.begin() + index) {
      return {StatusCode::kInvalidArgument,
              "deoptimization recovery slot is duplicated", operation.slot};
    }
  }

  try {
    records_.push_back(record);
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate deoptimization metadata", record.site};
  }
}

Status DeoptimizationTable::validate(
    std::size_t parameter_count) const {
  for (const DeoptimizationRecord& record : records_) {
    for (const RecoveryOperation& operation : record.recovery) {
      if (operation.source == RecoverySource::kArgument &&
          operation.argument_index >= parameter_count) {
        return {StatusCode::kInvalidArgument,
                "deoptimization recovery references an unknown argument",
                record.site};
      }
    }
  }
  return Status::ok_status();
}

const DeoptimizationRecord* DeoptimizationTable::find(
    std::size_t site) const noexcept {
  const auto record = std::find_if(
      records_.begin(), records_.end(),
      [site](const auto& candidate) { return candidate.site == site; });
  return record == records_.end() ? nullptr : &*record;
}

ReconstructionResult DeoptimizationTable::reconstruct(
    std::size_t site, const ir::Word* arguments, std::size_t argument_count,
    const ExecutionContext& context) const {
  const DeoptimizationRecord* record = find(site);
  if (record == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "deoptimization site has no reconstruction metadata", site},
            {}};
  }
  if (context.exit_reason() != ExitReason::kRuntime ||
      context.exit_site() != site) {
    return {{StatusCode::kInvalidArgument,
             "execution context does not match the deoptimization site",
             site},
            {}};
  }
  if (argument_count != 0 && arguments == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "deoptimization argument storage is null", site},
            {}};
  }

  try {
    ReconstructedFrame frame;
    frame.reason = record->reason;
    frame.site = record->site;
    frame.resume_offset = record->resume_offset;
    frame.values.reserve(record->recovery.size());
    for (const RecoveryOperation& operation : record->recovery) {
      ir::Word value = 0;
      switch (operation.source) {
        case RecoverySource::kArgument:
          if (operation.argument_index >= argument_count) {
            return {{StatusCode::kInvalidArgument,
                     "deoptimization invocation has too few arguments", site},
                    {}};
          }
          value = arguments[operation.argument_index];
          break;
        case RecoverySource::kConstant:
          value = operation.constant;
          break;
        case RecoverySource::kExitValue:
          value = context.exit_value();
          break;
        case RecoverySource::kCapturedValue:
          if (!operation.capture_resolved()) {
            return {{StatusCode::kInvalidArgument,
                     "deoptimization captured value is unresolved", site},
                    {}};
          }
          if (operation.capture_index >= context.captured_value_count()) {
            return {{StatusCode::kInvalidArgument,
                     "deoptimization captured value is unavailable", site},
                    {}};
          }
          value = context.captured_values()[operation.capture_index];
          break;
      }
      frame.values.push_back({operation.slot, operation.type, value});
    }
    return {Status::ok_status(), std::move(frame)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate a reconstructed frame", site},
            {}};
  }
}

}  // namespace unijit::runtime
