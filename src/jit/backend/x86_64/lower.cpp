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
#include "unijit/runtime/execution_context.h"

namespace unijit::jit::detail::x86_64 {
namespace {

constexpr int kRax = 0;
constexpr int kRcx = 1;
constexpr int kRdx = 2;
constexpr int kRsp = 4;
constexpr int kRsi = 6;
constexpr int kRdi = 7;
constexpr int kR8 = 8;
constexpr int kR9 = 9;
constexpr int kR10 = 10;
constexpr int kR11 = 11;

#if defined(_WIN32)
constexpr int kArgumentRegister = kRcx;
constexpr int kContextArgumentRegister = kRdx;
constexpr int kRuntimeArgument0 = kRcx;
constexpr int kRuntimeArgument1 = kRdx;
constexpr std::size_t kCallStackAdjustment = 40;
#else
constexpr int kArgumentRegister = kRdi;
constexpr int kContextArgumentRegister = kRsi;
constexpr int kRuntimeArgument0 = kRdi;
constexpr int kRuntimeArgument1 = kRsi;
constexpr std::size_t kCallStackAdjustment = 8;
#endif

constexpr int kReturnRegister = kRax;
constexpr int kArgumentBaseRegister = kR11;
constexpr int kScratch0 = kRax;
constexpr int kScratch1 = kR10;
constexpr int kFloatScratch0 = 0;
constexpr int kFloatScratch1 = 5;
constexpr std::array<int, 4> kAllocationRegisters = {kRcx, kRdx, kR8, kR9};
#if defined(_WIN32)
constexpr std::array<int, 4> kFloatAllocationRegisters = {1, 2, 3, 4};
#else
constexpr std::array<int, 14> kFloatAllocationRegisters = {
    1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
#endif
constexpr std::size_t kMaximumStackSize = 1024U * 1024U;
constexpr std::size_t kMaximumOffset =
    static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

enum class FloatCondition : std::uint8_t {
  kLessThan,
  kLessEqual,
  kEqual,
  kNotEqual,
};

enum class WordCondition : std::uint8_t {
  kLessThan,
  kLessEqual,
  kEqual,
  kNotEqual,
};

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

  void move_word_to_float(int destination, int source) {
    buffer_.emit_u8(0x66U);
    emit_rex(destination, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x6EU);
    emit_modrm(3, destination, source);
  }

  void move_float_to_word(int destination, int source) {
    buffer_.emit_u8(0x66U);
    emit_rex(source, destination);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x7EU);
    emit_modrm(3, source, destination);
  }

  void move_float_register(int destination, int source) {
    buffer_.emit_u8(0xF2U);
    emit_float_rex(destination, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x10U);
    emit_modrm(3, destination, source);
  }

  void load_float(int destination, int base, std::size_t byte_offset) {
    buffer_.emit_u8(0xF2U);
    emit_float_rex(destination, base);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x10U);
    emit_memory_modrm(destination, base);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_offset));
  }

  void store_float(int source, int base, std::size_t byte_offset) {
    buffer_.emit_u8(0xF2U);
    emit_float_rex(source, base);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x11U);
    emit_memory_modrm(source, base);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_offset));
  }

  void add_float(int destination, int lhs, int rhs) {
    prepare_float_binary(destination, lhs);
    emit_float_binary(0x58U, destination, rhs);
  }

  void subtract_float(int destination, int lhs, int rhs) {
    prepare_float_binary(destination, lhs);
    emit_float_binary(0x5CU, destination, rhs);
  }

  void negate_float(int destination, int source) {
    move_float_to_word(kScratch0, source);
    move_immediate(kScratch1, std::numeric_limits<ir::Word>::min());
    emit_rex(kScratch1, kScratch0);
    buffer_.emit_u8(0x31U);
    emit_modrm(3, kScratch1, kScratch0);
    move_word_to_float(destination, kScratch0);
  }

  void multiply_float(int destination, int lhs, int rhs) {
    prepare_float_binary(destination, lhs);
    emit_float_binary(0x59U, destination, rhs);
  }

  void divide_float(int destination, int lhs, int rhs) {
    prepare_float_binary(destination, lhs);
    emit_float_binary(0x5EU, destination, rhs);
  }

  void compare_float(int destination, int lhs, int rhs,
                     FloatCondition condition) {
    buffer_.emit_u8(0x66U);
    emit_float_rex(rhs, lhs);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x2EU);
    emit_modrm(3, rhs, lhs);
    if (condition == FloatCondition::kEqual ||
        condition == FloatCondition::kNotEqual) {
      emit_set_condition(destination,
                         condition == FloatCondition::kEqual ? 0x94U : 0x95U);
      emit_set_condition(kScratch1,
                         condition == FloatCondition::kEqual ? 0x9BU : 0x9AU);
      zero_extend_byte(destination);
      zero_extend_byte(kScratch1);
      emit_rex(kScratch1, destination);
      buffer_.emit_u8(condition == FloatCondition::kEqual ? 0x21U : 0x09U);
      emit_modrm(3, kScratch1, destination);
      return;
    }
    emit_set_condition(destination,
                       condition == FloatCondition::kLessEqual ? 0x93U
                                                               : 0x97U);
    zero_extend_byte(destination);
  }

  void emit_set_condition(int destination, std::uint8_t opcode) {
    emit_rex(0, destination);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(opcode);
    emit_modrm(3, 0, destination);
  }

  void zero_extend_byte(int destination) {
    emit_rex(destination, destination);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0xB6U);
    emit_modrm(3, destination, destination);
  }

  void add(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(rhs, destination);
    buffer_.emit_u8(0x01U);
    emit_modrm(3, rhs, destination);
  }

  void increment(int destination) {
    emit_rex(0, destination);
    buffer_.emit_u8(0x83U);
    emit_modrm(3, 0, destination);
    buffer_.emit_u8(1);
  }

  void subtract(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(rhs, destination);
    buffer_.emit_u8(0x29U);
    emit_modrm(3, rhs, destination);
  }

  void unary_word(int destination, int source, int extension) {
    if (destination != source) {
      move_register(destination, source);
    }
    emit_rex(0, destination);
    buffer_.emit_u8(0xF7U);
    emit_modrm(3, extension, destination);
  }

  void negate(int destination, int source) {
    unary_word(destination, source, 3);
  }

  void bitwise_not(int destination, int source) {
    unary_word(destination, source, 2);
  }

  void multiply(int destination, int lhs, int rhs) {
    prepare_binary(destination, lhs);
    emit_rex(destination, rhs);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0xAFU);
    emit_modrm(3, destination, rhs);
  }

  void bitwise_binary(int destination, int lhs, int rhs,
                      std::uint8_t opcode) {
    prepare_binary(destination, lhs);
    emit_rex(rhs, destination);
    buffer_.emit_u8(opcode);
    emit_modrm(3, rhs, destination);
  }

  void bitwise_and(int destination, int lhs, int rhs) {
    bitwise_binary(destination, lhs, rhs, 0x21U);
  }

  void bitwise_or(int destination, int lhs, int rhs) {
    bitwise_binary(destination, lhs, rhs, 0x09U);
  }

  void bitwise_xor(int destination, int lhs, int rhs) {
    bitwise_binary(destination, lhs, rhs, 0x31U);
  }

  Status shift_left(int destination, int value, int amount) {
    move_register(kScratch0, value);
    move_register(kScratch1, amount);
    move_register(kR11, kRcx);

    const std::size_t negative = branch_negative(kScratch1);
    const std::size_t positive_overshift =
        branch_unsigned_greater_equal(kScratch1, 64);
    move_register(kRcx, kScratch1);
    shift_variable(kScratch0, false);
    const std::size_t positive_done = branch();

    const std::size_t negative_path = size();
    negate(kScratch1, kScratch1);
    const std::size_t negative_overshift =
        branch_unsigned_greater_equal(kScratch1, 64);
    move_register(kRcx, kScratch1);
    shift_variable(kScratch0, true);
    const std::size_t negative_done = branch();

    const std::size_t zero = size();
    move_immediate(kScratch0, 0);
    const std::size_t done = size();

    Status status = patch_branch(negative, negative_path);
    if (status.ok()) {
      status = patch_branch(positive_overshift, zero);
    }
    if (status.ok()) {
      status = patch_branch(positive_done, done);
    }
    if (status.ok()) {
      status = patch_branch(negative_overshift, zero);
    }
    if (status.ok()) {
      status = patch_branch(negative_done, done);
    }
    move_register(kRcx, kR11);
    if (status.ok() && destination != kScratch0) {
      move_register(destination, kScratch0);
    }
    return status;
  }

  Status floor_arithmetic(int destination, int lhs, int rhs, bool modulo) {
    move_register(kScratch1, rhs);
    move_register(kScratch0, lhs);
    const std::size_t zero_divisor = branch_zero(kScratch1);
    const std::size_t minus_one = branch_equal_immediate(kScratch1, -1);

    if (destination != kRdx) {
      move_register(kR11, kRdx);
    }
    // CQO; IDIV r/m64. The -1 overflow case was split above.
    buffer_.emit_u8(0x48U);
    buffer_.emit_u8(0x99U);
    emit_rex(7, kScratch1);
    buffer_.emit_u8(0xF7U);
    emit_modrm(3, 7, kScratch1);
    const std::size_t exact = branch_zero(kRdx);
    const std::size_t negative_remainder = branch_negative(kRdx);
    const std::size_t positive_same_sign = branch_nonnegative(kScratch1);
    const std::size_t positive_adjust = branch();
    const std::size_t negative_path = size();
    const std::size_t negative_same_sign = branch_negative(kScratch1);
    const std::size_t adjust = size();
    if (modulo) {
      add(kRdx, kRdx, kScratch1);
    } else {
      emit_rex(0, kScratch0);
      buffer_.emit_u8(0x83U);
      emit_modrm(3, 5, kScratch0);
      buffer_.emit_u8(1U);
    }
    const std::size_t result = size();
    move_register(destination, modulo ? kRdx : kScratch0);
    if (destination != kRdx) {
      move_register(kRdx, kR11);
    }
    const std::size_t normal_done = branch();

    const std::size_t special = size();
    if (modulo) {
      move_immediate(destination, 0);
    } else {
      negate(destination, kScratch0);
    }
    const std::size_t special_done = branch();
    const std::size_t zero = size();
    move_immediate(destination, 0);
    const std::size_t end = size();

    Status status = patch_branch(zero_divisor, zero);
    if (status.ok()) {
      status = patch_branch(minus_one, special);
    }
    if (status.ok()) {
      status = patch_branch(exact, result);
    }
    if (status.ok()) {
      status = patch_branch(negative_remainder, negative_path);
    }
    if (status.ok()) {
      status = patch_branch(positive_same_sign, result);
    }
    if (status.ok()) {
      status = patch_branch(positive_adjust, adjust);
    }
    if (status.ok()) {
      status = patch_branch(negative_same_sign, result);
    }
    if (status.ok()) {
      status = patch_branch(normal_done, end);
    }
    if (status.ok()) {
      status = patch_branch(special_done, end);
    }
    return status;
  }

  void compare(int destination, int lhs, int rhs, WordCondition condition) {
    emit_rex(rhs, lhs);
    buffer_.emit_u8(0x39U);
    emit_modrm(3, rhs, lhs);
    emit_rex(0, destination);
    buffer_.emit_u8(0x0FU);
    std::uint8_t set_opcode = 0x9CU;
    if (condition == WordCondition::kLessEqual) {
      set_opcode = 0x9EU;
    } else if (condition == WordCondition::kEqual) {
      set_opcode = 0x94U;
    } else if (condition == WordCondition::kNotEqual) {
      set_opcode = 0x95U;
    }
    buffer_.emit_u8(set_opcode);
    emit_modrm(3, 0, destination);
    emit_rex(destination, destination);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0xB6U);
    emit_modrm(3, destination, destination);
  }

  void address(int destination, int base, std::size_t byte_offset) {
    emit_rex(destination, base);
    buffer_.emit_u8(0x8DU);
    emit_memory_modrm(destination, base);
    buffer_.emit_u32(static_cast<std::uint32_t>(byte_offset));
  }

  void call_register(int target) {
    emit_rex(2, target);
    buffer_.emit_u8(0xFFU);
    emit_modrm(3, 2, target);
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

  std::size_t branch_zero(int source) {
    emit_rex(source, source);
    buffer_.emit_u8(0x85U);
    emit_modrm(3, source, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x84U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  std::size_t branch_negative(int source) {
    emit_rex(source, source);
    buffer_.emit_u8(0x85U);
    emit_modrm(3, source, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x88U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  std::size_t branch_nonnegative(int source) {
    emit_rex(source, source);
    buffer_.emit_u8(0x85U);
    emit_modrm(3, source, source);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x89U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  std::size_t branch_equal_immediate(int source, std::int8_t immediate) {
    emit_rex(0, source);
    buffer_.emit_u8(0x83U);
    emit_modrm(3, 7, source);
    buffer_.emit_u8(static_cast<std::uint8_t>(immediate));
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x84U);
    const std::size_t displacement = buffer_.size();
    buffer_.emit_u32(0);
    return displacement;
  }

  std::size_t branch_unsigned_greater_equal(int source,
                                             std::uint8_t immediate) {
    emit_rex(0, source);
    buffer_.emit_u8(0x83U);
    emit_modrm(3, 7, source);
    buffer_.emit_u8(immediate);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(0x83U);
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
  void shift_variable(int destination, bool right) {
    emit_rex(0, destination);
    buffer_.emit_u8(0xD3U);
    emit_modrm(3, right ? 5 : 4, destination);
  }

  void prepare_binary(int destination, int lhs) {
    if (destination != lhs) {
      move_register(destination, lhs);
    }
  }

  void prepare_float_binary(int destination, int lhs) {
    if (destination != lhs) {
      move_float_register(destination, lhs);
    }
  }

  void emit_float_binary(std::uint8_t opcode, int destination, int rhs) {
    buffer_.emit_u8(0xF2U);
    emit_float_rex(destination, rhs);
    buffer_.emit_u8(0x0FU);
    buffer_.emit_u8(opcode);
    emit_modrm(3, destination, rhs);
  }

  void emit_rex(int reg_field, int rm_field) {
    buffer_.emit_u8(static_cast<std::uint8_t>(0x48U | ((reg_field >> 3) << 2) |
                                              (rm_field >> 3)));
  }

  void emit_float_rex(int reg_field, int rm_field) {
    buffer_.emit_u8(static_cast<std::uint8_t>(0x40U |
                                              ((reg_field >> 3) << 2) |
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
  assembler->load(scratch, kRsp, spill_offset(location));
  return scratch;
}

int load_float_operand(Assembler* assembler, const ValueLocation& location,
                       int scratch) {
  if (location.in_register()) {
    return physical_float_register(location);
  }
  assembler->load_float(scratch, kRsp, spill_offset(location));
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
      assembler->store_float(physical_float_register(location), kRsp,
                             spill_offset(location));
    } else {
      assembler->store(physical_register(location), kRsp,
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
      assembler->load_float(physical_float_register(location), kRsp,
                            spill_offset(location));
    } else {
      assembler->load(physical_register(location), kRsp,
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
      assembler->store_float(source, kRsp, offset);
    } else {
      const int source = load_operand(assembler, location, kScratch0);
      assembler->store(source, kRsp, offset);
    }
  }
}

void capture_straight_stack_map_values(
    Assembler* assembler, const std::vector<ir::Value>& live_values,
    std::size_t stack_map_base, int context) {
  for (std::size_t index = 0; index < live_values.size(); ++index) {
    assembler->load(kScratch1, kRsp,
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
                          const StackMapRequirements& requirements,
                          bool measure_safepoint_polls) {
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
  bool has_context_operations = false;
  for (const ir::Node& node : function.nodes()) {
    if (node.opcode == ir::Opcode::kCall) {
      maximum_call_arguments =
          std::max(maximum_call_arguments,
                   static_cast<std::size_t>(node.argument_count));
    } else if (node.opcode == ir::Opcode::kSafepoint ||
               node.opcode == ir::Opcode::kGuardWordNonzero ||
               node.opcode == ir::Opcode::kGuardFloatNonzero) {
      has_context_operations = true;
    }
  }
  const std::size_t call_argument_base = allocation.spill_slots;
  const std::size_t context_slot =
      call_argument_base + maximum_call_arguments;
  const std::size_t stack_map_base =
      context_slot + static_cast<std::size_t>(has_context_operations);
  const std::size_t total_slots =
      stack_map_base +
      (has_context_operations ? function.nodes().size() : 0);
  if (total_slots > kMaximumStackSize / sizeof(ir::Word)) {
    return {{StatusCode::kResourceExhausted,
             "x86-64 runtime-call frame exceeds the backend limit"},
            {},
            0};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kArgumentRegister);
  const std::size_t raw_stack_size = total_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }
  if (has_context_operations) {
    assembler.store(kContextArgumentRegister, kRsp,
                    context_slot * sizeof(ir::Word));
  }

  std::vector<StackMapRecord> stack_maps;

  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::Node& node = function.nodes()[index];
    const ValueLocation& destination = allocation.locations[index];
    const std::vector<ir::Value>& live_values =
        stack_map_liveness.live_values_by_node[index];
    if (node.opcode == ir::Opcode::kSafepoint ||
        node.opcode == ir::Opcode::kGuardWordNonzero ||
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
              target, kArgumentBaseRegister,
              static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
          if (!destination.in_register()) {
            assembler.store_float(target, kRsp, spill_offset(destination));
          }
          break;
        }
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
        if (node.type == ir::ValueType::kFloat64) {
          const int target = destination.in_register()
                                 ? physical_float_register(destination)
                                 : kFloatScratch0;
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.move_word_to_float(target, kScratch0);
          if (!destination.in_register()) {
            assembler.store_float(target, kRsp, spill_offset(destination));
          }
          break;
        }
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
      case ir::Opcode::kMultiply:
      case ir::Opcode::kBitwiseAnd:
      case ir::Opcode::kBitwiseOr:
      case ir::Opcode::kBitwiseXor:
      case ir::Opcode::kShiftLeft:
      case ir::Opcode::kFloorDivide:
      case ir::Opcode::kFloorModulo:
      case ir::Opcode::kLessThan:
      case ir::Opcode::kLessEqual:
      case ir::Opcode::kEqual:
      case ir::Opcode::kNotEqual: {
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
        } else if (node.opcode == ir::Opcode::kMultiply) {
          assembler.multiply(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kBitwiseAnd) {
          assembler.bitwise_and(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kBitwiseOr) {
          assembler.bitwise_or(target, lhs, rhs);
        } else if (node.opcode == ir::Opcode::kShiftLeft) {
          const Status shift_status = assembler.shift_left(target, lhs, rhs);
          if (!shift_status.ok()) {
            return {shift_status, {}, 0};
          }
        } else if (node.opcode == ir::Opcode::kFloorDivide ||
                   node.opcode == ir::Opcode::kFloorModulo) {
          const Status floor_status = assembler.floor_arithmetic(
              kReturnRegister, lhs, rhs,
              node.opcode == ir::Opcode::kFloorModulo);
          if (!floor_status.ok()) {
            return {floor_status, {}, 0};
          }
          if (target != kReturnRegister) {
            assembler.move_register(target, kReturnRegister);
          }
        } else if (node.opcode == ir::Opcode::kLessThan ||
                   node.opcode == ir::Opcode::kLessEqual ||
                   node.opcode == ir::Opcode::kEqual ||
                   node.opcode == ir::Opcode::kNotEqual) {
          WordCondition condition = WordCondition::kLessThan;
          if (node.opcode == ir::Opcode::kLessEqual) {
            condition = WordCondition::kLessEqual;
          } else if (node.opcode == ir::Opcode::kEqual) {
            condition = WordCondition::kEqual;
          } else if (node.opcode == ir::Opcode::kNotEqual) {
            condition = WordCondition::kNotEqual;
          }
          assembler.compare(target, lhs, rhs, condition);
        } else {
          assembler.bitwise_xor(target, lhs, rhs);
        }
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kNegate:
      case ir::Opcode::kBitwiseNot: {
        const int source = load_operand(
            &assembler, allocation.locations[node.lhs.id()], kScratch0);
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        if (node.opcode == ir::Opcode::kNegate) {
          assembler.negate(target, source);
        } else {
          assembler.bitwise_not(target, source);
        }
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
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
          assembler.store_float(target, kRsp, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kFloatNegate: {
        const int source = load_float_operand(
            &assembler, allocation.locations[node.lhs.id()], kFloatScratch0);
        const int target = destination.in_register()
                               ? physical_float_register(destination)
                               : kFloatScratch0;
        assembler.negate_float(target, source);
        if (!destination.in_register()) {
          assembler.store_float(target, kRsp, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kFloatLessThan:
      case ir::Opcode::kFloatLessEqual:
      case ir::Opcode::kFloatEqual:
      case ir::Opcode::kFloatNotEqual: {
        const int lhs = load_float_operand(
            &assembler, allocation.locations[node.lhs.id()], kFloatScratch0);
        const int rhs = load_float_operand(
            &assembler, allocation.locations[node.rhs.id()], kFloatScratch1);
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        FloatCondition condition = FloatCondition::kLessThan;
        if (node.opcode == ir::Opcode::kFloatLessEqual) {
          condition = FloatCondition::kLessEqual;
        } else if (node.opcode == ir::Opcode::kFloatEqual) {
          condition = FloatCondition::kEqual;
        } else if (node.opcode == ir::Opcode::kFloatNotEqual) {
          condition = FloatCondition::kNotEqual;
        }
        assembler.compare_float(target, lhs, rhs, condition);
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
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
            assembler.store_float(source_register, kRsp, argument_offset);
          } else {
            const int source_register =
                load_operand(&assembler, source, kScratch0);
            assembler.store(source_register, kRsp, argument_offset);
          }
        }
        assembler.address(kRuntimeArgument0, kRsp,
                          call_argument_base * sizeof(ir::Word));
        assembler.move_immediate(
            kRuntimeArgument1, static_cast<ir::Word>(node.argument_count));
        assembler.move_immediate(kScratch0, node.immediate);
        assembler.reserve_stack(kCallStackAdjustment);
        assembler.call_register(kScratch0);
        assembler.release_stack(kCallStackAdjustment);
        if (node.type == ir::ValueType::kFloat64) {
          if (destination.in_register()) {
            assembler.move_word_to_float(physical_float_register(destination),
                                         kReturnRegister);
          } else {
            assembler.store(kReturnRegister, kRsp, spill_offset(destination));
          }
        } else if (destination.in_register()) {
          assembler.move_register(physical_register(destination),
                                  kReturnRegister);
        } else {
          assembler.store(kReturnRegister, kRsp, spill_offset(destination));
        }
        restore_live_across_call(&assembler, function, allocation, index);
        break;
      }
      case ir::Opcode::kGuardWordNonzero:
      case ir::Opcode::kGuardFloatNonzero: {
        std::size_t nonzero = 0;
        if (node.opcode == ir::Opcode::kGuardWordNonzero) {
          const int source = load_operand(
              &assembler, allocation.locations[node.lhs.id()], kScratch0);
          assembler.move_register(kScratch1, source);
          nonzero = assembler.branch_nonzero(kScratch1);
        } else {
          const int source = load_float_operand(
              &assembler, allocation.locations[node.lhs.id()],
              kFloatScratch0);
          assembler.move_float_to_word(kScratch0, source);
          assembler.move_register(kScratch1, kScratch0);
          assembler.add(kScratch0, kScratch0, kScratch0);
          nonzero = assembler.branch_nonzero(kScratch0);
        }

        assembler.load(kScratch0, kRsp, context_slot * sizeof(ir::Word));
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
        assembler.move_immediate(kReturnRegister, 0);
        if (stack_size != 0) {
          assembler.release_stack(stack_size);
        }
        assembler.return_to_caller();

        const std::size_t resume = assembler.size();
        const Status context_status =
            assembler.patch_branch(no_context, exit);
        if (!context_status.ok()) {
          return {context_status, {}, 0};
        }
        const Status guard_status = assembler.patch_branch(nonzero, resume);
        if (!guard_status.ok()) {
          return {guard_status, {}, 0};
        }
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, 0);
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
        }
        break;
      }
      case ir::Opcode::kSafepoint: {
        assembler.load(kScratch0, kRsp, context_slot * sizeof(ir::Word));
        const std::size_t no_context = assembler.branch_zero(kScratch0);
        if (measure_safepoint_polls) {
          assembler.load(
              kScratch1, kScratch0,
              runtime::ExecutionContext::safepoint_polls_offset());
          assembler.increment(kScratch1);
          assembler.store(
              kScratch1, kScratch0,
              runtime::ExecutionContext::safepoint_polls_offset());
        }
        assembler.load(
            kScratch0, kScratch0,
            runtime::ExecutionContext::interrupt_requested_offset());
        const std::size_t not_interrupted = assembler.branch_zero(kScratch0);
        assembler.load(kScratch0, kRsp, context_slot * sizeof(ir::Word));
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
        assembler.move_immediate(kReturnRegister, 0);
        if (stack_size != 0) {
          assembler.release_stack(stack_size);
        }
        assembler.return_to_caller();

        const std::size_t resume = assembler.size();
        const Status context_status =
            assembler.patch_branch(no_context, resume);
        if (!context_status.ok()) {
          return {context_status, {}, 0};
        }
        const Status interrupt_status =
            assembler.patch_branch(not_interrupted, resume);
        if (!interrupt_status.ok()) {
          return {interrupt_status, {}, 0};
        }
        const int target = destination.in_register()
                               ? physical_register(destination)
                               : kScratch0;
        assembler.move_immediate(target, 0);
        if (!destination.in_register()) {
          assembler.store(target, kRsp, spill_offset(destination));
        }
        break;
      }
    }
  }

  const ValueLocation& returned =
      allocation.locations[function.return_value().id()];
  if (function.return_type() == ir::ValueType::kFloat64) {
    if (returned.in_register()) {
      assembler.move_float_to_word(kReturnRegister,
                                   physical_float_register(returned));
    } else {
      assembler.load(kReturnRegister, kRsp, spill_offset(returned));
    }
  } else if (returned.in_register()) {
    assembler.move_register(kReturnRegister, physical_register(returned));
  } else {
    assembler.load(kReturnRegister, kRsp, spill_offset(returned));
  }
  if (stack_size != 0) {
    assembler.release_stack(stack_size);
  }
  assembler.return_to_caller();

  return {Status::ok_status(), assembler.take_code(), total_slots,
          std::move(stack_maps)};
}

struct BranchFixup final {
  std::size_t displacement{0};
  ir::Block target;
};

std::size_t control_spill_offset(std::size_t slot) noexcept {
  return slot * sizeof(ir::Word);
}

std::size_t control_value_offset(
    const ControlFlowRegisterAllocation& allocation,
    ir::Value value) noexcept {
  return control_spill_offset(allocation.stack_indices[value.id()]);
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
  assembler->load(scratch, kRsp, control_value_offset(allocation, value));
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
  assembler->load_float(scratch, kRsp,
                        control_value_offset(allocation, value));
  return scratch;
}

void spill_control_stack_map_values(
    Assembler* assembler, const ir::ControlFlowFunction& function,
    const ControlFlowRegisterAllocation& allocation,
    const std::vector<ir::Value>& live_values, std::size_t current_block) {
  for (const ir::Value value : live_values) {
    if (allocation.requires_stack[value.id()]) {
      continue;
    }
    if (function.value_type(value) == ir::ValueType::kFloat64) {
      const int source =
          control_float_register(allocation, value, current_block);
      if (source >= 0) {
        assembler->store_float(source, kRsp,
                               control_value_offset(allocation, value));
      }
    } else {
      const int source =
          control_word_register(allocation, value, current_block);
      if (source >= 0) {
        assembler->store(source, kRsp,
                         control_value_offset(allocation, value));
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
          control_float_register(allocation, value, current_block), kRsp,
          control_value_offset(allocation, value));
    } else {
      assembler->store(
          control_word_register(allocation, value, current_block), kRsp,
          control_value_offset(allocation, value));
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
          control_float_register(allocation, value, current_block), kRsp,
          control_value_offset(allocation, value));
    } else {
      assembler->load(
          control_word_register(allocation, value, current_block), kRsp,
          control_value_offset(allocation, value));
    }
  }
}

void capture_control_stack_map_values(
    Assembler* assembler, const ControlFlowRegisterAllocation& allocation,
    const std::vector<ir::Value>& live_values, int context) {
  for (std::size_t index = 0; index < live_values.size(); ++index) {
    assembler->load(kScratch1, kRsp,
                    control_value_offset(allocation, live_values[index]));
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
    const ControlFlowRegisterAllocation& allocation,
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
        {value, function.value_type(value),
         control_value_offset(allocation, value)});
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
          assembler->load_float(destination, kRsp,
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
          assembler->load(destination, kRsp,
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
          assembler->store_float(source, kRsp,
                                 control_value_offset(allocation, parameter));
        } else {
          const int source = control_word_register(
              allocation, parameter, edge.target.id());
          assembler->store(source, kRsp,
                           control_value_offset(allocation, parameter));
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
      assembler->store_float(source, kRsp,
                             control_spill_offset(temporary_base + index));
    } else {
      const int source = load_control_word(
          assembler, allocation, argument, current_block, kScratch0);
      assembler->store(source, kRsp,
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
      assembler->load_float(destination, kRsp,
                            control_spill_offset(temporary_base + index));
      if (allocated < 0 || allocation.requires_stack[parameter.id()]) {
        assembler->store_float(destination, kRsp,
                               control_value_offset(allocation, parameter));
      }
    } else {
      const int allocated =
          control_word_register(allocation, parameter, edge.target.id());
      const int destination = allocated >= 0 ? allocated : kScratch0;
      assembler->load(destination, kRsp,
                      control_spill_offset(temporary_base + index));
      if (allocated < 0 || allocation.requires_stack[parameter.id()]) {
        assembler->store(destination, kRsp,
                         control_value_offset(allocation, parameter));
      }
    }
  }
}

LoweringResult lower_control_flow_impl(
    const ir::ControlFlowFunction& function,
    const StackMapRequirements& requirements,
    bool measure_safepoint_polls) {
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
  const bool has_context_operations = std::any_of(
      function.nodes().begin(), function.nodes().end(),
      [](const ir::ControlNode& node) {
        return node.opcode == ir::ControlOpcode::kSafepoint ||
               node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
               node.opcode == ir::ControlOpcode::kGuardFloatNonzero;
      });
  std::size_t maximum_call_arguments = 0;
  for (const ir::ControlNode& node : function.nodes()) {
    if (node.opcode == ir::ControlOpcode::kCall) {
      maximum_call_arguments =
          std::max(maximum_call_arguments,
                   static_cast<std::size_t>(node.argument_count));
    }
  }
  ControlFlowRegisterAllocation allocation = allocate_control_flow_registers(
      function, kAllocationRegisters.size(), kFloatAllocationRegisters.size(),
      requirements, true);
  if (!allocation.status.ok()) {
    return {allocation.status, {}, 0, {}};
  }
  const std::size_t spill_slots =
      allocation.stack_slots + maximum_block_parameters;
  const std::size_t context_slot = spill_slots;
  const std::size_t call_argument_base =
      context_slot + static_cast<std::size_t>(has_context_operations);
  const std::size_t total_slots =
      call_argument_base + maximum_call_arguments;
  const std::size_t raw_stack_size = total_slots * sizeof(ir::Word);
  const std::size_t stack_size = (raw_stack_size + 15U) & ~std::size_t{15U};
  if (stack_size > kMaximumStackSize) {
    return {{StatusCode::kResourceExhausted,
             "x86-64 CFG spill frame exceeds the backend limit"},
            {},
            0};
  }

  StackMapLiveness stack_map_liveness =
      plan_stack_map_liveness(function, requirements);
  if (!stack_map_liveness.status.ok()) {
    return {stack_map_liveness.status, {}, 0, {}};
  }

  Assembler assembler;
  assembler.move_register(kArgumentBaseRegister, kArgumentRegister);
  if (stack_size != 0) {
    assembler.reserve_stack(stack_size);
  }
  if (has_context_operations) {
    assembler.store(kContextArgumentRegister, kRsp,
                    context_slot * sizeof(ir::Word));
  }

  const std::size_t no_label = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> labels(function.blocks().size(), no_label);
  std::vector<BranchFixup> fixups;
  std::vector<StackMapRecord> stack_maps;
  const std::size_t temporary_base = allocation.stack_slots;

  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    labels[block_index] = assembler.size();
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (const ir::Value value : block.instructions) {
      const ir::ControlNode& node = function.nodes()[value.id()];
      const std::size_t destination_offset =
          allocation.stack_indices[value.id()] == ValueLocation::kNone
              ? 0
              : control_value_offset(allocation, value);
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
          node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
          node.opcode == ir::ControlOpcode::kGuardFloatNonzero) {
        spill_control_stack_map_values(&assembler, function, allocation,
                                       live_values, block_index);
        stack_maps.push_back(make_stack_map_record(
            function, node, allocation, live_values, assembler.size(),
            stack_size));
      }
      switch (node.opcode) {
        case ir::ControlOpcode::kParameter:
          if (destination_is_float) {
            assembler.load_float(
                float_destination, kArgumentBaseRegister,
                static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
            if (allocated_float < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store_float(float_destination, kRsp,
                                    destination_offset);
            }
          } else {
            assembler.load(
                word_destination, kArgumentBaseRegister,
                static_cast<std::size_t>(node.immediate) * sizeof(ir::Word));
            if (allocated_word < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kRsp, destination_offset);
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
              assembler.store_float(float_destination, kRsp,
                                    destination_offset);
            }
          } else {
            assembler.move_immediate(word_destination, node.immediate);
            if (allocated_word < 0 ||
                allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kRsp, destination_offset);
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
              assembler.store_float(source, kRsp, argument_offset);
            } else {
              const int source = load_control_word(
                  &assembler, allocation, argument, block_index, kScratch0);
              assembler.store(source, kRsp, argument_offset);
            }
          }
          assembler.address(kRuntimeArgument0, kRsp,
                            call_argument_base * sizeof(ir::Word));
          assembler.move_immediate(
              kRuntimeArgument1, static_cast<ir::Word>(node.argument_count));
          assembler.move_immediate(kScratch0, node.immediate);
          assembler.reserve_stack(kCallStackAdjustment);
          assembler.call_register(kScratch0);
          assembler.release_stack(kCallStackAdjustment);
          if (destination_is_float) {
            if (allocated_float >= 0) {
              assembler.move_word_to_float(float_destination,
                                           kReturnRegister);
              if (allocation.requires_stack[value.id()]) {
                assembler.store_float(float_destination, kRsp,
                                      destination_offset);
              }
            } else {
              assembler.store(kReturnRegister, kRsp, destination_offset);
            }
          } else if (allocated_word >= 0) {
            assembler.move_register(word_destination, kReturnRegister);
            if (allocation.requires_stack[value.id()]) {
              assembler.store(word_destination, kRsp, destination_offset);
            }
          } else {
            assembler.store(kReturnRegister, kRsp, destination_offset);
          }
          restore_control_live_across_call(&assembler, function, allocation,
                                           value, block_index);
          break;
        case ir::ControlOpcode::kGuardWordNonzero:
        case ir::ControlOpcode::kGuardFloatNonzero: {
          std::size_t nonzero = 0;
          if (node.opcode == ir::ControlOpcode::kGuardWordNonzero) {
            const int source = load_control_word(
                &assembler, allocation, node.lhs, block_index, kScratch0);
            assembler.move_register(kScratch1, source);
            nonzero = assembler.branch_nonzero(kScratch1);
          } else {
            const int source = load_control_float(
                &assembler, allocation, node.lhs, block_index,
                kFloatScratch0);
            assembler.move_float_to_word(kScratch0, source);
            assembler.move_register(kScratch1, kScratch0);
            assembler.add(kScratch0, kScratch0, kScratch0);
            nonzero = assembler.branch_nonzero(kScratch0);
          }

          assembler.load(kScratch0, kRsp,
                         context_slot * sizeof(ir::Word));
          const std::size_t no_context = assembler.branch_zero(kScratch0);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_value_offset());
          capture_control_stack_map_values(&assembler, allocation,
                                           live_values, kScratch0);
          assembler.move_immediate(kScratch1, node.immediate);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_site_offset());
          assembler.move_immediate(
              kScratch1,
              static_cast<ir::Word>(runtime::ExitReason::kRuntime));
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_reason_offset());

          const std::size_t exit = assembler.size();
          assembler.move_immediate(kReturnRegister, 0);
          if (stack_size != 0) {
            assembler.release_stack(stack_size);
          }
          assembler.return_to_caller();

          const std::size_t resume = assembler.size();
          const Status context_status =
              assembler.patch_branch(no_context, exit);
          if (!context_status.ok()) {
            return {context_status, {}, 0};
          }
          const Status guard_status = assembler.patch_branch(nonzero, resume);
          if (!guard_status.ok()) {
            return {guard_status, {}, 0};
          }
          assembler.move_immediate(word_destination, 0);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kRsp, destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kSafepoint: {
          assembler.load(kScratch0, kRsp, context_slot * sizeof(ir::Word));
          const std::size_t no_context = assembler.branch_zero(kScratch0);
          if (measure_safepoint_polls) {
            assembler.load(
                kScratch1, kScratch0,
                runtime::ExecutionContext::safepoint_polls_offset());
            assembler.increment(kScratch1);
            assembler.store(
                kScratch1, kScratch0,
                runtime::ExecutionContext::safepoint_polls_offset());
          }
          assembler.load(
              kScratch0, kScratch0,
              runtime::ExecutionContext::interrupt_requested_offset());
          const std::size_t not_interrupted = assembler.branch_zero(kScratch0);
          assembler.load(kScratch0, kRsp, context_slot * sizeof(ir::Word));
          assembler.move_immediate(kScratch1, 0);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_value_offset());
          capture_control_stack_map_values(&assembler, allocation,
                                           live_values, kScratch0);
          assembler.move_immediate(kScratch1, node.immediate);
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_site_offset());
          assembler.move_immediate(
              kScratch1,
              static_cast<ir::Word>(runtime::ExitReason::kSafepoint));
          assembler.store(kScratch1, kScratch0,
                          runtime::ExecutionContext::exit_reason_offset());
          assembler.move_immediate(kReturnRegister, 0);
          if (stack_size != 0) {
            assembler.release_stack(stack_size);
          }
          assembler.return_to_caller();

          const std::size_t resume = assembler.size();
          const Status context_status =
              assembler.patch_branch(no_context, resume);
          if (!context_status.ok()) {
            return {context_status, {}, 0};
          }
          const Status interrupt_status =
              assembler.patch_branch(not_interrupted, resume);
          if (!interrupt_status.ok()) {
            return {interrupt_status, {}, 0};
          }
          assembler.move_immediate(word_destination, 0);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kRsp, destination_offset);
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
            assembler.store_float(float_destination, kRsp,
                                  destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kFloatNegate: {
          const int source = load_control_float(
              &assembler, allocation, node.lhs, block_index, kFloatScratch0);
          assembler.negate_float(float_destination, source);
          if (allocated_float < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store_float(float_destination, kRsp,
                                  destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kNegate:
        case ir::ControlOpcode::kBitwiseNot: {
          const int source = load_control_word(
              &assembler, allocation, node.lhs, block_index, kScratch0);
          if (node.opcode == ir::ControlOpcode::kNegate) {
            assembler.negate(word_destination, source);
          } else {
            assembler.bitwise_not(word_destination, source);
          }
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kRsp, destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kFloatLessThan:
        case ir::ControlOpcode::kFloatLessEqual:
        case ir::ControlOpcode::kFloatEqual:
        case ir::ControlOpcode::kFloatNotEqual: {
          const int lhs = load_control_float(
              &assembler, allocation, node.lhs, block_index, kFloatScratch0);
          const int rhs = load_control_float(
              &assembler, allocation, node.rhs, block_index, kFloatScratch1);
          FloatCondition condition = FloatCondition::kLessThan;
          if (node.opcode == ir::ControlOpcode::kFloatLessEqual) {
            condition = FloatCondition::kLessEqual;
          } else if (node.opcode == ir::ControlOpcode::kFloatEqual) {
            condition = FloatCondition::kEqual;
          } else if (node.opcode == ir::ControlOpcode::kFloatNotEqual) {
            condition = FloatCondition::kNotEqual;
          }
          assembler.compare_float(word_destination, lhs, rhs, condition);
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kRsp, destination_offset);
          }
          break;
        }
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kBitwiseAnd:
        case ir::ControlOpcode::kBitwiseOr:
        case ir::ControlOpcode::kBitwiseXor:
        case ir::ControlOpcode::kShiftLeft:
        case ir::ControlOpcode::kFloorDivide:
        case ir::ControlOpcode::kFloorModulo:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
        case ir::ControlOpcode::kEqual:
        case ir::ControlOpcode::kNotEqual:
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
          } else if (node.opcode == ir::ControlOpcode::kBitwiseAnd) {
            assembler.bitwise_and(word_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kBitwiseOr) {
            assembler.bitwise_or(word_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kBitwiseXor) {
            assembler.bitwise_xor(word_destination, lhs, rhs);
          } else if (node.opcode == ir::ControlOpcode::kShiftLeft) {
            const Status shift_status =
                assembler.shift_left(word_destination, lhs, rhs);
            if (!shift_status.ok()) {
              return {shift_status, {}, 0};
            }
          } else if (node.opcode == ir::ControlOpcode::kFloorDivide ||
                     node.opcode == ir::ControlOpcode::kFloorModulo) {
            const Status floor_status = assembler.floor_arithmetic(
                kReturnRegister, lhs, rhs,
                node.opcode == ir::ControlOpcode::kFloorModulo);
            if (!floor_status.ok()) {
              return {floor_status, {}, 0};
            }
            if (word_destination != kReturnRegister) {
              assembler.move_register(word_destination, kReturnRegister);
            }
          } else {
            WordCondition condition = WordCondition::kLessThan;
            if (node.opcode == ir::ControlOpcode::kLessEqual) {
              condition = WordCondition::kLessEqual;
            } else if (node.opcode == ir::ControlOpcode::kEqual) {
              condition = WordCondition::kEqual;
            } else if (node.opcode == ir::ControlOpcode::kNotEqual) {
              condition = WordCondition::kNotEqual;
            }
            assembler.compare(word_destination, lhs, rhs, condition);
          }
          if (allocated_word < 0 ||
              allocation.requires_stack[value.id()]) {
            assembler.store(word_destination, kRsp, destination_offset);
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
        assembler.move_float_to_word(kReturnRegister, returned);
      } else {
        const int returned = load_control_word(
            &assembler, allocation, terminator.value, block_index, kScratch0);
        if (returned != kReturnRegister) {
          assembler.move_register(kReturnRegister, returned);
        }
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
          assembler.patch_branch(true_selector, true_copy);
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
  return {Status::ok_status(), assembler.take_code(), total_slots,
          std::move(stack_maps)};
}

}  // namespace

LoweringResult lower(const ir::Function& function,
                     const StackMapRequirements& requirements,
                     bool measure_safepoint_polls) {
  try {
    return lower_impl(function, requirements, measure_safepoint_polls);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate x86-64 lowering state"},
            {},
            0};
  }
}

LoweringResult lower(const ir::ControlFlowFunction& function,
                     const StackMapRequirements& requirements,
                     bool measure_safepoint_polls) {
  try {
    return lower_control_flow_impl(function, requirements,
                                   measure_safepoint_polls);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate x86-64 CFG lowering state"},
            {},
            0};
  }
}

}  // namespace unijit::jit::detail::x86_64
