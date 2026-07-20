#include "jit/backend/aarch64/lower.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "jit/code_buffer.h"

namespace unijit::jit::detail::aarch64 {
namespace {

constexpr int kReturnRegister = 0;
constexpr int kArgumentBaseRegister = 9;
constexpr int kScratch0 = 16;
constexpr int kScratch1 = 17;
constexpr int kStackPointer = 31;
constexpr std::array<int, 6> kAllocationRegisters = {10, 11, 12, 13, 14,
                                                     15};
constexpr std::size_t kNoSpill = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaximumStackSize = 4080;
constexpr std::size_t kMaximumAddressableParameters = 4096;

struct Location final {
  int reg{-1};
  std::size_t spill_slot{kNoSpill};

  bool in_register() const noexcept { return reg >= 0; }
};

struct Allocation final {
  Status status;
  std::vector<Location> locations;
  std::size_t spill_slots{0};
};

void note_use(std::vector<std::size_t>* last_use, ir::Value value,
              std::size_t use_index) {
  auto& entry = (*last_use)[value.id()];
  entry = std::max(entry, use_index);
}

Allocation allocate_registers(const ir::Function& function) {
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

  std::vector<Location> locations(value_count);
  std::vector<std::size_t> active;
  std::vector<int> free_registers(kAllocationRegisters.rbegin(),
                                  kAllocationRegisters.rend());
  std::size_t spill_slots = 0;

  for (std::size_t index = 0; index < value_count; ++index) {
    auto active_it = active.begin();
    while (active_it != active.end()) {
      const std::size_t active_value = *active_it;
      if (last_use[active_value] < index) {
        free_registers.push_back(locations[active_value].reg);
        active_it = active.erase(active_it);
      } else {
        ++active_it;
      }
    }

    if (!free_registers.empty()) {
      locations[index].reg = free_registers.back();
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
      locations[index].reg = locations[victim].reg;
      locations[victim].reg = -1;
      locations[victim].spill_slot = spill_slots++;
      *victim_it = index;
    } else {
      locations[index].spill_slot = spill_slots++;
    }
  }

  if (spill_slots > kMaximumStackSize / sizeof(ir::Word)) {
    return {{StatusCode::kResourceExhausted,
             "AArch64 bootstrap allocator exceeded its spill-frame limit"},
            {}, 0};
  }
  return {Status::ok_status(), std::move(locations), spill_slots};
}

class Assembler final {
 public:
  void move_register(int destination, int source) {
    buffer_.emit_u32(0xAA0003E0U | (reg(source) << 16U) | reg(destination));
  }

  void move_immediate(int destination, ir::Word immediate) {
    const auto bits = static_cast<std::uint64_t>(immediate);
    bool emitted = false;
    for (std::uint32_t half = 0; half < 4; ++half) {
      const auto value =
          static_cast<std::uint32_t>((bits >> (half * 16U)) & 0xFFFFU);
      if (value == 0 && emitted) {
        continue;
      }
      if (!emitted) {
        buffer_.emit_u32(0xD2800000U | (half << 21U) | (value << 5U) |
                         reg(destination));
        emitted = true;
      } else {
        buffer_.emit_u32(0xF2800000U | (half << 21U) | (value << 5U) |
                         reg(destination));
      }
    }
    if (!emitted) {
      buffer_.emit_u32(0xD2800000U | reg(destination));
    }
  }

  void load(int destination, int base, std::size_t byte_offset) {
    const auto scaled_offset = static_cast<std::uint32_t>(byte_offset / 8U);
    buffer_.emit_u32(0xF9400000U | (scaled_offset << 10U) |
                     (reg(base) << 5U) | reg(destination));
  }

  void store(int source, int base, std::size_t byte_offset) {
    const auto scaled_offset = static_cast<std::uint32_t>(byte_offset / 8U);
    buffer_.emit_u32(0xF9000000U | (scaled_offset << 10U) |
                     (reg(base) << 5U) | reg(source));
  }

  void add(int destination, int lhs, int rhs) {
    buffer_.emit_u32(0x8B000000U | (reg(rhs) << 16U) | (reg(lhs) << 5U) |
                     reg(destination));
  }

  void subtract(int destination, int lhs, int rhs) {
    buffer_.emit_u32(0xCB000000U | (reg(rhs) << 16U) | (reg(lhs) << 5U) |
                     reg(destination));
  }

  void multiply(int destination, int lhs, int rhs) {
    buffer_.emit_u32(0x9B007C00U | (reg(rhs) << 16U) | (reg(lhs) << 5U) |
                     reg(destination));
  }

  void reserve_stack(std::size_t byte_count) {
    buffer_.emit_u32(0xD1000000U |
                     (static_cast<std::uint32_t>(byte_count) << 10U) |
                     (reg(kStackPointer) << 5U) | reg(kStackPointer));
  }

  void release_stack(std::size_t byte_count) {
    buffer_.emit_u32(0x91000000U |
                     (static_cast<std::uint32_t>(byte_count) << 10U) |
                     (reg(kStackPointer) << 5U) | reg(kStackPointer));
  }

  void return_to_caller() { buffer_.emit_u32(0xD65F03C0U); }

  std::vector<std::uint8_t> take_code() noexcept {
    return buffer_.take_bytes();
  }

 private:
  static std::uint32_t reg(int value) noexcept {
    return static_cast<std::uint32_t>(value);
  }

  CodeBuffer buffer_;
};

std::size_t spill_offset(const Location& location) noexcept {
  return location.spill_slot * sizeof(ir::Word);
}

int load_operand(Assembler* assembler, const Location& location,
                 int scratch) {
  if (location.in_register()) {
    return location.reg;
  }
  assembler->load(scratch, kStackPointer, spill_offset(location));
  return scratch;
}

LoweringResult lower_impl(const ir::Function& function) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the AArch64 encoder currently supports little-endian targets"},
          {}, 0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "AArch64 parameter area exceeds direct-load addressing"},
            {}, 0};
  }

  Allocation allocation = allocate_registers(function);
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kReturnRegister);
  const std::size_t raw_stack_size =
      allocation.spill_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }

  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::Node& node = function.nodes()[index];
    const Location& destination = allocation.locations[index];
    switch (node.opcode) {
      case ir::Opcode::kParameter: {
        const int target =
            destination.in_register() ? destination.reg : kScratch0;
        assembler.load(target, kArgumentBaseRegister,
                       static_cast<std::size_t>(node.immediate) *
                           sizeof(ir::Word));
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kConstant: {
        const int target =
            destination.in_register() ? destination.reg : kScratch0;
        assembler.move_immediate(target, node.immediate);
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kAdd:
      case ir::Opcode::kSubtract:
      case ir::Opcode::kMultiply: {
        const int lhs = load_operand(&assembler,
                                     allocation.locations[node.lhs.id()],
                                     kScratch0);
        const int rhs = load_operand(&assembler,
                                     allocation.locations[node.rhs.id()],
                                     kScratch1);
        const int target =
            destination.in_register() ? destination.reg : kScratch0;
        if (node.opcode == ir::Opcode::kAdd) {
          assembler.add(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kSubtract) {
          assembler.subtract(target, lhs, rhs);
        } else {
          assembler.multiply(target, lhs, rhs);
        }
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
    }
  }

  const Location& returned =
      allocation.locations[function.return_value().id()];
  if (returned.in_register()) {
    assembler.move_register(kReturnRegister, returned.reg);
  } else {
    assembler.load(kReturnRegister, kStackPointer, spill_offset(returned));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  return {Status::ok_status(), assembler.take_code(), allocation.spill_slots};
}

}  // namespace

LoweringResult lower(const ir::Function& function) {
  try {
    return lower_impl(function);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate AArch64 lowering state"},
            {}, 0};
  }
}

}  // namespace unijit::jit::detail::aarch64
