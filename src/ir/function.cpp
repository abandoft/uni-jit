#include "unijit/ir/function.h"

#include <limits>
#include <utility>

namespace unijit::ir {

FunctionBuilder::FunctionBuilder(std::size_t parameter_count) {
  const auto max_parameters =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  function_.parameter_count_ =
      parameter_count > max_parameters ? max_parameters : parameter_count;
  function_.nodes_.reserve(function_.parameter_count_);

  for (std::size_t index = 0; index < function_.parameter_count_; ++index) {
    function_.nodes_.push_back(
        Node{Opcode::kParameter, {}, {}, static_cast<Word>(index)});
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

Value FunctionBuilder::append_binary(Opcode opcode, Value lhs, Value rhs) {
  if (function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.nodes_.size());
  function_.nodes_.push_back(Node{opcode, lhs, rhs, 0});
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

Status FunctionBuilder::set_return(Value value) {
  if (!value.valid() || value.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "return value is not in the function"};
  }
  function_.return_value_ = value;
  return Status::ok_status();
}

Function FunctionBuilder::build() && noexcept { return std::move(function_); }

}  // namespace unijit::ir
