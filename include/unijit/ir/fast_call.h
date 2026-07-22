#ifndef UNIJIT_IR_FAST_CALL_H
#define UNIJIT_IR_FAST_CALL_H

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "unijit/ir/vector.h"

namespace unijit::ir {

class FastCallSlot final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr FastCallSlot() noexcept = default;
  explicit constexpr FastCallSlot(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(FastCallSlot lhs,
                                   FastCallSlot rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(FastCallSlot lhs,
                                   FastCallSlot rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

struct FastCallDescriptor final {
  FastCallDescriptor() = default;
  FastCallDescriptor(std::vector<ValueType> parameters, ValueType result)
      : parameter_types(std::move(parameters)), return_type(result) {}

  std::vector<ValueType> parameter_types;
  ValueType return_type{ValueType::kWord};
};

}  // namespace unijit::ir

#endif  // UNIJIT_IR_FAST_CALL_H
