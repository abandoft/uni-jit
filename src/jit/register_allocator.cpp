#include "jit/register_allocator.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace unijit::jit::detail {
namespace {

void note_use(std::vector<std::size_t>* last_use, ir::Value value,
              std::size_t use_index) {
  auto& entry = (*last_use)[value.id()];
  entry = std::max(entry, use_index);
}

RegisterAllocation allocate_impl(const ir::Function& function,
                                 std::size_t register_count,
                                 std::size_t maximum_spill_slots) {
  if (register_count == 0) {
    return {{StatusCode::kInvalidArgument,
             "linear scan requires at least one allocatable register"},
            {}, 0};
  }

  const std::size_t value_count = function.nodes().size();
  std::vector<std::size_t> last_use(value_count);
  for (std::size_t index = 0; index < value_count; ++index) {
    last_use[index] = index;
    const ir::Node& node = function.nodes()[index];
    if (node.opcode == ir::Opcode::kAdd ||
        node.opcode == ir::Opcode::kSubtract ||
        node.opcode == ir::Opcode::kMultiply) {
      note_use(&last_use, node.lhs, index);
      note_use(&last_use, node.rhs, index);
    }
  }
  note_use(&last_use, function.return_value(), value_count);

  std::vector<ValueLocation> locations(value_count);
  std::vector<std::size_t> active;
  std::vector<std::size_t> free_registers;
  free_registers.reserve(register_count);
  for (std::size_t index = register_count; index > 0; --index) {
    free_registers.push_back(index - 1);
  }
  std::size_t spill_slots = 0;

  for (std::size_t index = 0; index < value_count; ++index) {
    auto active_it = active.begin();
    while (active_it != active.end()) {
      const std::size_t active_value = *active_it;
      if (last_use[active_value] < index) {
        free_registers.push_back(locations[active_value].register_index);
        active_it = active.erase(active_it);
      } else {
        ++active_it;
      }
    }

    if (!free_registers.empty()) {
      locations[index].register_index = free_registers.back();
      free_registers.pop_back();
      active.push_back(index);
      continue;
    }

    const auto victim_it = std::max_element(
        active.begin(), active.end(), [&](std::size_t lhs, std::size_t rhs) {
          return last_use[lhs] < last_use[rhs];
        });
    if (victim_it != active.end() && last_use[*victim_it] > last_use[index]) {
      const std::size_t victim = *victim_it;
      locations[index].register_index = locations[victim].register_index;
      locations[victim].register_index = ValueLocation::kNone;
      locations[victim].spill_slot = spill_slots++;
      *victim_it = index;
    } else {
      locations[index].spill_slot = spill_slots++;
    }
  }

  if (spill_slots > maximum_spill_slots) {
    return {{StatusCode::kResourceExhausted,
             "linear scan exceeded the backend spill-frame limit"},
            {}, 0};
  }
  return {Status::ok_status(), std::move(locations), spill_slots};
}

}  // namespace

RegisterAllocation allocate_linear_scan(const ir::Function& function,
                                        std::size_t register_count,
                                        std::size_t maximum_spill_slots) {
  try {
    return allocate_impl(function, register_count, maximum_spill_slots);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate linear-scan state"},
            {}, 0};
  }
}

}  // namespace unijit::jit::detail
