#include "jit/backend/riscv64/lower.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "jit/code_buffer.h"
#include "jit/register_allocator.h"

namespace unijit::jit::detail::riscv64 {
namespace {

constexpr int kZero = 0;
constexpr int kReturnAddress = 1;
constexpr int kStackPointer = 2;
constexpr int kArgumentAndReturn = 10;
constexpr int kArgumentBase = 5;
constexpr int kScratch0 = 6;
constexpr int kScratch1 = 7;
constexpr std::array<int, 8> kAllocationRegisters = {11, 12, 13, 14,
                                                     15, 16, 17, 28};
constexpr std::size_t kMaximumStackSize = 2032;
constexpr std::size_t kMaximumAddressableParameters = 256;

struct LiteralUse final {
  std::size_t instruction_offset{0};
  int destination{0};
  std::uint64_t value{0};
};

class Assembler final {
 public:
  void move_register(int destination, int source) {
    emit_i(0, source, 0, destination, 0x13);
  }

  void move_immediate(int destination, ir::Word immediate) {
    literals_.push_back(
        LiteralUse{buffer_.size(), destination,
                   static_cast<std::uint64_t>(immediate)});
    buffer_.emit_u32(0);
    buffer_.emit_u32(0);
  }

  void load(int destination, int base, std::size_t byte_offset) {
    emit_i(static_cast<std::int32_t>(byte_offset), base, 3, destination, 0x03);
  }

  void store(int source, int base, std::size_t byte_offset) {
    emit_s(static_cast<std::int32_t>(byte_offset), source, base, 3, 0x23);
  }

  void add(int destination, int lhs, int rhs) {
    emit_r(0x00, rhs, lhs, 0, destination, 0x33);
  }

  void subtract(int destination, int lhs, int rhs) {
    emit_r(0x20, rhs, lhs, 0, destination, 0x33);
  }

  void multiply(int destination, int lhs, int rhs) {
    emit_r(0x01, rhs, lhs, 0, destination, 0x33);
  }

  void reserve_stack(std::size_t byte_count) {
    emit_i(-static_cast<std::int32_t>(byte_count), kStackPointer, 0,
           kStackPointer, 0x13);
  }

  void release_stack(std::size_t byte_count) {
    emit_i(static_cast<std::int32_t>(byte_count), kStackPointer, 0,
           kStackPointer, 0x13);
  }

  void return_to_caller() {
    emit_i(0, kReturnAddress, 0, kZero, 0x67);
  }

  Status finalize_literals() {
    if ((buffer_.size() & 7U) != 0) {
      emit_i(0, kZero, 0, kZero, 0x13);
    }
    for (const LiteralUse& literal : literals_) {
      const std::size_t literal_offset = buffer_.size();
      if (literal_offset >
          literal.instruction_offset +
              static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return {StatusCode::kResourceExhausted,
                "RISC-V literal pool exceeds PC-relative addressing"};
      }
      const auto delta = static_cast<std::int64_t>(literal_offset -
                                                   literal.instruction_offset);
      const std::int64_t high = (delta + 0x800) >> 12;
      const std::int64_t low = delta - (high << 12);
      buffer_.patch_u32(literal.instruction_offset,
                        encode_u(static_cast<std::int32_t>(high),
                                 literal.destination, 0x17));
      buffer_.patch_u32(
          literal.instruction_offset + 4,
          encode_i(static_cast<std::int32_t>(low), literal.destination, 3,
                   literal.destination, 0x03));
      buffer_.emit_u64(literal.value);
    }
    return Status::ok_status();
  }

  std::vector<std::uint8_t> take_code() noexcept {
    return buffer_.take_bytes();
  }

 private:
  static std::uint32_t encode_i(std::int32_t immediate, int source,
                                int function, int destination, int opcode) {
    return ((static_cast<std::uint32_t>(immediate) & 0xFFFU) << 20U) |
           (reg(source) << 15U) | (reg(function) << 12U) |
           (reg(destination) << 7U) | reg(opcode);
  }

  static std::uint32_t encode_u(std::int32_t immediate, int destination,
                                int opcode) {
    return ((static_cast<std::uint32_t>(immediate) & 0xFFFFFU) << 12U) |
           (reg(destination) << 7U) | reg(opcode);
  }

  void emit_i(std::int32_t immediate, int source, int function,
              int destination, int opcode) {
    buffer_.emit_u32(
        encode_i(immediate, source, function, destination, opcode));
  }

  void emit_s(std::int32_t immediate, int source, int base, int function,
              int opcode) {
    const std::uint32_t bits = static_cast<std::uint32_t>(immediate);
    buffer_.emit_u32(((bits >> 5U) & 0x7FU) << 25U |
                     (reg(source) << 20U) | (reg(base) << 15U) |
                     (reg(function) << 12U) | ((bits & 0x1FU) << 7U) |
                     reg(opcode));
  }

  void emit_r(int function7, int rhs, int lhs, int function3,
              int destination, int opcode) {
    buffer_.emit_u32((reg(function7) << 25U) | (reg(rhs) << 20U) |
                     (reg(lhs) << 15U) | (reg(function3) << 12U) |
                     (reg(destination) << 7U) | reg(opcode));
  }

  static std::uint32_t reg(int value) noexcept {
    return static_cast<std::uint32_t>(value);
  }

  CodeBuffer buffer_;
  std::vector<LiteralUse> literals_;
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
           "the RISC-V 64 encoder currently supports little-endian targets"},
          {}, 0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "RISC-V parameter area exceeds signed 12-bit addressing"},
            {}, 0};
  }

  RegisterAllocation allocation = allocate_linear_scan(
      function, kAllocationRegisters.size(),
      kMaximumStackSize / sizeof(ir::Word));
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBase, kArgumentAndReturn);
  const std::size_t raw_stack_size =
      allocation.spill_slots * sizeof(ir::Word);
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
        assembler.load(target, kArgumentBase,
                       static_cast<std::size_t>(node.immediate) *
                           sizeof(ir::Word));
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
        const int lhs = load_operand(&assembler,
                                     allocation.locations[node.lhs.id()],
                                     kScratch0);
        const int rhs = load_operand(&assembler,
                                     allocation.locations[node.rhs.id()],
                                     kScratch1);
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
    assembler.move_register(kArgumentAndReturn, physical_register(returned));
  } else {
    assembler.load(kArgumentAndReturn, kStackPointer, spill_offset(returned));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  const Status literals = assembler.finalize_literals();
  if (!literals.ok()) {
    return {literals, {}, 0};
  }
  return {Status::ok_status(), assembler.take_code(), allocation.spill_slots};
}

}  // namespace

LoweringResult lower(const ir::Function& function) {
  try {
    return lower_impl(function);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate RISC-V 64 lowering state"},
            {}, 0};
  }
}

}  // namespace unijit::jit::detail::riscv64
