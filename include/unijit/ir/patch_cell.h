#ifndef UNIJIT_IR_PATCH_CELL_H
#define UNIJIT_IR_PATCH_CELL_H

#include <cstdint>
#include <limits>

#include "unijit/ir/vector.h"

namespace unijit::ir {

enum class PatchCellKind : std::uint8_t {
  kValue = 0,
  kTarget,
  kShape,
  kGeneration,
  kCounter,
};

class PatchCellSlot final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr PatchCellSlot() noexcept = default;
  explicit constexpr PatchCellSlot(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(PatchCellSlot lhs,
                                   PatchCellSlot rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(PatchCellSlot lhs,
                                   PatchCellSlot rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

struct PatchCellDescriptor final {
  Word initial_value{0};
  PatchCellKind kind{PatchCellKind::kValue};
};

constexpr bool is_valid_patch_cell_kind(PatchCellKind kind) noexcept {
  return kind >= PatchCellKind::kValue && kind <= PatchCellKind::kCounter;
}

}  // namespace unijit::ir

#endif  // UNIJIT_IR_PATCH_CELL_H
