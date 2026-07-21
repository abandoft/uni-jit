#include "jit/atomic_fallback.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace unijit::jit::detail {
namespace {

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
  switch (width) {
    case 1:
      return execute_width<std::uint8_t>(address, operation, operand, desired);
    case 2:
      return execute_width<std::uint16_t>(address, operation, operand, desired);
    case 4:
      return execute_width<std::uint32_t>(address, operation, operand, desired);
    case 8:
      return execute_width<std::uint64_t>(address, operation, operand, desired);
    default:
      return 0;
  }
}

}  // namespace unijit::jit::detail
