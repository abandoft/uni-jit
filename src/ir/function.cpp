#include "unijit/ir/function.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace unijit::ir {

Word pack_float64(double value) noexcept {
  Word bits = 0;
  static_assert(sizeof(bits) == sizeof(value),
                "Float64 values must occupy one IR word");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

double unpack_float64(Word bits) noexcept {
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

Word pack_runtime_helper(RuntimeHelper helper) noexcept {
  Word bits = 0;
  static_assert(sizeof(bits) == sizeof(helper),
                "runtime helpers must fit in one IR word");
  std::memcpy(&bits, &helper, sizeof(bits));
  return bits;
}

RuntimeHelper unpack_runtime_helper(Word bits) noexcept {
  RuntimeHelper helper = nullptr;
  std::memcpy(&helper, &bits, sizeof(helper));
  return helper;
}

Word floor_divide_word(Word lhs, Word rhs) noexcept {
  if (rhs == 0) {
    return 0;
  }
  if (rhs == -1) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &lhs, sizeof(bits));
    bits = UINT64_C(0) - bits;
    Word result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }
  Word quotient = lhs / rhs;
  if ((lhs ^ rhs) < 0 && lhs % rhs != 0) {
    --quotient;
  }
  return quotient;
}

Word floor_modulo_word(Word lhs, Word rhs) noexcept {
  if (rhs == 0 || rhs == -1) {
    return 0;
  }
  Word remainder = lhs % rhs;
  if (remainder != 0 && (remainder ^ rhs) < 0) {
    remainder += rhs;
  }
  return remainder;
}

Word byte_swap_word(Word value, MemoryWidth width) noexcept {
  if (width != MemoryWidth::k16 && width != MemoryWidth::k32 &&
      width != MemoryWidth::k64) {
    return 0;
  }
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const std::size_t byte_count = memory_width_bytes(width);
  std::uint64_t reversed = 0;
  for (std::size_t index = 0; index < byte_count; ++index) {
    reversed = (reversed << 8U) | ((bits >> (index * 8U)) & UINT64_C(0xFF));
  }
  Word result = 0;
  std::memcpy(&result, &reversed, sizeof(result));
  return result;
}

FunctionBuilder::FunctionBuilder(std::size_t parameter_count,
                                 std::size_t memory_region_count)
    : FunctionBuilder(std::vector<ValueType>(parameter_count,
                                             ValueType::kWord),
                      memory_region_count) {}

FunctionBuilder::FunctionBuilder(std::vector<ValueType> parameter_types,
                                 std::size_t memory_region_count) {
  const auto max_parameters =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  function_.parameter_count_ =
      parameter_types.size() > max_parameters ? max_parameters
                                              : parameter_types.size();
  parameter_types.resize(function_.parameter_count_);
  function_.parameter_types_ = std::move(parameter_types);
  function_.memory_region_count_ = memory_region_count;
  function_.nodes_.reserve(function_.parameter_count_);

  for (std::size_t index = 0; index < function_.parameter_count_; ++index) {
    function_.nodes_.push_back(Node{Opcode::kParameter, {}, {},
                                    static_cast<Word>(index),
                                    function_.parameter_types_[index]});
  }
}

Value FunctionBuilder::parameter(std::size_t index) const noexcept {
  if (index >= function_.parameter_count_) {
    return {};
  }
  return Value{static_cast<std::uint32_t>(index)};
}

Value FunctionBuilder::constant(Word value) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kConstant, {}, {}, value});
  return Value{id};
}

Value FunctionBuilder::float64_constant(double value) {
  return float64_constant_bits(pack_float64(value));
}

Value FunctionBuilder::float64_constant_bits(Word bits) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kConstant, {}, {}, bits, ValueType::kFloat64});
  return Value{id};
}

Value FunctionBuilder::append_binary(Opcode opcode, Value lhs, Value rhs) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  const bool is_float =
      opcode == Opcode::kFloatAdd || opcode == Opcode::kFloatSubtract ||
      opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide;
  function_.nodes_.push_back(
      Node{opcode, lhs, rhs, 0,
           is_float ? ValueType::kFloat64 : ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::add(Value lhs, Value rhs) {
  return append_binary(Opcode::kAdd, lhs, rhs);
}

Value FunctionBuilder::subtract(Value lhs, Value rhs) {
  return append_binary(Opcode::kSubtract, lhs, rhs);
}

Value FunctionBuilder::multiply(Value lhs, Value rhs) {
  return append_binary(Opcode::kMultiply, lhs, rhs);
}

Value FunctionBuilder::bitwise_and(Value lhs, Value rhs) {
  return append_binary(Opcode::kBitwiseAnd, lhs, rhs);
}

Value FunctionBuilder::bitwise_or(Value lhs, Value rhs) {
  return append_binary(Opcode::kBitwiseOr, lhs, rhs);
}

Value FunctionBuilder::bitwise_xor(Value lhs, Value rhs) {
  return append_binary(Opcode::kBitwiseXor, lhs, rhs);
}

Value FunctionBuilder::shift_left(Value value, Value amount) {
  return append_binary(Opcode::kShiftLeft, value, amount);
}

Value FunctionBuilder::floor_divide(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloorDivide, lhs, rhs);
}

Value FunctionBuilder::floor_modulo(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloorModulo, lhs, rhs);
}

Value FunctionBuilder::less_than(Value lhs, Value rhs) {
  return append_binary(Opcode::kLessThan, lhs, rhs);
}

Value FunctionBuilder::less_equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kLessEqual, lhs, rhs);
}

Value FunctionBuilder::equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kEqual, lhs, rhs);
}

Value FunctionBuilder::not_equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kNotEqual, lhs, rhs);
}

Value FunctionBuilder::negate(Value value) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kNegate, value, {}, 0, ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::bitwise_not(Value value) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kBitwiseNot, value, {}, 0, ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::byte_swap(Value value, MemoryWidth width) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kByteSwap,
                                  value,
                                  {},
                                  static_cast<Word>(width), ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::float64_add(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatAdd, lhs, rhs);
}

Value FunctionBuilder::float64_subtract(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatSubtract, lhs, rhs);
}

Value FunctionBuilder::float64_negate(Value value) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kFloatNegate, value, {}, 0, ValueType::kFloat64});
  return Value{id};
}

Value FunctionBuilder::float64_multiply(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatMultiply, lhs, rhs);
}

Value FunctionBuilder::float64_divide(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatDivide, lhs, rhs);
}

Value FunctionBuilder::float64_less_than(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatLessThan, lhs, rhs);
}

Value FunctionBuilder::float64_less_equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatLessEqual, lhs, rhs);
}

Value FunctionBuilder::float64_equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatEqual, lhs, rhs);
}

Value FunctionBuilder::float64_not_equal(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatNotEqual, lhs, rhs);
}

Value FunctionBuilder::guard_float64_nonzero(Value value, std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kGuardFloatNonzero, value, {},
                                  static_cast<Word>(site)});
  return Value{id};
}

Value FunctionBuilder::guard_word_nonzero(Value value, std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kGuardWordNonzero, value, {},
                                  static_cast<Word>(site)});
  return Value{id};
}

Value FunctionBuilder::call(RuntimeHelper helper,
                            std::vector<Value> arguments,
                            ValueType result_type) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      arguments.size() > std::numeric_limits<std::uint32_t>::max() ||
      function_.call_arguments_.size() >
          std::numeric_limits<std::uint32_t>::max() - arguments.size()) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  const auto argument_begin =
      static_cast<std::uint32_t>(function_.call_arguments_.size());
  const auto argument_count = static_cast<std::uint32_t>(arguments.size());
  function_.call_arguments_.insert(function_.call_arguments_.end(),
                                   arguments.begin(), arguments.end());
  function_.nodes_.push_back(Node{Opcode::kCall,
                                  {},
                                  {},
                                  pack_runtime_helper(helper),
                                  result_type,
                                  argument_begin,
                                  argument_count});
  return Value{id};
}

Value FunctionBuilder::safepoint(std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kSafepoint, {}, {}, static_cast<Word>(site)});
  return Value{id};
}

Value FunctionBuilder::load_word(Value byte_offset,
                                 MemoryAccessDescriptor access,
                                 std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kLoadWord, byte_offset, {}, static_cast<Word>(site),
           ValueType::kWord, 0, 0, access_index});
  return Value{id};
}

Value FunctionBuilder::store_word(Value byte_offset, Value value,
                                  MemoryAccessDescriptor access,
                                  std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kStoreWord, byte_offset, value, static_cast<Word>(site),
           ValueType::kWord, 0, 0, access_index});
  return Value{id};
}

Value FunctionBuilder::load_float(Value byte_offset,
                                  MemoryAccessDescriptor access,
                                  std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kLoadFloat,
                                  byte_offset,
                                  {},
                                  static_cast<Word>(site),
                                  ValueType::kFloat64,
                                  0,
                                  0,
                                  access_index});
  return Value{id};
}

Value FunctionBuilder::store_float(Value byte_offset, Value value,
                                   MemoryAccessDescriptor access,
                                   std::size_t site) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kStoreFloat, byte_offset, value,
                                  static_cast<Word>(site), ValueType::kFloat64,
                                  0, 0, access_index});
  return Value{id};
}

FrameSlot FunctionBuilder::create_frame_slot(ValueType type, bool sensitive) {
  if (function_.frame_slots_.size() >= FrameSlot::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.frame_slots_.size());
  function_.frame_slots_.push_back(FrameSlotDescriptor{type, sensitive});
  return FrameSlot{id};
}

Value FunctionBuilder::load_frame(FrameSlot slot) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType type = slot.valid() && slot.id() < function_.frame_slots_.size()
                             ? function_.frame_slots_[slot.id()].type
                             : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  Node node;
  node.opcode = Opcode::kLoadFrame;
  node.type = type;
  node.frame_slot = slot.id();
  function_.nodes_.push_back(node);
  return Value{id};
}

Value FunctionBuilder::store_frame(FrameSlot slot, Value value) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType type = slot.valid() && slot.id() < function_.frame_slots_.size()
                             ? function_.frame_slots_[slot.id()].type
                             : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  Node node;
  node.opcode = Opcode::kStoreFrame;
  node.lhs = value;
  node.type = type;
  node.frame_slot = slot.id();
  function_.nodes_.push_back(node);
  return Value{id};
}

TrustedObjectSlot FunctionBuilder::create_trusted_object(
    std::uint64_t layout_identity, std::size_t byte_size) {
  if (function_.trusted_objects_.size() >= TrustedObjectSlot::kInvalidId) {
    return {};
  }
  const auto id =
      static_cast<std::uint32_t>(function_.trusted_objects_.size());
  function_.trusted_objects_.push_back(
      TrustedObjectDescriptor{layout_identity, byte_size});
  return TrustedObjectSlot{id};
}

Value FunctionBuilder::load_object(TrustedObjectSlot object,
                                   std::size_t byte_offset,
                                   ValueType type) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      byte_offset > static_cast<std::size_t>(
                        std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  Node node;
  node.opcode = Opcode::kLoadObject;
  node.immediate = static_cast<Word>(byte_offset);
  node.type = type;
  node.trusted_object = object.id();
  function_.nodes_.push_back(node);
  return Value{id};
}

Value FunctionBuilder::store_object(TrustedObjectSlot object,
                                    std::size_t byte_offset, Value value) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      byte_offset > static_cast<std::size_t>(
                        std::numeric_limits<Word>::max())) {
    return {};
  }
  const ValueType type =
      value.valid() && value.id() < function_.nodes_.size()
          ? function_.nodes_[value.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  Node node;
  node.opcode = Opcode::kStoreObject;
  node.lhs = value;
  node.immediate = static_cast<Word>(byte_offset);
  node.type = type;
  node.trusted_object = object.id();
  function_.nodes_.push_back(node);
  return Value{id};
}

Value FunctionBuilder::vector_constant(ValueType type, Vector128 bits) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.vector_constants_.size() >= VectorShuffle::kInvalidIndex) {
    return {};
  }
  const auto constant_index =
      static_cast<std::uint32_t>(function_.vector_constants_.size());
  function_.vector_constants_.push_back(bits);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorConstant, {}, {},
                                  static_cast<Word>(constant_index), type});
  return Value{id};
}

Value FunctionBuilder::vector_zero(ValueType type) {
  return vector_constant(type, Vector128{});
}

Value FunctionBuilder::vector_splat(ValueType type, Value lane_bits) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kVectorSplat, lane_bits, {}, 0, type});
  return Value{id};
}

Value FunctionBuilder::vector_extract_lane(Value vector, std::size_t lane,
                                           bool sign_extend) {
  if (function_.nodes_.size() >= Value::kInvalidId || lane > UINT8_MAX) {
    return {};
  }
  const Word immediate = static_cast<Word>(lane) |
                         (sign_extend ? static_cast<Word>(UINT64_C(1) << 8U)
                                      : 0);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorExtractLane, vector, {},
                                  immediate, ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::vector_insert_lane(Value vector, std::size_t lane,
                                          Value lane_bits) {
  if (function_.nodes_.size() >= Value::kInvalidId || lane > UINT8_MAX) {
    return {};
  }
  const ValueType type =
      vector.valid() && vector.id() < function_.nodes_.size()
          ? function_.nodes_[vector.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorInsertLane, vector, lane_bits,
                                  static_cast<Word>(lane), type});
  return Value{id};
}

Value FunctionBuilder::vector_unary(VectorUnaryOperation operation,
                                    Value vector) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType type =
      vector.valid() && vector.id() < function_.nodes_.size()
          ? function_.nodes_[vector.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorUnary, vector, {},
                                  static_cast<Word>(operation), type});
  return Value{id};
}

Value FunctionBuilder::vector_binary(VectorBinaryOperation operation,
                                     Value lhs, Value rhs) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType type = lhs.valid() && lhs.id() < function_.nodes_.size()
                             ? function_.nodes_[lhs.id()].type
                             : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorBinary, lhs, rhs,
                                  static_cast<Word>(operation), type});
  return Value{id};
}

Value FunctionBuilder::vector_compare(VectorComparison comparison, Value lhs,
                                      Value rhs) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType input_type =
      lhs.valid() && lhs.id() < function_.nodes_.size()
          ? function_.nodes_[lhs.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorCompare, lhs, rhs,
                                  static_cast<Word>(comparison),
                                  vector_mask_type(input_type)});
  return Value{id};
}

Value FunctionBuilder::vector_select(Value mask, Value true_value,
                                     Value false_value) {
  if (function_.nodes_.size() >= Value::kInvalidId ||
      function_.vector_select_arguments_.size() >= Value::kInvalidId) {
    return {};
  }
  const ValueType type =
      true_value.valid() && true_value.id() < function_.nodes_.size()
          ? function_.nodes_[true_value.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  const auto select_index = static_cast<std::uint32_t>(
      function_.vector_select_arguments_.size());
  function_.vector_select_arguments_.push_back(false_value);
  function_.nodes_.push_back(Node{Opcode::kVectorSelect, mask, true_value,
                                  static_cast<Word>(select_index), type});
  return Value{id};
}

Value FunctionBuilder::vector_lane_sign_mask(Value vector) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorLaneSignMask, vector, {}, 0,
                                  ValueType::kWord});
  return Value{id};
}

Value FunctionBuilder::vector_shuffle(Value vector,
                                      std::vector<std::uint8_t> lanes) {
  if (function_.nodes_.size() >= Value::kInvalidId || lanes.size() > 16 ||
      function_.vector_shuffles_.size() >= VectorShuffle::kInvalidIndex) {
    return {};
  }
  VectorShuffle shuffle;
  shuffle.lane_count = static_cast<std::uint8_t>(lanes.size());
  std::copy(lanes.begin(), lanes.end(), shuffle.lanes.begin());
  const auto shuffle_index =
      static_cast<std::uint32_t>(function_.vector_shuffles_.size());
  function_.vector_shuffles_.push_back(shuffle);
  const ValueType type =
      vector.valid() && vector.id() < function_.nodes_.size()
          ? function_.nodes_[vector.id()].type
          : ValueType::kWord;
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{Opcode::kVectorShuffle, vector, {},
                                  static_cast<Word>(shuffle_index), type});
  return Value{id};
}

Value FunctionBuilder::vector_widen(Value vector, ValueType result_type,
                                    VectorExtension extension,
                                    VectorHalf half) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const Word immediate = static_cast<Word>(extension) |
                         (static_cast<Word>(half) << 8U);
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(
      Node{Opcode::kVectorWiden, vector, {}, immediate, result_type});
  return Value{id};
}

Status FunctionBuilder::set_return(Value value) {
  if (!value.valid() || value.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "return value is not in the function"};
  }
  if (!is_scalar_value_type(function_.nodes_[value.id()].type)) {
    return {StatusCode::kInvalidArgument,
            "the scalar invocation ABI cannot return a vector value"};
  }
  function_.return_value_ = value;
  return Status::ok_status();
}

Function FunctionBuilder::build() && noexcept { return std::move(function_); }

}  // namespace unijit::ir
