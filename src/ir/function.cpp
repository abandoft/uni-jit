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

Status FunctionBuilder::set_return(Value value) {
  if (!value.valid() || value.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "return value is not in the function"};
  }
  function_.return_value_ = value;
  return Status::ok_status();
}

Function FunctionBuilder::build() && noexcept { return std::move(function_); }

}  // namespace unijit::ir
