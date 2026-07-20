#ifndef UNIJIT_SRC_JIT_STACK_MAP_REQUIREMENTS_H
#define UNIJIT_SRC_JIT_STACK_MAP_REQUIREMENTS_H

#include <cstddef>
#include <vector>

#include "unijit/ir/function.h"

namespace unijit::jit::detail {

struct StackMapRequirement final {
  std::size_t site{0};
  std::vector<ir::Value> values;
};

using StackMapRequirements = std::vector<StackMapRequirement>;

inline const StackMapRequirement* find_stack_map_requirement(
    const StackMapRequirements& requirements, std::size_t site) noexcept {
  for (const StackMapRequirement& requirement : requirements) {
    if (requirement.site == site) {
      return &requirement;
    }
  }
  return nullptr;
}

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_STACK_MAP_REQUIREMENTS_H
