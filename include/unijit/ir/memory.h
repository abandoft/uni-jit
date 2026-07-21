#ifndef UNIJIT_IR_MEMORY_H
#define UNIJIT_IR_MEMORY_H

#include <cstdint>
#include <limits>

namespace unijit::ir {

enum class MemoryWidth : std::uint8_t {
  k8 = 1,
  k16 = 2,
  k32 = 4,
  k64 = 8,
};

enum class MemoryByteOrder : std::uint8_t {
  kNative = 0,
  kLittleEndian,
  kBigEndian,
};

struct MemoryAccessDescriptor final {
  static constexpr std::uint32_t kInvalidIndex =
      std::numeric_limits<std::uint32_t>::max();

  std::uint32_t region{0};
  std::uint32_t alias_class{0};
  MemoryWidth width{MemoryWidth::k8};
  std::uint8_t alignment{1};
  MemoryByteOrder byte_order{MemoryByteOrder::kNative};
  bool sign_extend{false};
  bool is_volatile{false};
};

constexpr std::size_t memory_width_bytes(MemoryWidth width) noexcept {
  return static_cast<std::size_t>(width);
}

}  // namespace unijit::ir

#endif  // UNIJIT_IR_MEMORY_H
