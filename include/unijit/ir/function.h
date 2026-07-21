#ifndef UNIJIT_IR_FUNCTION_H
#define UNIJIT_IR_FUNCTION_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/status.h"
#include "unijit/ir/memory.h"

namespace unijit::ir {

using Word = std::int64_t;
using RuntimeHelper = Word (*)(const Word* arguments, std::size_t count);

Word pack_float64(double value) noexcept;
double unpack_float64(Word bits) noexcept;
Word pack_runtime_helper(RuntimeHelper helper) noexcept;
RuntimeHelper unpack_runtime_helper(Word bits) noexcept;
// Lua/Python-style signed floor arithmetic. Division by zero is totalized to
// zero so malformed or speculative IR cannot raise a host hardware trap;
// language frontends that require an error insert a Word nonzero guard.
Word floor_divide_word(Word lhs, Word rhs) noexcept;
Word floor_modulo_word(Word lhs, Word rhs) noexcept;
Word byte_swap_word(Word value, MemoryWidth width) noexcept;

class Value final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr Value() noexcept = default;
  explicit constexpr Value(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(Value lhs, Value rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(Value lhs, Value rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

enum class Opcode : std::uint8_t {
  kParameter,
  kConstant,
  kAdd,
  kSubtract,
  kMultiply,
  kBitwiseAnd,
  kBitwiseOr,
  kBitwiseXor,
  kShiftLeft,
  kFloorDivide,
  kFloorModulo,
  kLessThan,
  kLessEqual,
  kEqual,
  kNotEqual,
  kNegate,
  kBitwiseNot,
  kByteSwap,
  kFloatAdd,
  kFloatSubtract,
  kFloatNegate,
  kFloatMultiply,
  kFloatDivide,
  kFloatLessThan,
  kFloatLessEqual,
  kFloatEqual,
  kFloatNotEqual,
  kGuardWordNonzero,
  kGuardFloatNonzero,
  kCall,
  kSafepoint,
  kLoadWord,
  kStoreWord,
  kLoadFloat,
  kStoreFloat,
  kLoadFrame,
  kStoreFrame,
};

enum class ValueType : std::uint8_t {
  kWord,
  kFloat64,
};

class FrameSlot final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr FrameSlot() noexcept = default;
  explicit constexpr FrameSlot(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(FrameSlot lhs, FrameSlot rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(FrameSlot lhs, FrameSlot rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

struct FrameSlotDescriptor final {
  ValueType type{ValueType::kWord};
  bool sensitive{false};
};

struct Node final {
  Opcode opcode{Opcode::kConstant};
  Value lhs;
  Value rhs;
  Word immediate{0};
  ValueType type{ValueType::kWord};
  std::uint32_t argument_begin{0};
  std::uint32_t argument_count{0};
  std::uint32_t memory_access{MemoryAccessDescriptor::kInvalidIndex};
  std::uint32_t frame_slot{FrameSlot::kInvalidId};
};

class Function final {
 public:
  Function() = default;

  std::size_t parameter_count() const noexcept { return parameter_count_; }
  ValueType parameter_type(std::size_t index) const noexcept {
    return index < parameter_types_.size() ? parameter_types_[index]
                                           : ValueType::kWord;
  }
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  const std::vector<Value>& call_arguments() const noexcept {
    return call_arguments_;
  }
  std::size_t memory_region_count() const noexcept {
    return memory_region_count_;
  }
  const std::vector<MemoryAccessDescriptor>& memory_accesses() const noexcept {
    return memory_accesses_;
  }
  const std::vector<FrameSlotDescriptor>& frame_slots() const noexcept {
    return frame_slots_;
  }
  Value return_value() const noexcept { return return_value_; }
  ValueType return_type() const noexcept {
    return return_value_.valid() && return_value_.id() < nodes_.size()
               ? nodes_[return_value_.id()].type
               : ValueType::kWord;
  }
  ValueType value_type(Value value) const noexcept {
    return value.valid() && value.id() < nodes_.size()
               ? nodes_[value.id()].type
               : ValueType::kWord;
  }

 private:
  friend class FunctionBuilder;

  std::size_t parameter_count_{0};
  std::vector<ValueType> parameter_types_;
  std::vector<Node> nodes_;
  std::vector<Value> call_arguments_;
  std::size_t memory_region_count_{0};
  std::vector<MemoryAccessDescriptor> memory_accesses_;
  std::vector<FrameSlotDescriptor> frame_slots_;
  Value return_value_;
};

class FunctionBuilder final {
 public:
  explicit FunctionBuilder(std::size_t parameter_count,
                           std::size_t memory_region_count = 0);
  explicit FunctionBuilder(std::vector<ValueType> parameter_types,
                           std::size_t memory_region_count = 0);

  std::size_t parameter_count() const noexcept {
    return function_.parameter_count_;
  }

  Value parameter(std::size_t index) const noexcept;
  Value constant(Word value);
  Value float64_constant(double value);
  Value float64_constant_bits(Word bits);
  Value add(Value lhs, Value rhs);
  Value subtract(Value lhs, Value rhs);
  Value multiply(Value lhs, Value rhs);
  Value bitwise_and(Value lhs, Value rhs);
  Value bitwise_or(Value lhs, Value rhs);
  Value bitwise_xor(Value lhs, Value rhs);
  // Shifts left for nonnegative amounts and logically right for negative
  // amounts. Magnitudes of 64 or more produce zero.
  Value shift_left(Value value, Value amount);
  Value floor_divide(Value lhs, Value rhs);
  Value floor_modulo(Value lhs, Value rhs);
  Value less_than(Value lhs, Value rhs);
  Value less_equal(Value lhs, Value rhs);
  Value equal(Value lhs, Value rhs);
  Value not_equal(Value lhs, Value rhs);
  Value negate(Value value);
  Value bitwise_not(Value value);
  // Reverses the low 16, 32, or 64 bits and zero-extends narrower results.
  Value byte_swap(Value value, MemoryWidth width);
  Value float64_add(Value lhs, Value rhs);
  Value float64_subtract(Value lhs, Value rhs);
  Value float64_negate(Value value);
  Value float64_multiply(Value lhs, Value rhs);
  Value float64_divide(Value lhs, Value rhs);
  Value float64_less_than(Value lhs, Value rhs);
  Value float64_less_equal(Value lhs, Value rhs);
  Value float64_equal(Value lhs, Value rhs);
  Value float64_not_equal(Value lhs, Value rhs);
  Value guard_word_nonzero(Value value, std::size_t site);
  Value guard_float64_nonzero(Value value, std::size_t site);
  Value call(RuntimeHelper helper, std::vector<Value> arguments,
             ValueType result_type = ValueType::kWord);
  Value safepoint(std::size_t site);
  Value load_word(Value byte_offset, MemoryAccessDescriptor access,
                  std::size_t site);
  Value store_word(Value byte_offset, Value value,
                   MemoryAccessDescriptor access, std::size_t site);
  // Float32 storage converts to/from the IR Float64 value type. Float64
  // storage preserves all 64 payload bits. Only k32 and k64 are valid.
  Value load_float(Value byte_offset, MemoryAccessDescriptor access,
                   std::size_t site);
  Value store_float(Value byte_offset, Value value,
                    MemoryAccessDescriptor access, std::size_t site);
  // Frame slots are fixed-size, zero-initialized for each invocation, and
  // remain live for the complete generated frame. Sensitive slots are cleared
  // on every native return path.
  FrameSlot create_frame_slot(ValueType type, bool sensitive = false);
  Value load_frame(FrameSlot slot);
  Value store_frame(FrameSlot slot, Value value);
  Status set_return(Value value);

  Function build() && noexcept;

 private:
  Value append_binary(Opcode opcode, Value lhs, Value rhs);

  Function function_;
};

Status verify(const Function& function);

}  // namespace unijit::ir

#endif  // UNIJIT_IR_FUNCTION_H
