#include "jit/backend/riscv64/lower.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "jit/code_buffer.h"
#include "jit/register_allocator.h"
#include "unijit/runtime/execution_context.h"

namespace unijit::jit::detail::riscv64 {
namespace {

constexpr int kZero = 0;
constexpr int kReturnAddress = 1;
constexpr int kStackPointer = 2;
constexpr int kArgumentAndReturn = 10;
constexpr int kContextArgument = 11;
constexpr int kArgumentBase = 5;
constexpr int kScratch0 = 6;
constexpr int kScratch1 = 7;
constexpr int kFloatScratch0 = 0;
constexpr int kFloatScratch1 = 1;
constexpr std::array<int, 8> kAllocationRegisters = {11, 12, 13, 14,
                                                     15, 16, 17, 28};
constexpr std::array<int, 8> kFloatAllocationRegisters = {10, 11, 12, 13,
                                                          14, 15, 16, 17};
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
    literals_.push_back(LiteralUse{buffer_.size(), destination,
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

  void move_word_to_float(int destination, int source) {
    emit_r(0x79, 0, source, 0, destination, 0x53);
  }

  void move_float_to_word(int destination, int source) {
    emit_r(0x71, 0, source, 0, destination, 0x53);
  }

  void move_float_register(int destination, int source) {
    emit_r(0x11, source, source, 0, destination, 0x53);
  }

  void load_float(int destination, int base, std::size_t byte_offset) {
    emit_i(static_cast<std::int32_t>(byte_offset), base, 3, destination, 0x07);
  }

  void store_float(int source, int base, std::size_t byte_offset) {
    emit_s(static_cast<std::int32_t>(byte_offset), source, base, 3, 0x27);
  }

  void add_float(int destination, int lhs, int rhs) {
    emit_r(0x01, rhs, lhs, 0, destination, 0x53);
  }

  void subtract_float(int destination, int lhs, int rhs) {
    emit_r(0x05, rhs, lhs, 0, destination, 0x53);
  }

  void multiply_float(int destination, int lhs, int rhs) {
    emit_r(0x09, rhs, lhs, 0, destination, 0x53);
  }

  void divide_float(int destination, int lhs, int rhs) {
    emit_r(0x0D, rhs, lhs, 0, destination, 0x53);
  }

  void compare_float(int destination, int lhs, int rhs, bool or_equal) {
    emit_r(0x51, rhs, lhs, or_equal ? 0 : 1, destination, 0x53);
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

  void compare(int destination, int lhs, int rhs, bool or_equal) {
    if (or_equal) {
      emit_r(0x00, lhs, rhs, 2, destination, 0x33);
      emit_i(1, destination, 4, destination, 0x13);
    } else {
      emit_r(0x00, rhs, lhs, 2, destination, 0x33);
    }
  }

  void address(int destination, int base, std::size_t byte_offset) {
    emit_i(static_cast<std::int32_t>(byte_offset), base, 0, destination, 0x13);
  }

  void call_register(int target) {
    emit_i(0, target, 0, kReturnAddress, 0x67);
  }

  std::size_t branch() {
    const std::size_t offset = buffer_.size();
    buffer_.emit_u32(0x0000006FU);
    return offset;
  }

  std::size_t branch_nonzero(int source) {
    const std::size_t offset = buffer_.size();
    emit_b(0, kZero, source, 1, 0x63);
    return offset;
  }

  std::size_t branch_zero(int source) {
    const std::size_t offset = buffer_.size();
    emit_b(0, kZero, source, 0, 0x63);
    return offset;
  }

  Status patch_branch(std::size_t offset, std::size_t target,
                      bool conditional) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(offset);
    if ((delta & 1) != 0) {
      return {StatusCode::kCodeGenerationFailed,
              "RISC-V branch target is not halfword aligned"};
    }
    if (conditional) {
      if (delta < -4096 || delta > 4094) {
        return {StatusCode::kResourceExhausted,
                "RISC-V conditional branch exceeds its encoding range"};
      }
      buffer_.patch_u32(offset, encode_b(static_cast<std::int32_t>(delta),
                                         kZero, kScratch0, 1, 0x63));
    } else {
      if (delta < -(std::int64_t{1} << 20) ||
          delta > (std::int64_t{1} << 20) - 2) {
        return {StatusCode::kResourceExhausted,
                "RISC-V jump exceeds its encoding range"};
      }
      buffer_.patch_u32(
          offset, encode_j(static_cast<std::int32_t>(delta), kZero, 0x6F));
    }
    return Status::ok_status();
  }

  Status patch_zero_branch(std::size_t offset, std::size_t target) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(offset);
    if ((delta & 1) != 0) {
      return {StatusCode::kCodeGenerationFailed,
              "RISC-V safepoint branch target is not halfword aligned"};
    }
    if (delta < -4096 || delta > 4094) {
      return {StatusCode::kResourceExhausted,
              "RISC-V safepoint branch exceeds its encoding range"};
    }
    buffer_.patch_u32(offset, encode_b(static_cast<std::int32_t>(delta),
                                       kZero, kScratch0, 0, 0x63));
    return Status::ok_status();
  }

  void reserve_stack(std::size_t byte_count) {
    emit_i(-static_cast<std::int32_t>(byte_count), kStackPointer, 0,
           kStackPointer, 0x13);
  }

  void release_stack(std::size_t byte_count) {
    emit_i(static_cast<std::int32_t>(byte_count), kStackPointer, 0,
           kStackPointer, 0x13);
  }

  void return_to_caller() { emit_i(0, kReturnAddress, 0, kZero, 0x67); }

  std::size_t size() const noexcept { return buffer_.size(); }

  Status finalize_literals() {
    if ((buffer_.size() & 7U) != 0) {
      emit_i(0, kZero, 0, kZero, 0x13);
    }
    for (const LiteralUse& literal : literals_) {
      const std::size_t literal_offset = buffer_.size();
      if (literal_offset > literal.instruction_offset +
                               static_cast<std::size_t>(
                                   std::numeric_limits<std::int32_t>::max())) {
        return {StatusCode::kResourceExhausted,
                "RISC-V literal pool exceeds PC-relative addressing"};
      }
      const auto delta = static_cast<std::int64_t>(literal_offset -
                                                   literal.instruction_offset);
      const std::int64_t high = (delta + 0x800) >> 12;
      const std::int64_t low = delta - (high << 12);
      buffer_.patch_u32(
          literal.instruction_offset,
          encode_u(static_cast<std::int32_t>(high), literal.destination, 0x17));
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

  static std::uint32_t encode_b(std::int32_t immediate, int rhs, int lhs,
                                int function, int opcode) {
    const std::uint32_t bits = static_cast<std::uint32_t>(immediate);
    return (((bits >> 12U) & 0x1U) << 31U) | (((bits >> 5U) & 0x3FU) << 25U) |
           (reg(rhs) << 20U) | (reg(lhs) << 15U) | (reg(function) << 12U) |
           (((bits >> 1U) & 0xFU) << 8U) | (((bits >> 11U) & 0x1U) << 7U) |
           reg(opcode);
  }

  static std::uint32_t encode_j(std::int32_t immediate, int destination,
                                int opcode) {
    const std::uint32_t bits = static_cast<std::uint32_t>(immediate);
    return (((bits >> 20U) & 0x1U) << 31U) | (((bits >> 1U) & 0x3FFU) << 21U) |
           (((bits >> 11U) & 0x1U) << 20U) | (((bits >> 12U) & 0xFFU) << 12U) |
           (reg(destination) << 7U) | reg(opcode);
  }

  void emit_i(std::int32_t immediate, int source, int function, int destination,
              int opcode) {
    buffer_.emit_u32(
        encode_i(immediate, source, function, destination, opcode));
  }

  void emit_s(std::int32_t immediate, int source, int base, int function,
              int opcode) {
    const std::uint32_t bits = static_cast<std::uint32_t>(immediate);
    buffer_.emit_u32(((bits >> 5U) & 0x7FU) << 25U | (reg(source) << 20U) |
                     (reg(base) << 15U) | (reg(function) << 12U) |
                     ((bits & 0x1FU) << 7U) | reg(opcode));
  }

  void emit_b(std::int32_t immediate, int rhs, int lhs, int function,
              int opcode) {
    buffer_.emit_u32(encode_b(immediate, rhs, lhs, function, opcode));
  }

  void emit_r(int function7, int rhs, int lhs, int function3, int destination,
              int opcode) {
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

int physical_float_register(const ValueLocation& location) noexcept {
  return kFloatAllocationRegisters[location.register_index];
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

int load_float_operand(Assembler* assembler, const ValueLocation& location,
                       int scratch) {
  if (location.in_register()) {
    return physical_float_register(location);
  }
  assembler->load_float(scratch, kStackPointer, spill_offset(location));
  return scratch;
}

void save_live_across_call(Assembler* assembler,
                           const ir::Function& function,
                           const RegisterAllocation& allocation,
                           std::size_t call_index) {
  for (std::size_t value_index = 0; value_index < call_index; ++value_index) {
    const ValueLocation& location = allocation.locations[value_index];
    if (!location.in_register() ||
        allocation.last_uses[value_index] <= call_index) {
      continue;
    }
    if (function.nodes()[value_index].type == ir::ValueType::kFloat64) {
      assembler->store_float(physical_float_register(location), kStackPointer,
                             spill_offset(location));
    } else {
      assembler->store(physical_register(location), kStackPointer,
                       spill_offset(location));
    }
  }
}

void restore_live_across_call(Assembler* assembler,
                              const ir::Function& function,
                              const RegisterAllocation& allocation,
                              std::size_t call_index) {
  for (std::size_t value_index = 0; value_index < call_index; ++value_index) {
    const ValueLocation& location = allocation.locations[value_index];
    if (!location.in_register() ||
        allocation.last_uses[value_index] <= call_index) {
      continue;
    }
    if (function.nodes()[value_index].type == ir::ValueType::kFloat64) {
      assembler->load_float(physical_float_register(location), kStackPointer,
                            spill_offset(location));
    } else {
      assembler->load(physical_register(location), kStackPointer,
                      spill_offset(location));
    }
  }
}

void spill_straight_stack_map_values(
    Assembler* assembler, const ir::Function& function,
    const RegisterAllocation& allocation,
    const std::vector<ir::Value>& live_values,
    std::size_t stack_map_base) {
  for (const ir::Value value : live_values) {
    const ValueLocation& location = allocation.locations[value.id()];
    const std::size_t offset =
        (stack_map_base + value.id()) * sizeof(ir::Word);
    if (function.value_type(value) == ir::ValueType::kFloat64) {
      const int source =
          load_float_operand(assembler, location, kFloatScratch0);
      assembler->store_float(source, kStackPointer, offset);
    } else {
      const int source = load_operand(assembler, location, kScratch0);
      assembler->store(source, kStackPointer, offset);
    }
  }
}

void capture_straight_stack_map_values(
    Assembler* assembler, const std::vector<ir::Value>& live_values,
    std::size_t stack_map_base, int context) {
  for (std::size_t index = 0; index < live_values.size(); ++index) {
    assembler->load(kScratch1, kStackPointer,
                    (stack_map_base + live_values[index].id()) *
                        sizeof(ir::Word));
    assembler->store(
        kScratch1, context,
        runtime::ExecutionContext::captured_values_offset() +
            index * sizeof(ir::Word));
  }
  assembler->move_immediate(kScratch1,
                            static_cast<ir::Word>(live_values.size()));
  assembler->store(
      kScratch1, context,
      runtime::ExecutionContext::captured_value_count_offset());
}

StackMapRecord make_stack_map_record(
    const ir::Function& function, const ir::Node& node,
    const std::vector<ir::Value>& live_values, std::size_t native_offset,
    std::size_t frame_size, std::size_t stack_map_base) {
  StackMapRecord record;
  record.site = static_cast<std::size_t>(node.immediate);
  record.native_offset = native_offset;
  record.frame_size = frame_size;
  record.kind = node.opcode == ir::Opcode::kSafepoint
                    ? StackMapKind::kSafepoint
                    : StackMapKind::kGuard;
  record.live_values.reserve(live_values.size());
  for (const ir::Value value : live_values) {
    record.live_values.push_back(
        {value, function.value_type(value),
         (stack_map_base + value.id()) * sizeof(ir::Word)});
  }
  return record;
}

LoweringResult lower_impl(const ir::Function& function,
                          const StackMapRequirements& requirements) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the RISC-V 64 encoder currently supports little-endian targets"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "RISC-V parameter area exceeds signed 12-bit addressing"},
            {},
            0};
  }

  RegisterAllocation allocation =
      allocate_linear_scan(function, kAllocationRegisters.size(),
                           kMaximumStackSize / sizeof(ir::Word), requirements);
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0, {}};
  }
  StackMapLiveness stack_map_liveness =
      plan_stack_map_liveness(function, allocation);
  if (!stack_map_liveness.status.ok()) {
    return {stack_map_liveness.status, {}, 0, {}};
  }

  std::size_t maximum_call_arguments = 0;
  bool has_calls = false;
  bool has_context_operations = false;
  for (const ir::Node& node : function.nodes()) {
    if (node.opcode == ir::Opcode::kCall) {
      has_calls = true;
      maximum_call_arguments =
          std::max(maximum_call_arguments,
                   static_cast<std::size_t>(node.argument_count));
    } else if (node.opcode == ir::Opcode::kSafepoint ||
               node.opcode == ir::Opcode::kGuardFloatNonzero) {
      has_context_operations = true;
    }
  }
  const std::size_t call_argument_base = allocation.spill_slots;
  const std::size_t return_address_slot =
      call_argument_base + maximum_call_arguments;
  const std::size_t context_slot =
      return_address_slot + static_cast<std::size_t>(has_calls);
  const std::size_t stack_map_base =
      context_slot + static_cast<std::size_t>(has_context_operations);
  const std::size_t total_slots =
      stack_map_base +
      (has_context_operations ? function.nodes().size() : 0);
  if (total_slots > kMaximumStackSize / sizeof(ir::Word)) {
    return {{StatusCode::kResourceExhausted,
             "RISC-V runtime-call frame exceeds the backend limit"},
            {},
            0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBase, kArgumentAndReturn);
  const std::size_t raw_stack_size = total_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }
  if (has_calls) {
    assembler.store(kReturnAddress, kStackPointer,
                    return_address_slot * sizeof(ir::Word));
  }
  if (has_context_operations) {
    assembler.store(kContextArgument, kStackPointer,
                    context_slot * sizeof(ir::Word));
  }

  std::vector<StackMapRecord> stack_maps;

  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::Node& node = function.nodes()[index];
    const ValueLocation& destination = allocation.locations[index];
    const std::vector<ir::Value>& live_values =
        stack_map_liveness.live_values_by_node[index];
    if (node.opcode == ir::Opcode::kSafepoint ||
        node.opcode == ir::Opcode::kGuardFloatNonzero) {
      spill_straight_stack_map_values(&assembler, function, allocation,
                                      live_values, stack_map_base);
      stack_maps.push_back(make_stack_map_record(
          function, node, live_values, assembler.size(), stack_size,
          stack_map_base));
    }
    switch (node.opcode) {
      case ir::Opcode::kParameter: {
        if (node.type == ir::ValueType::kFloat64) {
          const int target = destination.in_register()
                                 ? physical_float_register(destination)
                                 : kFloatScratch0;
          assembler.load_float(
              target, kArgumentBase,
              static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
          if (!destination.in_register()) {
            assembler.store_float(target, kStackPointer,
                                  spill_offset(destination));
          }
          break;
        }
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.load(
            target, kArgumentBase,
            static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kConstant: {
        if (node.type == ir::ValueType::kFloat64) {
          const int target = destination.in_register()
                                 ? physical_float_register(destination)
                                 : kFloatScratch0;
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.move_word_to_float(target, kScratch0);
          if (!destination.in_register()) {
            assembler.store_float(target, kStackPointer,
                                  spill_offset(destination));
          }
          break;
        }
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
      case ir::Opcode::kFloatAdd:
      case ir::Opcode::kFloatSubtract:
      case ir::Opcode::kFloatMultiply:
      case ir::Opcode::kFloatDivide: {
        const int lhs = load_float_operand(
            &assembler, allocation.locations[node.lhs.id()], kFloatScratch0);
        const int rhs = load_float_operand(
            &assembler, allocation.locations[node.rhs.id()], kFloatScratch1);
        const int target = destination.in_register()
                               ? physical_float_register(destination)
                               : kFloatScratch0;
        if (node.opcode == ir::Opcode::kFloatAdd) {
          assembler.add_float(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kFloatSubtract) {
          assembler.subtract_float(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kFloatMultiply) {
          assembler.multiply_float(target, lhs, rhs);
        } else {
          assembler.divide_float(target, lhs, rhs);
        }
        if (!destination.in_register()) {
          assembler.store_float(target, kStackPointer,
                                spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kFloatLessThan:
      case ir::Opcode::kFloatLessEqual: {
        const int lhs = load_float_operand(
            &assembler, allocation.locations[node.lhs.id()], kFloatScratch0);
        const int rhs = load_float_operand(
            &assembler, allocation.locations[node.rhs.id()], kFloatScratch1);
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.compare_float(target, lhs, rhs,
                                node.opcode == ir::Opcode::kFloatLessEqual);
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kCall: {
        save_live_across_call(&assembler, function, allocation, index);
        for (std::size_t argument_index = 0;
             argument_index < node.argument_count; ++argument_index) {
          const ir::Value argument = function.call_arguments()[
              static_cast<std::size_t>(node.argument_begin) + argument_index];
          const ValueLocation& source = allocation.locations[argument.id()];
          const std::size_t argument_offset =
              (call_argument_base + argument_index) * sizeof(ir::Word);
          if (function.value_type(argument) == ir::ValueType::kFloat64) {
            const int source_register = load_float_operand(
                &assembler, source, kFloatScratch0);
            assembler.store_float(source_register, kStackPointer,
                                  argument_offset);
          } else {
            const int source_register =
                load_operand(&assembler, source, kScratch0);
            assembler.store(source_register, kStackPointer, argument_offset);
          }
        }
        assembler.address(kArgumentAndReturn, kStackPointer,
                          call_argument_base * sizeof(ir::Word));
        assembler.move_immediate(
            11, static_cast<ir::Word>(node.argument_count));
        assembler.move_immediate(kScratch0, node.immediate);
        assembler.call_register(kScratch0);
        if (node.type == ir::ValueType::kFloat64) {
          if (destination.in_register()) {
            assembler.move_word_to_float(physical_float_register(destination),
                                         kArgumentAndReturn);
          } else {
            assembler.store(kArgumentAndReturn, kStackPointer,
                            spill_offset(destination));
          }
        } else if (destination.in_register()) {
          assembler.move_register(physical_register(destination),
                                  kArgumentAndReturn);
        } else {
          assembler.store(kArgumentAndReturn, kStackPointer,
                          spill_offset(destination));
        }
        restore_live_across_call(&assembler, function, allocation, index);
        break;
      }
      case ir::Opcode::kGuardFloatNonzero: {
        const int source = load_float_operand(
            &assembler, allocation.locations[node.lhs.id()], kFloatScratch0);
        assembler.move_float_to_word(kScratch0, source);
        assembler.move_register(kScratch1, kScratch0);
        assembler.add(kScratch0, kScratch0, kScratch0);
        const std::size_t nonzero = assembler.branch_nonzero(kScratch0);

        assembler.load(kScratch0, kStackPointer,
                       context_slot * sizeof(ir::Word));
        const std::size_t no_context = assembler.branch_zero(kScratch0);
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_value_offset());
        capture_straight_stack_map_values(
            &assembler, live_values, stack_map_base, kScratch0);
        assembler.move_immediate(kScratch1, node.immediate);
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_site_offset());
        assembler.move_immediate(
            kScratch1,
            static_cast<ir::Word>(runtime::ExitReason::kRuntime));
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_reason_offset());

        const std::size_t exit = assembler.size();
        assembler.move_immediate(kArgumentAndReturn, 0);
        if (has_calls) {
          assembler.load(kReturnAddress, kStackPointer,
                         return_address_slot * sizeof(ir::Word));
        }
        if (stack_size != 0) {
          assembler.release_stack(stack_size);
        }
        assembler.return_to_caller();

        const std::size_t resume = assembler.size();
        const Status context_status =
            assembler.patch_zero_branch(no_context, exit);
        if (!context_status.ok()) {
          return {context_status, {}, 0};
        }
        const Status guard_status =
            assembler.patch_branch(nonzero, resume, true);
        if (!guard_status.ok()) {
          return {guard_status, {}, 0};
        }
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, 0);
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kSafepoint: {
        assembler.load(kScratch0, kStackPointer,
                       context_slot * sizeof(ir::Word));
        const std::size_t no_context = assembler.branch_zero(kScratch0);
        assembler.load(
            kScratch0, kScratch0,
            runtime::ExecutionContext::interrupt_requested_offset());
        const std::size_t not_interrupted = assembler.branch_zero(kScratch0);
        assembler.load(kScratch0, kStackPointer,
                       context_slot * sizeof(ir::Word));
        assembler.move_immediate(kScratch1, 0);
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_value_offset());
        capture_straight_stack_map_values(
            &assembler, live_values, stack_map_base, kScratch0);
        assembler.move_immediate(kScratch1, node.immediate);
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_site_offset());
        assembler.move_immediate(
            kScratch1,
            static_cast<ir::Word>(runtime::ExitReason::kSafepoint));
        assembler.store(kScratch1, kScratch0,
                        runtime::ExecutionContext::exit_reason_offset());
        assembler.move_immediate(kArgumentAndReturn, 0);
        if (has_calls) {
          assembler.load(kReturnAddress, kStackPointer,
                         return_address_slot * sizeof(ir::Word));
        }
        if (stack_size != 0) {
          assembler.release_stack(stack_size);
        }
        assembler.return_to_caller();

        const std::size_t resume = assembler.size();
        const Status context_status =
            assembler.patch_zero_branch(no_context, resume);
        if (!context_status.ok()) {
          return {context_status, {}, 0};
        }
        const Status interrupt_status =
            assembler.patch_zero_branch(not_interrupted, resume);
        if (!interrupt_status.ok()) {
          return {interrupt_status, {}, 0};
        }
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, 0);
        if (!destination.in_register()) {
          assembler.store(target, kStackPointer, spill_offset(destination));
        }
        break;
      }
    }
  }

  const ValueLocation& returned =
      allocation.locations[function.return_value().id()];
  if (function.return_type() == ir::ValueType::kFloat64) {
    if (returned.in_register()) {
      assembler.move_float_to_word(kArgumentAndReturn,
                                   physical_float_register(returned));
    } else {
      assembler.load(kArgumentAndReturn, kStackPointer,
                     spill_offset(returned));
    }
  } else if (returned.in_register()) {
    assembler.move_register(kArgumentAndReturn, physical_register(returned));
  } else {
    assembler.load(kArgumentAndReturn, kStackPointer, spill_offset(returned));
  }
  if (has_calls) {
    assembler.load(kReturnAddress, kStackPointer,
                   return_address_slot * sizeof(ir::Word));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  const Status literals = assembler.finalize_literals();
  if (!literals.ok()) {
    return {literals, {}, 0};
  }
  return {Status::ok_status(), assembler.take_code(), total_slots,
          std::move(stack_maps)};
}

struct BranchFixup final {
  std::size_t offset{0};
  ir::Block target;
};

std::size_t control_spill_offset(std::size_t slot) noexcept {
  return slot * sizeof(ir::Word);
}

int control_word_register(
    const ControlFlowRegisterAllocation& allocation, ir::Value value,
    std::size_t current_block) noexcept {
  if (allocation.owner_blocks[value.id()] != current_block ||
      allocation.register_indices[value.id()] == ValueLocation::kNone) {
    return -1;
  }
  return kAllocationRegisters[allocation.register_indices[value.id()]];
}

int control_float_register(
    const ControlFlowRegisterAllocation& allocation, ir::Value value,
    std::size_t current_block) noexcept {
  if (allocation.owner_blocks[value.id()] != current_block ||
      allocation.register_indices[value.id()] == ValueLocation::kNone) {
    return -1;
  }
  return kFloatAllocationRegisters[allocation.register_indices[value.id()]];
}

int load_control_word(Assembler* assembler,
                      const ControlFlowRegisterAllocation& allocation,
                      ir::Value value, std::size_t current_block,
                      int scratch) {
  const int allocated =
      control_word_register(allocation, value, current_block);
  if (allocated >= 0) {
    return allocated;
  }
  assembler->load(scratch, kStackPointer, control_spill_offset(value.id()));
  return scratch;
}

int load_control_float(Assembler* assembler,
                       const ControlFlowRegisterAllocation& allocation,
                       ir::Value value, std::size_t current_block,
                       int scratch) {
  const int allocated =
      control_float_register(allocation, value, current_block);
  if (allocated >= 0) {
    return allocated;
  }
  assembler->load_float(scratch, kStackPointer,
                        control_spill_offset(value.id()));
  return scratch;
}

void spill_control_stack_map_values(
    Assembler* assembler, const ir::ControlFlowFunction& function,
    const ControlFlowRegisterAllocation& allocation,
    const std::vector<ir::Value>& live_values, std::size_t current_block) {
  for (const ir::Value value : live_values) {
    if (function.value_type(value) == ir::ValueType::kFloat64) {
      const int source =
          control_float_register(allocation, value, current_block);
      if (source >= 0) {
        assembler->store_float(source, kStackPointer,
                               control_spill_offset(value.id()));
      }
    } else {
      const int source =
          control_word_register(allocation, value, current_block);
      if (source >= 0) {
        assembler->store(source, kStackPointer,
                         control_spill_offset(value.id()));
      }
    }
  }
}

void save_control_live_across_call(
    Assembler* assembler, const ir::ControlFlowFunction& function,
    const ControlFlowRegisterAllocation& allocation, ir::Value call,
    std::size_t current_block) {
  for (const ir::Value value : allocation.live_across_calls[call.id()]) {
    if (function.value_type(value) == ir::ValueType::kFloat64) {
      assembler->store_float(
          control_float_register(allocation, value, current_block),
          kStackPointer, control_spill_offset(value.id()));
    } else {
      assembler->store(
          control_word_register(allocation, value, current_block),
          kStackPointer, control_spill_offset(value.id()));
    }
  }
}

void restore_control_live_across_call(
    Assembler* assembler, const ir::ControlFlowFunction& function,
    const ControlFlowRegisterAllocation& allocation, ir::Value call,
    std::size_t current_block) {
  for (const ir::Value value : allocation.live_across_calls[call.id()]) {
    if (function.value_type(value) == ir::ValueType::kFloat64) {
      assembler->load_float(
          control_float_register(allocation, value, current_block),
          kStackPointer, control_spill_offset(value.id()));
    } else {
      assembler->load(
          control_word_register(allocation, value, current_block),
          kStackPointer, control_spill_offset(value.id()));
    }
  }
}

void capture_control_stack_map_values(
    Assembler* assembler, const std::vector<ir::Value>& live_values,
    int context) {
  for (std::size_t index = 0; index < live_values.size(); ++index) {
    assembler->load(kScratch1, kStackPointer,
                    control_spill_offset(live_values[index].id()));
    assembler->store(
        kScratch1, context,
        runtime::ExecutionContext::captured_values_offset() +
            index * sizeof(ir::Word));
  }
  assembler->move_immediate(kScratch1,
                            static_cast<ir::Word>(live_values.size()));
  assembler->store(
      kScratch1, context,
      runtime::ExecutionContext::captured_value_count_offset());
}

StackMapRecord make_stack_map_record(
    const ir::ControlFlowFunction& function, const ir::ControlNode& node,
    const std::vector<ir::Value>& live_values, std::size_t native_offset,
    std::size_t frame_size) {
  StackMapRecord record;
  record.site = static_cast<std::size_t>(node.immediate);
  record.native_offset = native_offset;
  record.frame_size = frame_size;
  record.kind = node.opcode == ir::ControlOpcode::kSafepoint
                    ? StackMapKind::kSafepoint
                    : StackMapKind::kGuard;
  record.live_values.reserve(live_values.size());
  for (const ir::Value value : live_values) {
    record.live_values.push_back(
        {value, function.value_type(value), control_spill_offset(value.id())});
  }
  return record;
}

void copy_edge_arguments(Assembler* assembler,
                         const ir::ControlFlowFunction& function,
                         const ir::ControlEdge& edge,
                         const ControlFlowRegisterAllocation& allocation,
                         std::size_t current_block,
                         std::size_t temporary_base) {
  const ControlFlowEdgeMoves moves = plan_control_flow_edge_moves(
      function, edge, allocation, current_block);
  const ir::BasicBlock& target = function.blocks()[edge.target.id()];
  if (moves.uses_registers) {
    for (const ControlFlowRegisterMove& move : moves.moves) {
      if (move.type == ir::ValueType::kFloat64) {
        const int destination =
            move.destination_index == ValueLocation::kNone
                ? kFloatScratch0
                : kFloatAllocationRegisters[move.destination_index];
        if (move.source_kind == ControlFlowMoveSource::kRegister) {
          assembler->move_float_register(
              destination, kFloatAllocationRegisters[move.source_index]);
        } else if (move.source_kind == ControlFlowMoveSource::kStack) {
          assembler->load_float(destination, kStackPointer,
                                control_spill_offset(move.source_index));
        } else {
          assembler->move_float_register(destination, kFloatScratch0);
        }
      } else {
        const int destination =
            move.destination_index == ValueLocation::kNone
                ? kScratch0
                : kAllocationRegisters[move.destination_index];
        if (move.source_kind == ControlFlowMoveSource::kRegister) {
          assembler->move_register(
              destination, kAllocationRegisters[move.source_index]);
        } else if (move.source_kind == ControlFlowMoveSource::kStack) {
          assembler->load(destination, kStackPointer,
                          control_spill_offset(move.source_index));
        } else {
          assembler->move_register(destination, kScratch0);
        }
      }
    }
    for (const ir::Value parameter : target.parameters) {
      if (allocation.requires_stack[parameter.id()]) {
        if (function.value_type(parameter) == ir::ValueType::kFloat64) {
          const int source = control_float_register(
              allocation, parameter, edge.target.id());
          assembler->store_float(source, kStackPointer,
                                 control_spill_offset(parameter.id()));
        } else {
          const int source = control_word_register(
              allocation, parameter, edge.target.id());
          assembler->store(source, kStackPointer,
                           control_spill_offset(parameter.id()));
        }
      }
    }
    return;
  }

  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    const ir::Value argument = edge.arguments[index];
    if (function.value_type(argument) == ir::ValueType::kFloat64) {
      const int source = load_control_float(
          assembler, allocation, argument, current_block, kFloatScratch0);
      assembler->store_float(source, kStackPointer,
                             control_spill_offset(temporary_base + index));
    } else {
      const int source = load_control_word(
          assembler, allocation, argument, current_block, kScratch0);
      assembler->store(source, kStackPointer,
                       control_spill_offset(temporary_base + index));
    }
  }
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    const ir::Value parameter = target.parameters[index];
    if (function.value_type(parameter) == ir::ValueType::kFloat64) {
      const int allocated =
          control_float_register(allocation, parameter, edge.target.id());
      const int destination =
          allocated >= 0 ? allocated : kFloatScratch0;
      assembler->load_float(destination, kStackPointer,
                            control_spill_offset(temporary_base + index));
      assembler->store_float(destination, kStackPointer,
                             control_spill_offset(parameter.id()));
    } else {
      const int allocated =
          control_word_register(allocation, parameter, edge.target.id());
      const int destination = allocated >= 0 ? allocated : kScratch0;
      assembler->load(destination, kStackPointer,
                      control_spill_offset(temporary_base + index));
      assembler->store(destination, kStackPointer,
                       control_spill_offset(parameter.id()));
    }
  }
}

LoweringResult lower_control_flow_impl(
    const ir::ControlFlowFunction& function,
    const StackMapRequirements& requirements) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return {{StatusCode::kUnsupportedArchitecture,
           "the RISC-V 64 encoder currently supports little-endian targets"},
          {},
          0};
#endif
  if (function.parameter_count() > kMaximumAddressableParameters) {
    return {{StatusCode::kResourceExhausted,
             "RISC-V parameter area exceeds signed 12-bit addressing"},
            {},
            0};
  }

  std::size_t maximum_block_parameters = 0;
  for (const ir::BasicBlock& block : function.blocks()) {
    maximum_block_parameters =
        std::max(maximum_block_parameters, block.parameters.size());
  }
  const bool has_context_operations = std::any_of(
      function.nodes().begin(), function.nodes().end(),
      [](const ir::ControlNode& node) {
        return node.opcode == ir::ControlOpcode::kSafepoint ||
               node.opcode == ir::ControlOpcode::kGuardFloatNonzero;
      });
  std::size_t maximum_call_arguments = 0;
  bool has_calls = false;
  for (const ir::ControlNode& node : function.nodes()) {
    if (node.opcode == ir::ControlOpcode::kCall) {
      has_calls = true;
      maximum_call_arguments =
          std::max(maximum_call_arguments,
                   static_cast<std::size_t>(node.argument_count));
    }
  }
  const std::size_t spill_slots =
      function.nodes().size() + maximum_block_parameters;
  const std::size_t context_slot = spill_slots;
  const std::size_t call_argument_base =
      context_slot + static_cast<std::size_t>(has_context_operations);
  const std::size_t return_address_slot =
      call_argument_base + maximum_call_arguments;
  const std::size_t total_slots =
      return_address_slot + static_cast<std::size_t>(has_calls);
  const std::size_t raw_stack_size = total_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size > kMaximumStackSize) {
    return {{StatusCode::kResourceExhausted,
             "RISC-V CFG spill frame exceeds the backend limit"},
            {},
            0};
  }

  ControlFlowRegisterAllocation allocation = allocate_control_flow_registers(
      function, kAllocationRegisters.size(), kFloatAllocationRegisters.size(),
      requirements);
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0, {}};
  }
  StackMapLiveness stack_map_liveness =
      plan_stack_map_liveness(function, requirements);
  if (!stack_map_liveness.status.ok()) {
    return {stack_map_liveness.status, {}, 0, {}};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBase, kArgumentAndReturn);
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }
  if (has_context_operations) {
    assembler.store(kContextArgument, kStackPointer,
                    context_slot * sizeof(ir::Word));
  }
  if (has_calls) {
    assembler.store(kReturnAddress, kStackPointer,
                    return_address_slot * sizeof(ir::Word));
  }

  const std::size_t no_label = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> labels(function.blocks().size(), no_label);
  std::vector<BranchFixup> fixups;
  std::vector<StackMapRecord> stack_maps;
  const std::size_t temporary_base = function.nodes().size();

  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    labels[block_index] = assembler.size();
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (const ir::Value value : block.instructions) {
      const ir::ControlNode& node = function.nodes()[value.id()];
      const std::size_t destination_offset = control_spill_offset(value.id());
      const bool destination_is_float =
          function.value_type(value) == ir::ValueType::kFloat64;
      const int allocated_word =
          destination_is_float
              ? -1
              : control_word_register(allocation, value, block_index);
      const int allocated_float =
          destination_is_float
              ? control_float_register(allocation, value, block_index)
              : -1;
      const int word_destination =
          allocated_word >= 0 ? allocated_word : kScratch0;
      const int float_destination =
          allocated_float >= 0 ? allocated_float : kFloatScratch0;
      const std::vector<ir::Value>& live_values =
          stack_map_liveness.live_values_by_node[value.id()];
      if (node.opcode == ir::ControlOpcode::kSafepoint ||
          node.opcode == ir::ControlOpcode::kGuardFloatNonzero) {
        spill_control_stack_map_values(&assembler, function, allocation,
                                       live_values, block_index);
        stack_maps.push_back(make_stack_map_record(
            function, node, live_values, assembler.size(), stack_size));
      }
      switch (node.opcode) {
        case ir::ControlOpcode::kParameter:
          if (destination_is_float) {
            assembler.load_float(
                float_destination, kArgumentBase,
                static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
            if (allocated_float < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store_float(float_destination, kStackPointer,
                                    destination_offset);
            }
          } else {
            assembler.load(
                word_destination, kArgumentBase,
                static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
            if (allocated_word < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kStackPointer,
                              destination_offset);
            }
          }
          break;
        case ir::ControlOpcode::kBlockParameter:
          break;
        case ir::ControlOpcode::kConstant:
          if (destination_is_float) {
            assembler.move_immediate(kScratch0, node.immediate);
            assembler.move_word_to_float(float_destination, kScratch0);
            if (allocated_float < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store_float(float_destination, kStackPointer,
                                    destination_offset);
            }
          } else {
            assembler.move_immediate(word_destination, node.immediate);
            if (allocated_word < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kStackPointer,
                              destination_offset);
            }
          }
          break;
        case ir::ControlOpcode::kCall:
          save_control_live_across_call(&assembler, function, allocation,
                                        value, block_index);
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            const ir::Value argument = function.call_arguments()[
                static_cast<std::size_t>(node.argument_begin) +
                argument_index];
            const std::size_t argument_offset =
                (call_argument_base + argument_index) * sizeof(ir::Word);
            if (function.value_type(argument) == ir::ValueType::kFloat64) {
              const int source = load_control_float(
                  &assembler, allocation, argument, block_index,
                  kFloatScratch0);
              assembler.store_float(source, kStackPointer, argument_offset);
            } else {
              const int source = load_control_word(
                  &assembler, allocation, argument, block_index, kScratch0);
              assembler.store(source, kStackPointer, argument_offset);
            }
          }
          assembler.address(kArgumentAndReturn, kStackPointer,
                            call_argument_base * sizeof(ir::Word));
          assembler.move_immediate(
              11, static_cast<ir::Word>(node.argument_count));
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.call_register(kScratch0);
          if (destination_is_float) {
            if (allocated_float >= 0) {
              assembler.move_word_to_float(float_destination,
                                           kArgumentAndReturn);
              if (allocation.requires_stack[value.id()]) {
                assembler.store_float(float_destination, kStackPointer,
                                      destination_offset);
              }
            } else {
              assembler.store(kArgumentAndReturn, kStackPointer,
                              destination_offset);
            }
          } else if (allocated_word >= 0) {
            assembler.move_register(word_destination, kArgumentAndReturn);
            if (allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kStackPointer,
                              destination_offset);
            }
          } else {
            assembler.store(kArgumentAndReturn, kStackPointer,
                            destination_offset);
          }
          restore_control_live_across_call(&assembler, function, allocation,
                                           value, block_index);
          break;
        case ir::ControlOpcode::kGuardFloatNonzero: {
          const int source = load_control_float(
              &assembler, allocation, node.lhs, block_index, kFloatScratch0);
          assembler.move_float_to_word(kScratch0, source);
          assembler.move_register(kScratch1, kScratch0);
          assembler.add(kScratch0, kScratch0, kScratch0);
          const std::size_t nonzero = assembler.branch_nonzero(kScratch0);

          assembler.load(kScratch0, kStackPointer,
                         context_slot * sizeof(ir::Word));
          const std::size_t no_context = assembler.branch_zero(kScratch0);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_value_offset());
          capture_control_stack_map_values(&assembler, live_values,
                                           kScratch0);
          assembler.move_immediate(kScratch1, node.immediate);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_site_offset());
          assembler.move_immediate(
              kScratch1,
              static_cast<ir::Word>(runtime::ExitReason::kRuntime));
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_reason_offset());

          const std::size_t exit = assembler.size();
          assembler.move_immediate(kArgumentAndReturn, 0);
          if (has_calls) {
            assembler.load(kReturnAddress, kStackPointer,
                           return_address_slot * sizeof(ir::Word));
          }
          if (stack_size != 0) {
            assembler.release_stack(stack_size);
          }
          assembler.return_to_caller();

          const std::size_t resume = assembler.size();
          const Status context_status =
              assembler.patch_zero_branch(no_context, exit);
          if (!context_status.ok()) {
            return {context_status, {}, 0};
          }
          const Status guard_status =
              assembler.patch_branch(nonzero, resume, true);
          if (!guard_status.ok()) {
            return {guard_status, {}, 0};
          }
          assembler.move_immediate(word_destination, 0);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kStackPointer,
                            destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kSafepoint: {
          assembler.load(kScratch0, kStackPointer,
                         context_slot * sizeof(ir::Word));
          const std::size_t no_context = assembler.branch_zero(kScratch0);
          assembler.load(
              kScratch0, kScratch0,
              runtime::ExecutionContext::interrupt_requested_offset());
          const std::size_t not_interrupted = assembler.branch_zero(kScratch0);
          assembler.load(kScratch0, kStackPointer,
                         context_slot * sizeof(ir::Word));
          assembler.move_immediate(kScratch1, 0);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_value_offset());
          capture_control_stack_map_values(&assembler, live_values,
                                           kScratch0);
          assembler.move_immediate(kScratch1, node.immediate);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_site_offset());
          assembler.move_immediate(
              kScratch1,
              static_cast<ir::Word>(runtime::ExitReason::kSafepoint));
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_reason_offset());
          assembler.move_immediate(kArgumentAndReturn, 0);
          if (has_calls) {
            assembler.load(kReturnAddress, kStackPointer,
                           return_address_slot * sizeof(ir::Word));
          }
          if (stack_size != 0) {
            assembler.release_stack(stack_size);
          }
          assembler.return_to_caller();

          const std::size_t resume = assembler.size();
          const Status context_status =
              assembler.patch_zero_branch(no_context, resume);
          if (!context_status.ok()) {
            return {context_status, {}, 0};
          }
          const Status interrupt_status =
              assembler.patch_zero_branch(not_interrupted, resume);
          if (!interrupt_status.ok()) {
            return {interrupt_status, {}, 0};
          }
          assembler.move_immediate(word_destination, 0);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kStackPointer,
                            destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kFloatAdd:
        case ir::ControlOpcode::kFloatSubtract:
        case ir::ControlOpcode::kFloatMultiply:
        case ir::ControlOpcode::kFloatDivide: {
          const int lhs = load_control_float(
              &assembler, allocation, node.lhs, block_index, kFloatScratch0);
          const int rhs = load_control_float(
              &assembler, allocation, node.rhs, block_index, kFloatScratch1);
          if (node.opcode == ir::ControlOpcode::kFloatAdd) {
            assembler.add_float(float_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kFloatSubtract) {
            assembler.subtract_float(float_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kFloatMultiply) {
            assembler.multiply_float(float_destination, lhs, rhs);
          } else {
            assembler.divide_float(float_destination, lhs, rhs);
          }
          if (allocated_float < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store_float(float_destination, kStackPointer,
                                  destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kFloatLessThan:
        case ir::ControlOpcode::kFloatLessEqual: {
          const int lhs = load_control_float(
              &assembler, allocation, node.lhs, block_index, kFloatScratch0);
          const int rhs = load_control_float(
              &assembler, allocation, node.rhs, block_index, kFloatScratch1);
          assembler.compare_float(
              word_destination, lhs, rhs,
              node.opcode == ir::ControlOpcode::kFloatLessEqual);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kStackPointer,
                            destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          const int lhs = load_control_word(
              &assembler, allocation, node.lhs, block_index, kScratch0);
          const int rhs = load_control_word(
              &assembler, allocation, node.rhs, block_index, kScratch1);
          if (node.opcode == ir::ControlOpcode::kAdd) {
            assembler.add(word_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kSubtract) {
            assembler.subtract(word_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kMultiply) {
            assembler.multiply(word_destination, lhs, rhs);
          } else {
            assembler.compare(word_destination, lhs, rhs,
                              node.opcode == ir::ControlOpcode::kLessEqual);
          }
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kStackPointer,
                            destination_offset);
          }
          break;
      }
    }

    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kReturn) {
      if (function.return_type() == ir::ValueType::kFloat64) {
        const int returned = load_control_float(
            &assembler, allocation, terminator.value, block_index,
            kFloatScratch0);
        assembler.move_float_to_word(kArgumentAndReturn, returned);
      } else {
        const int returned = load_control_word(
            &assembler, allocation, terminator.value, block_index, kScratch0);
        if (returned != kArgumentAndReturn) {
          assembler.move_register(kArgumentAndReturn, returned);
        }
      }
      if (has_calls) {
        assembler.load(kReturnAddress, kStackPointer,
                       return_address_slot * sizeof(ir::Word));
      }
      if (stack_size != 0) {
        assembler.release_stack(stack_size);
      }
      assembler.return_to_caller();
    } else if (terminator.opcode == ir::TerminatorOpcode::kJump) {
      copy_edge_arguments(&assembler, function, terminator.true_edge,
                          allocation, block_index, temporary_base);
      fixups.push_back({assembler.branch(), terminator.true_edge.target});
    } else {
      const int condition = load_control_word(
          &assembler, allocation, terminator.value, block_index, kScratch0);
      if (condition != kScratch0) {
        assembler.move_register(kScratch0, condition);
      }
      const std::size_t true_selector = assembler.branch_nonzero(kScratch0);
      copy_edge_arguments(&assembler, function, terminator.false_edge,
                          allocation, block_index, temporary_base);
      fixups.push_back({assembler.branch(), terminator.false_edge.target});
      const std::size_t true_copy = assembler.size();
      const Status selector_status =
          assembler.patch_branch(true_selector, true_copy, true);
      if (!selector_status.ok()) {
        return {selector_status, {}, 0};
      }
      copy_edge_arguments(&assembler, function, terminator.true_edge,
                          allocation, block_index, temporary_base);
      fixups.push_back({assembler.branch(), terminator.true_edge.target});
    }
  }

  for (const BranchFixup& fixup : fixups) {
    if (!fixup.target.valid() || fixup.target.id() >= labels.size() ||
        labels[fixup.target.id()] == no_label) {
      return {{StatusCode::kCodeGenerationFailed,
               "RISC-V CFG branch has no bound target"},
              {},
              0};
    }
    const Status patch_status =
        assembler.patch_branch(fixup.offset, labels[fixup.target.id()], false);
    if (!patch_status.ok()) {
      return {patch_status, {}, 0};
    }
  }

  const Status literals = assembler.finalize_literals();
  if (!literals.ok()) {
    return {literals, {}, 0};
  }
  return {Status::ok_status(), assembler.take_code(), total_slots,
          std::move(stack_maps)};
}

}  // namespace

LoweringResult lower(const ir::Function& function,
                     const StackMapRequirements& requirements) {
  try {
    return lower_impl(function, requirements);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate RISC-V 64 lowering state"},
            {},
            0};
  }
}

LoweringResult lower(const ir::ControlFlowFunction& function,
                     const StackMapRequirements& requirements) {
  try {
    return lower_control_flow_impl(function, requirements);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate RISC-V CFG lowering state"},
            {},
            0};
  }
}

}  // namespace unijit::jit::detail::riscv64
