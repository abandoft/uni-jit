#ifndef UNIJIT_JIT_CAPABILITY_H
#define UNIJIT_JIT_CAPABILITY_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/target.h"
#include "unijit/status.h"

namespace unijit::jit {

enum class LoweringStrategy : std::uint8_t {
  kNative = 0,
  kLegalized,
  kScalarized,
  kHelper,
  kUnsupported,
  kCount,
};

enum class VectorOperationClass : std::uint8_t {
  kMemory = 0,
  kConstruction,
  kLaneMovement,
  kIntegerArithmetic,
  kFloatingArithmetic,
  kBitwiseLogic,
  kComparison,
  kSelection,
  kLaneSignMask,
  kShuffle,
  kWidening,
  kCount,
};

enum class LoweringResource : std::uint32_t {
  kWordRegister = UINT32_C(1) << 0U,
  kFloatRegister = UINT32_C(1) << 1U,
  kVectorRegister = UINT32_C(1) << 2U,
  kAlignedVectorStack = UINT32_C(1) << 3U,
};

constexpr std::uint32_t
lowering_resource_bit(LoweringResource resource) noexcept {
  return static_cast<std::uint32_t>(resource);
}

struct VectorOperationCapability final {
  LoweringStrategy strategy{LoweringStrategy::kNative};
  std::uint64_t required_features{0};
  std::uint32_t resource_mask{0};
  std::size_t operation_count{0};
};

struct CapabilityReport final {
  Status status;
  TargetProfile target_profile;
  LoweringStrategy overall_strategy{LoweringStrategy::kNative};
  std::uint64_t required_features{0};
  std::uint16_t vector_bits{0};
  bool requires_execution_context{false};
  std::array<std::size_t, static_cast<std::size_t>(LoweringStrategy::kCount)>
      operation_counts{};
  std::array<VectorOperationCapability,
             static_cast<std::size_t>(VectorOperationClass::kCount)>
      vector_operations{};

  bool ok() const noexcept {
    return status.ok() && overall_strategy != LoweringStrategy::kUnsupported;
  }

  std::uint64_t target_key() const noexcept {
    return target_profile_key(target_profile);
  }

  std::size_t operation_count(LoweringStrategy strategy) const noexcept {
    const std::size_t index = static_cast<std::size_t>(strategy);
    return index < operation_counts.size() ? operation_counts[index] : 0;
  }

  const VectorOperationCapability *
  vector_operation(VectorOperationClass operation_class) const noexcept {
    const std::size_t index = static_cast<std::size_t>(operation_class);
    return index < vector_operations.size() ? &vector_operations[index]
                                            : nullptr;
  }
};

// Preflight validates IR and the immutable target profile, then classifies the
// complete typed operation set without allocating executable memory or
// emitting machine code. Compilation reuses the same classifier after
// optimization and publishes that exact report with the compiled function.
CapabilityReport preflight_capabilities(const ir::Function &function,
                                        const TargetProfile &target_profile);
CapabilityReport preflight_capabilities(const ir::ControlFlowFunction &function,
                                        const TargetProfile &target_profile);

const char *lowering_strategy_name(LoweringStrategy strategy) noexcept;
const char *
vector_operation_class_name(VectorOperationClass operation_class) noexcept;

} // namespace unijit::jit

#endif // UNIJIT_JIT_CAPABILITY_H
