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
#include "jit/register_allocator.h"

namespace unijit::jit::detail::aarch64 {
namespace {

constexpr int kReturnRegister = 0;
constexpr int kArgumentBaseRegister = 9;
constexpr int kScratch0 = 16;
constexpr int kScratch1 = 17;
constexpr int kStackPointer = 31;
constexpr std::array<int, 6> kAllocationRegisters = {10, 11, 12, 13, 14, 15};
constexpr std::size_t kMaximumStackSize = 4080;
constexpr std::size_t kMaximumAddressableParameters = 4096;

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
    buffer_.emit_u32(0xF9400000U | (scaled_offset << 10U) | (reg(base) << 5U) |
                     reg(destination));
  }

  void store(int source, int base, std::size_t byte_offset) {
    const auto scaled_offset = static_cast<std::uint32_t>(byte_offset / 8U);
    buffer_.emit_u32(0xF9000000U | (scaled_offset << 10U) | (reg(base) << 5U) |
                     reg(source));
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

  void compare(int destination, int lhs, int rhs, bool or_equal) {
    buffer_.emit_u32(0xEB00001FU | (reg(rhs) << 16U) | (reg(lhs) << 5U));
    const std::uint32_t inverse_condition = or_equal ? 0xCU : 0xAU;
    buffer_.emit_u32(0x9A800400U | (reg(31) << 16U) |
                     (inverse_condition << 12U) | (reg(31) << 5U) |
                     reg(destination));
  }

  std::size_t branch() {
    const std::size_t offset = buffer_.size();
    buffer_.emit_u32(0x14000000U);
    return offset;
  }

  std::size_t branch_nonzero(int source) {
    const std::size_t offset = buffer_.size();
    buffer_.emit_u32(0xB5000000U | reg(source));
    return offset;
  }

  Status patch_branch(std::size_t offset, std::size_t target,
                      bool conditional) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(offset);
    if ((delta & 3) != 0) {
      return {StatusCode::kCodeGenerationFailed,
              "AArch64 branch target is not instruction aligned"};
    }
    const std::int64_t words = delta / 4;
    if (conditional) {
      constexpr std::int64_t kMinimum = -(std::int64_t{1} << 18);
      constexpr std::int64_t kMaximum = (std::int64_t{1} << 18) - 1;
      if (words < kMinimum || words > kMaximum) {
        return {StatusCode::kResourceExhausted,
                "AArch64 conditional branch exceeds its encoding range"};
      }
      const std::uint32_t instruction =
          0xB5000000U | ((static_cast<std::uint32_t>(words) & 0x7FFFFU) << 5U) |
          reg(kScratch0);
      buffer_.patch_u32(offset, instruction);
    } else {
      constexpr std::int64_t kMinimum = -(std::int64_t{1} << 25);
      constexpr std::int64_t kMaximum = (std::int64_t{1} << 25) - 1;
      if (words < kMinimum || words > kMaximum) {
        return {StatusCode::kResourceExhausted,
                "AArch64 branch exceeds its encoding range"};
      }
      buffer_.patch_u32(
          offset,
          0x14000000U | (static_cast<std::uint32_t>(words) & 0x03FFFFFFU));
    }
    return Status::ok_status();
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

  std::size_t size() const noexcept { return buffer_.size(); }

  std::vector<std::uint8_t> take_code() noexcept {
    return buffer_.take_bytes();
  }

 private:
  static std::uint32_t reg(int value) noexcept {
    return static_cast<std::uint32_t>(value);
  }

  CodeBuffer buffer_;
};

int physical_register(const ValueLocation& location) noexcept {
  return kAllocationRegisters[location.register_index];
}

std::size_t spill_offset(const ValueLocation& location) noexcept {
  return location.spill_slot * sizeof(ir::Word);
}

int load_operand(Assembler* assembler, const ValueLocation& location,
                 int scratch) {
  if (location.in_register()) {
    return physical_register(location);
  }
  assembler->load(scratch, kStackPointer, spill_offset(location));
  return scratch;
}

LoweringResult lower_impl(const ir::Function& function) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the AArch64 encoder currently supports little-endian targets"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "AArch64 parameter area exceeds direct-load addressing"},
            {},
            0};
  }

  RegisterAllocation allocation =
      allocate_linear_scan(function, kAllocationRegisters.size(),
                           kMaximumStackSize / sizeof(ir::Word));
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kReturnRegister);
  const std::size_t raw_stack_size = allocation.spill_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }

  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::Node& node = function.nodes()[index];
    const ValueLocation& destination = allocation.locations[index];
    switch (node.opcode) {
      case ir::Opcode::kParameter: {
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.load(
            target, kArgumentBaseRegister,
            static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kConstant: {
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, node.immediate);
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kAdd:
      case ir::Opcode::kSubtract:
      case ir::Opcode::kMultiply: {
        const int lhs = load_operand(
            &assembler, allocation.locations[node.lhs.id()], kScratch0);
        const int rhs = load_operand(
            &assembler, allocation.locations[node.rhs.id()], kScratch1);
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
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

  const ValueLocation& returned =
      allocation.locations[function.return_value().id()];
  if (returned.in_register()) {
    assembler.move_register(kReturnRegister, physical_register(returned));
  } else {
    assembler.load(kReturnRegister, kStackPointer, spill_offset(returned));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  return {Status::ok_status(), assembler.take_code(), allocation.spill_slots};
}

struct BranchFixup final {
  std::size_t offset{0};
  ir::Block target;
};

std::size_t control_spill_offset(std::size_t slot) noexcept {
  return slot * sizeof(ir::Word);
}

void copy_edge_arguments(Assembler* assembler,
                         const ir::ControlFlowFunction& function,
                         const ir::ControlEdge& edge,
                         std::size_t temporary_base) {
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    assembler->load(kScratch0, kStackPointer,
                    control_spill_offset(edge.arguments[index].id()));
    assembler->store(kScratch0, kStackPointer,
                     control_spill_offset(temporary_base + index));
  }
  const ir::BasicBlock& target = function.blocks()[edge.target.id()];
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    assembler->load(kScratch0, kStackPointer,
                    control_spill_offset(temporary_base + index));
    assembler->store(kScratch0, kStackPointer,
                     control_spill_offset(target.parameters[index].id()));
  }
}

LoweringResult lower_control_flow_impl(
    const ir::ControlFlowFunction& function) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the AArch64 encoder currently supports little-endian targets"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "AArch64 parameter area exceeds direct-load addressing"},
            {},
            0};
  }

  std::size_t maximum_block_parameters = 0;
  for (const ir::BasicBlock& block : function.blocks()) {
    maximum_block_parameters =
        std::max(maximum_block_parameters, block.parameters.size());
  }
  const std::size_t spill_slots =
      function.nodes().size() + maximum_block_parameters;
  const std::size_t raw_stack_size = spill_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size > kMaximumStackSize) {
    return {{StatusCode::kResourceExhausted,
             "AArch64 CFG spill frame exceeds the backend limit"},
            {},
            0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kReturnRegister);
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }

  const std::size_t no_label = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> labels(function.blocks().size(), no_label);
  std::vector<BranchFixup> fixups;
  const std::size_t temporary_base = function.nodes().size();

  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    labels[block_index] = assembler.size();
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (const ir::Value value : block.instructions) {
      const ir::ControlNode& node = function.nodes()[value.id()];
      const std::size_t destination_offset = control_spill_offset(value.id());
      switch (node.opcode) {
        case ir::ControlOpcode::kParameter:
          assembler.load(
              kScratch0, kArgumentBaseRegister,
              static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
          assembler.store(kScratch0, kStackPointer, destination_offset);
          break;
        case ir::ControlOpcode::kBlockParameter:
          break;
        case ir::ControlOpcode::kConstant:
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.store(kScratch0, kStackPointer, destination_offset);
          break;
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          assembler.load(kScratch0, kStackPointer,
                         control_spill_offset(node.lhs.id()));
          assembler.load(kScratch1, kStackPointer,
                         control_spill_offset(node.rhs.id()));
          if (node.opcode == ir::ControlOpcode::kAdd) {
            assembler.add(kScratch0, kScratch0, kScratch1);
          } else if (node.opcode == ir::ControlOpcode::kSubtract) {
            assembler.subtract(kScratch0, kScratch0, kScratch1);
          } else if (node.opcode == ir::ControlOpcode::kMultiply) {
            assembler.multiply(kScratch0, kScratch0, kScratch1);
          } else {
            assembler.compare(kScratch0, kScratch0, kScratch1,
                              node.opcode == ir::ControlOpcode::kLessEqual);
          }
          assembler.store(kScratch0, kStackPointer, destination_offset);
          break;
      }
    }

    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kReturn) {
      assembler.load(kReturnRegister, kStackPointer,
                     control_spill_offset(terminator.value.id()));
      if (stack_size != 0) {
        assembler.release_stack(stack_size);
      }
      assembler.return_to_caller();
    } else if (terminator.opcode == ir::TerminatorOpcode::kJump) {
      copy_edge_arguments(&assembler, function, terminator.true_edge,
                          temporary_base);
      fixups.push_back({assembler.branch(), terminator.true_edge.target});
    } else {
      assembler.load(kScratch0, kStackPointer,
                     control_spill_offset(terminator.value.id()));
      const std::size_t true_selector = assembler.branch_nonzero(kScratch0);
      copy_edge_arguments(&assembler, function, terminator.false_edge,
                          temporary_base);
      fixups.push_back({assembler.branch(), terminator.false_edge.target});
      const std::size_t true_copy = assembler.size();
      const Status selector_status =
          assembler.patch_branch(true_selector, true_copy, true);
      if (!selector_status.ok()) {
        return {selector_status, {}, 0};
      }
      copy_edge_arguments(&assembler, function, terminator.true_edge,
                          temporary_base);
      fixups.push_back({assembler.branch(), terminator.true_edge.target});
    }
  }

  for (const BranchFixup& fixup : fixups) {
    if (!fixup.target.valid() || fixup.target.id() >= labels.size() ||
        labels[fixup.target.id()] == no_label) {
      return {{StatusCode::kCodeGenerationFailed,
               "AArch64 CFG branch has no bound target"},
              {},
              0};
    }
    const Status patch_status =
        assembler.patch_branch(fixup.offset, labels[fixup.target.id()], false);
    if (!patch_status.ok()) {
      return {patch_status, {}, 0};
    }
  }
  return {Status::ok_status(), assembler.take_code(), spill_slots};
}

}  // namespace

LoweringResult lower(const ir::Function& function) {
  try {
    return lower_impl(function);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate AArch64 lowering state"},
            {},
            0};
  }
}

LoweringResult lower(const ir::ControlFlowFunction& function) {
  try {
    return lower_control_flow_impl(function);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate AArch64 CFG lowering state"},
            {},
            0};
  }
}

}  // namespace unijit::jit::detail::aarch64
