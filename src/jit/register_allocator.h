#ifndef UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H
#define UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H

#include <cstddef>
#include <limits>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::jit::detail {

struct ValueLocation final {
  static constexpr std::size_t kNone =
      std::numeric_limits<std::size_t>::max();

  std::size_t register_index{kNone};
  std::size_t spill_slot{kNone};

  bool in_register() const noexcept { return register_index != kNone; }
};

struct RegisterAllocation final {
  Status status;
  std::vector<ValueLocation> locations;
  std::size_t spill_slots{0};
};

RegisterAllocation allocate_linear_scan(const ir::Function& function,
                                        std::size_t register_count,
                                        std::size_t maximum_spill_slots);

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H
