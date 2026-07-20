#include "jit/backend/x86_64/lower.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "jit/code_buffer.h"
#include "jit/register_allocator.h"

namespace unijit::jit::detail::x86_64 {
namespace {

constexpr int kRax = 0;
constexpr int kRcx = 1;
constexpr int kRdx = 2;
constexpr int kRsp = 4;
constexpr int kRdi = 7;
constexpr int kR8 = 8;
constexpr int kR9 = 9;
constexpr int kR10 = 10;
constexpr int kR11 = 11;

#if defined(_WIN32)
constexpr int kArgumentRegister = kRcx;
#else
constexpr int kArgumentRegister = kRdi;
#endif

constexpr int kReturnRegister = kRax;
constexpr int kArgumentBaseRegister = kR11;
constexpr int kScratch0 = kRax;
constexpr int kScratch1 = kR10;
constexpr std::array<int, 4> kAllocationRegisters = {kRcx, kRdx, kR8, kR9};
constexpr std::size_t kMaximumStackSize = 1024U * 1024U;
constexpr std::size_t kMaximumOffset =
    static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

class Assembler final {
 public:
  void move_register(int destination, int source) {
    emit_rex(destination, source);
    buffer_.emit_u8(0x8BU);
    emit_modrm(3, destination, source);
  }

  void move_immediate(int destination, ir::Word immediate) {
    emit_rex(0, destination);
    buffer_.emit_u8(static_cast<std::uint8_t>(0xB8U + (destination & 7)));
    buffer_.emit_u64(static_cast<std::uint64_t>(immediate));
  }

  void load(int destination, int base, std::size_t byte_offset) {
    emit_rex(destination, base);
    buffer_.emit_u8(0x8BU);
    emit_memory_modrm(destination, base);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_offset));
  }

  void store(int source, int base, std::size_t byte_offset) {
    emit_rex(source, base);
    buffer_.emit_u8(0x89U);
    emit_memory_modrm(source, base);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_offset));
  }

  void add(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(rhs, destination);
    buffer_.emit_u8(0x01U);
    emit_modrm(3, rhs, destination);
  }

  void subtract(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(rhs, destination);
    buffer_.emit_u8(0x29U);
    emit_modrm(3, rhs, destination);
  }

  void multiply(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(destination, rhs);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0xAFU);
    emit_modrm(3, destination, rhs);
  }

  void compare(int destination, int lhs, int rhs, bool or_equal) {
    emit_rex(rhs, lhs);
    buffer_.emit_u8(0x39U);
    emit_modrm(3, rhs, lhs);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(or_equal ? 0x9EU : 0x9CU);
    emit_modrm(3, 0, destination);
    emit_rex(destination, destination);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0xB6U);
    emit_modrm(3, destination, destination);
  }

  std::size_t branch() {
    buffer_.emit_u8(0xE9U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  std::size_t branch_nonzero(int source) {
    emit_rex(source, source);
    buffer_.emit_u8(0x85U);
    emit_modrm(3, source, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x85U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  Status patch_branch(std::size_t displacement, std::size_t target) {
    const std::int64_t delta = static_cast<std::int64_t>(target) -
                               static_cast<std::int64_t>(displacement + 4U);
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
      return {StatusCode::kResourceExhausted,
              "x86-64 CFG branch exceeds its encoding range"};
    }
    buffer_.patch_u32(displacement, static_cast<std::uint32_t>(delta));
    return Status::ok_status();
  }

  void reserve_stack(std::size_t byte_count) {
    buffer_.emit_u8(0x48U);
    buffer_.emit_u8(0x81U);
    buffer_.emit_u8(0xECU);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_count));
  }

  void release_stack(std::size_t byte_count) {
    buffer_.emit_u8(0x48U);
    buffer_.emit_u8(0x81U);
    buffer_.emit_u8(0xC4U);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_count));
  }

  void return_to_caller() { buffer_.emit_u8(0xC3U); }

  std::size_t size() const noexcept { return buffer_.size(); }

  std::vector<std::uint8_t> take_code() noexcept {
    return buffer_.take_bytes();
  }

 private:
  void prepare_binary(int destination, int lhs) {
    if (destination != lhs) {
      move_register(destination, lhs);
    }
  }

  void emit_rex(int reg_field, int rm_field) {
    buffer_.emit_u8(static_cast<std::uint8_t>(0x48U | ((reg_field >> 3) << 2) |
                                              (rm_field >> 3)));
  }

  void emit_modrm(int mode, int reg_field, int rm_field) {
    buffer_.emit_u8(static_cast<std::uint8_t>(
        (mode << 6) | ((reg_field & 7) << 3) | (rm_field & 7)));
  }

  void emit_memory_modrm(int reg_field, int base) {
    emit_modrm(2, reg_field, base);
    if ((base & 7) == 4) {
      buffer_.emit_u8(static_cast<std::uint8_t>(0x20U | (base & 7)));
    }
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
  assembler->load(scratch, kRsp, spill_offset(location));
  return scratch;
}

LoweringResult lower_impl(const ir::Function& function) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the x86-64 encoder requires a little-endian target"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumOffset / sizeof(ir::Word)) {
    return {{StatusCode::kResourceExhausted,
             "x86-64 parameter area exceeds direct-load addressing"},
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
  assembler.move_register(kArgumentBaseRegister, kArgumentRegister);
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
          assembler.store(target, kRsp, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kConstant: {
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, node.immediate);
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
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
          assembler.store(target, kRsp, spill_offset(destination));
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
    assembler.load(kReturnRegister, kRsp, spill_offset(returned));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  return {Status::ok_status(), assembler.take_code(), allocation.spill_slots};
}

struct BranchFixup final {
  std::size_t displacement{0};
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
    assembler->load(kScratch0, kRsp,
                    control_spill_offset(edge.arguments[index].id()));
    assembler->store(kScratch0, kRsp,
                     control_spill_offset(temporary_base + index));
  }
  const ir::BasicBlock& target = function.blocks()[edge.target.id()];
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    assembler->load(kScratch0, kRsp,
                    control_spill_offset(temporary_base + index));
    assembler->store(kScratch0, kRsp,
                     control_spill_offset(target.parameters[index].id()));
  }
}

LoweringResult lower_control_flow_impl(
    const ir::ControlFlowFunction& function) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the x86-64 encoder requires a little-endian target"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumOffset / sizeof(ir::Word)) {
    return {{StatusCode::kResourceExhausted,
             "x86-64 parameter area exceeds direct-load addressing"},
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
             "x86-64 CFG spill frame exceeds the backend limit"},
            {},
            0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kArgumentRegister);
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
          assembler.store(kScratch0, kRsp, destination_offset);
          break;
        case ir::ControlOpcode::kBlockParameter:
          break;
        case ir::ControlOpcode::kConstant:
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.store(kScratch0, kRsp, destination_offset);
          break;
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          assembler.load(kScratch0, kRsp, control_spill_offset(node.lhs.id()));
          assembler.load(kScratch1, kRsp, control_spill_offset(node.rhs.id()));
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
          assembler.store(kScratch0, kRsp, destination_offset);
          break;
      }
    }

    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kReturn) {
      assembler.load(kReturnRegister, kRsp,
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
      assembler.load(kScratch0, kRsp,
                     control_spill_offset(terminator.value.id()));
      const std::size_t true_selector = assembler.branch_nonzero(kScratch0);
      copy_edge_arguments(&assembler, function, terminator.false_edge,
                          temporary_base);
      fixups.push_back({assembler.branch(), terminator.false_edge.target});
      const std::size_t true_copy = assembler.size();
      const Status selector_status =
          assembler.patch_branch(true_selector, true_copy);
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
               "x86-64 CFG branch has no bound target"},
              {},
              0};
    }
    const Status patch_status =
        assembler.patch_branch(fixup.displacement, labels[fixup.target.id()]);
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
             "unable to allocate x86-64 lowering state"},
            {},
            0};
  }
}

LoweringResult lower(const ir::ControlFlowFunction& function) {
  try {
    return lower_control_flow_impl(function);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate x86-64 CFG lowering state"},
            {},
            0};
  }
}

}  // namespace unijit::jit::detail::x86_64
