#ifndef UNIJIT_IR_ATOMIC_H
#define UNIJIT_IR_ATOMIC_H

#include <cstdint>
#include <limits>

#include "unijit/ir/memory.h"

namespace unijit::ir {

enum class AtomicMemoryOrder : std::uint8_t {
  kRelaxed = 0,
  kAcquire,
  kRelease,
  kAcquireRelease,
  kSequentiallyConsistent,
};

enum class AtomicCompareExchangeStrength : std::uint8_t {
  kStrong = 0,
  kWeak,
};

struct AtomicAccessDescriptor final {
  static constexpr std::uint32_t kInvalidIndex =
      std::numeric_limits<std::uint32_t>::max();

  MemoryAccessDescriptor memory;
  AtomicMemoryOrder order{AtomicMemoryOrder::kSequentiallyConsistent};
  AtomicMemoryOrder failure_order{AtomicMemoryOrder::kRelaxed};
  AtomicCompareExchangeStrength strength{
      AtomicCompareExchangeStrength::kStrong};
};

constexpr bool is_valid_atomic_memory_order(AtomicMemoryOrder order) noexcept {
  return order == AtomicMemoryOrder::kRelaxed ||
         order == AtomicMemoryOrder::kAcquire ||
         order == AtomicMemoryOrder::kRelease ||
         order == AtomicMemoryOrder::kAcquireRelease ||
         order == AtomicMemoryOrder::kSequentiallyConsistent;
}

constexpr bool is_valid_atomic_load_order(AtomicMemoryOrder order) noexcept {
  return order == AtomicMemoryOrder::kRelaxed ||
         order == AtomicMemoryOrder::kAcquire ||
         order == AtomicMemoryOrder::kSequentiallyConsistent;
}

constexpr bool is_valid_atomic_store_order(AtomicMemoryOrder order) noexcept {
  return order == AtomicMemoryOrder::kRelaxed ||
         order == AtomicMemoryOrder::kRelease ||
         order == AtomicMemoryOrder::kSequentiallyConsistent;
}

constexpr bool is_valid_atomic_failure_order(AtomicMemoryOrder order) noexcept {
  return order == AtomicMemoryOrder::kRelaxed ||
         order == AtomicMemoryOrder::kAcquire ||
         order == AtomicMemoryOrder::kSequentiallyConsistent;
}

constexpr bool is_valid_atomic_fence_order(AtomicMemoryOrder order) noexcept {
  return order == AtomicMemoryOrder::kAcquire ||
         order == AtomicMemoryOrder::kRelease ||
         order == AtomicMemoryOrder::kAcquireRelease ||
         order == AtomicMemoryOrder::kSequentiallyConsistent;
}

constexpr bool
atomic_failure_order_allowed(AtomicMemoryOrder success,
                             AtomicMemoryOrder failure) noexcept {
  if (!is_valid_atomic_failure_order(failure)) {
    return false;
  }
  if (failure == AtomicMemoryOrder::kSequentiallyConsistent) {
    return success == AtomicMemoryOrder::kSequentiallyConsistent;
  }
  if (failure == AtomicMemoryOrder::kAcquire) {
    return success == AtomicMemoryOrder::kAcquire ||
           success == AtomicMemoryOrder::kAcquireRelease ||
           success == AtomicMemoryOrder::kSequentiallyConsistent;
  }
  return is_valid_atomic_memory_order(success);
}

constexpr bool is_valid_atomic_compare_exchange_strength(
    AtomicCompareExchangeStrength strength) noexcept {
  return strength == AtomicCompareExchangeStrength::kStrong ||
         strength == AtomicCompareExchangeStrength::kWeak;
}

} // namespace unijit::ir

#endif // UNIJIT_IR_ATOMIC_H
