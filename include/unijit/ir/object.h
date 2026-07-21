#ifndef UNIJIT_IR_OBJECT_H
#define UNIJIT_IR_OBJECT_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace unijit::ir {

class TrustedObjectSlot final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr TrustedObjectSlot() noexcept = default;
  explicit constexpr TrustedObjectSlot(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(TrustedObjectSlot lhs,
                                   TrustedObjectSlot rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(TrustedObjectSlot lhs,
                                   TrustedObjectSlot rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

struct TrustedObjectDescriptor final {
  static constexpr std::size_t kMaximumByteSize = 2048;

  std::uint64_t layout_identity{0};
  std::size_t byte_size{0};
};

}  // namespace unijit::ir

#endif  // UNIJIT_IR_OBJECT_H
