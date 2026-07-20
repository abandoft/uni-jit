#include "unijit/ir/function.h"

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

FunctionBuilder::FunctionBuilder(std::size_t parameter_count)
    : FunctionBuilder(std::vector<ValueType>(parameter_count,
                                             ValueType::kWord)) {}

FunctionBuilder::FunctionBuilder(std::vector<ValueType> parameter_types) {
  const auto max_parameters =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  function_.parameter_count_ =
      parameter_types.size() > max_parameters ? max_parameters
                                              : parameter_types.size();
  parameter_types.resize(function_.parameter_count_);
  function_.parameter_types_ = std::move(parameter_types);
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
      opcode == Opcode::kFloatMultiply;
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

Value FunctionBuilder::float64_add(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatAdd, lhs, rhs);
}

Value FunctionBuilder::float64_subtract(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatSubtract, lhs, rhs);
}

Value FunctionBuilder::float64_multiply(Value lhs, Value rhs) {
  return append_binary(Opcode::kFloatMultiply, lhs, rhs);
}

Status FunctionBuilder::set_return(Value value) {
  if (!value.valid() || value.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "return value is not in the function"};
  }
  function_.return_value_ = value;
  return Status::ok_status();
}

Function FunctionBuilder::build() && noexcept { return std::move(function_); }

}  // namespace unijit::ir
