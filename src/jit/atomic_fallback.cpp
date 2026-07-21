#include "jit/atomic_fallback.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace unijit::jit::detail {
namespace {

#if defined(UNIJIT_TARGET_AARCH64)
static_assert(__atomic_always_lock_free(sizeof(std::uint8_t), nullptr),
              "byte atomics must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(std::uint16_t), nullptr),
              "halfword atomics must be lock-free");
#endif
static_assert(__atomic_always_lock_free(sizeof(std::uint32_t), nullptr),
              "word atomics must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(std::uint64_t), nullptr),
              "doubleword atomics must be lock-free");

#if defined(UNIJIT_TARGET_RISCV64)
std::mutex& subword_atomic_mutex() {
  static std::mutex mutex;
  return mutex;
}
#endif

std::uint64_t word_bits(ir::Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

ir::Word bits_word(std::uint64_t bits) noexcept {
  ir::Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

template <typename T>
ir::Word execute_width(std::uintptr_t address,
                       AtomicFallbackOperation operation,
                       std::uint64_t operand_bits,
                       std::uint64_t desired_bits) noexcept {
  auto* target = reinterpret_cast<T*>(address);
  const T operand = static_cast<T>(operand_bits);
  const T desired = static_cast<T>(desired_bits);
  T observed = 0;
  switch (operation) {
    case AtomicFallbackOperation::kLoad:
      observed = __atomic_load_n(target, __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kStore:
      __atomic_store_n(target, operand, __ATOMIC_SEQ_CST);
      observed = operand;
      break;
    case AtomicFallbackOperation::kExchange:
      observed = __atomic_exchange_n(target, operand, __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kCompareExchange:
      observed = operand;
      (void)__atomic_compare_exchange_n(target, &observed, desired, false,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kFetchAdd:
      observed = __atomic_fetch_add(target, operand, __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kFetchAnd:
      observed = __atomic_fetch_and(target, operand, __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kFetchOr:
      observed = __atomic_fetch_or(target, operand, __ATOMIC_SEQ_CST);
      break;
    case AtomicFallbackOperation::kFetchXor:
      observed = __atomic_fetch_xor(target, operand, __ATOMIC_SEQ_CST);
      break;
  }
  return bits_word(static_cast<std::uint64_t>(observed));
}

#if defined(UNIJIT_TARGET_RISCV64)
template <typename T>
ir::Word execute_locked_width(std::uintptr_t address,
                              AtomicFallbackOperation operation,
                              std::uint64_t operand_bits,
                              std::uint64_t desired_bits) noexcept {
  auto* target = reinterpret_cast<void*>(address);
  const T operand = static_cast<T>(operand_bits);
  const T desired = static_cast<T>(desired_bits);
  std::lock_guard<std::mutex> lock(subword_atomic_mutex());
  T observed = 0;
  std::memcpy(&observed, target, sizeof(observed));
  T replacement = observed;
  bool write = false;
  switch (operation) {
    case AtomicFallbackOperation::kLoad:
      break;
    case AtomicFallbackOperation::kStore:
    case AtomicFallbackOperation::kExchange:
      replacement = operand;
      write = true;
      break;
    case AtomicFallbackOperation::kCompareExchange:
      replacement = desired;
      write = observed == operand;
      break;
    case AtomicFallbackOperation::kFetchAdd:
      replacement = static_cast<T>(observed + operand);
      write = true;
      break;
    case AtomicFallbackOperation::kFetchAnd:
      replacement = static_cast<T>(observed & operand);
      write = true;
      break;
    case AtomicFallbackOperation::kFetchOr:
      replacement = static_cast<T>(observed | operand);
      write = true;
      break;
    case AtomicFallbackOperation::kFetchXor:
      replacement = static_cast<T>(observed ^ operand);
      write = true;
      break;
  }
  if (write) {
    std::memcpy(target, &replacement, sizeof(replacement));
  }
  return bits_word(static_cast<std::uint64_t>(observed));
}
#endif

}  // namespace

ir::Word execute_atomic_fallback(const ir::Word* arguments,
                                 std::size_t count) noexcept {
  if (arguments == nullptr || count != 5) {
    return 0;
  }
  const auto address = static_cast<std::uintptr_t>(word_bits(arguments[0]));
  const auto operation =
      static_cast<AtomicFallbackOperation>(word_bits(arguments[1]));
  const std::uint64_t width = word_bits(arguments[2]);
  const std::uint64_t operand = word_bits(arguments[3]);
  const std::uint64_t desired = word_bits(arguments[4]);
  ir::Word result = 0;
  switch (width) {
    case 1:
#if defined(UNIJIT_TARGET_RISCV64)
      result = execute_locked_width<std::uint8_t>(address, operation, operand,
                                                  desired);
#else
      result = execute_width<std::uint8_t>(address, operation, operand,
                                           desired);
#endif
      break;
    case 2:
#if defined(UNIJIT_TARGET_RISCV64)
      result = execute_locked_width<std::uint16_t>(address, operation, operand,
                                                   desired);
#else
      result = execute_width<std::uint16_t>(address, operation, operand,
                                            desired);
#endif
      break;
    case 4:
      result = execute_width<std::uint32_t>(address, operation, operand,
                                            desired);
      break;
    case 8:
      result = execute_width<std::uint64_t>(address, operation, operand,
                                            desired);
      break;
    default:
      return 0;
  }
  return operation == AtomicFallbackOperation::kStore ? arguments[3] : result;
}

}  // namespace unijit::jit::detail
