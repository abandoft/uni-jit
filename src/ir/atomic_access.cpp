#include "ir/atomic_access.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "ir/memory_access.h"

namespace unijit::ir::detail {
namespace {

std::mutex &interpreter_atomic_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::uint64_t word_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word bits_word(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint64_t width_mask(std::size_t width) noexcept {
  return width == sizeof(std::uint64_t)
             ? UINT64_MAX
             : (UINT64_C(1) << (width * 8U)) - UINT64_C(1);
}

std::memory_order fence_order(AtomicMemoryOrder order) noexcept {
  switch (order) {
  case AtomicMemoryOrder::kAcquire:
    return std::memory_order_acquire;
  case AtomicMemoryOrder::kRelease:
    return std::memory_order_release;
  case AtomicMemoryOrder::kAcquireRelease:
    return std::memory_order_acq_rel;
  case AtomicMemoryOrder::kSequentiallyConsistent:
    return std::memory_order_seq_cst;
  case AtomicMemoryOrder::kRelaxed:
    return std::memory_order_relaxed;
  }
  return std::memory_order_seq_cst;
}

} // namespace

AtomicAccessResult access_bounded_atomic(const AtomicAccessDescriptor &access,
                                         AtomicOperation operation,
                                         Word byte_offset, Word value,
                                         Word desired, std::size_t site,
                                         runtime::ExecutionContext *context) {
  const bool modifies = operation != AtomicOperation::kLoad;
  const ResolvedMemoryAccess resolved = resolve_bounded_memory(
      access.memory, byte_offset, site, context, modifies);
  if (!resolved.ok()) {
    return {resolved.status, 0, false};
  }

  std::lock_guard<std::mutex> lock(interpreter_atomic_mutex());
  std::uint64_t observed = 0;
  std::memcpy(&observed, resolved.address, resolved.width);
  const std::uint64_t mask = width_mask(resolved.width);
  observed &= mask;
  const std::uint64_t operand = word_bits(value) & mask;
  const std::uint64_t desired_bits = word_bits(desired) & mask;
  std::uint64_t replacement = observed;
  bool write = false;
  bool success = false;

  switch (operation) {
  case AtomicOperation::kLoad:
    break;
  case AtomicOperation::kStore:
    replacement = operand;
    write = true;
    break;
  case AtomicOperation::kExchange:
    replacement = operand;
    write = true;
    break;
  case AtomicOperation::kCompareExchange:
    success = observed == operand;
    replacement = desired_bits;
    write = success;
    break;
  case AtomicOperation::kFetchAdd:
    replacement = (observed + operand) & mask;
    write = true;
    break;
  case AtomicOperation::kFetchAnd:
    replacement = observed & operand;
    write = true;
    break;
  case AtomicOperation::kFetchOr:
    replacement = observed | operand;
    write = true;
    break;
  case AtomicOperation::kFetchXor:
    replacement = observed ^ operand;
    write = true;
    break;
  }
  if (write) {
    std::memcpy(resolved.address, &replacement, resolved.width);
  }
  const Word result =
      operation == AtomicOperation::kStore ? value : bits_word(observed);
  return {Status::ok_status(), result, success};
}

void execute_atomic_fence(AtomicMemoryOrder order) {
  std::lock_guard<std::mutex> lock(interpreter_atomic_mutex());
  std::atomic_thread_fence(fence_order(order));
}

} // namespace unijit::ir::detail
