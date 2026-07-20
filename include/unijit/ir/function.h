#ifndef UNIJIT_IR_FUNCTION_H
#define UNIJIT_IR_FUNCTION_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/status.h"

namespace unijit::ir {

using Word = std::int64_t;
using RuntimeHelper = Word (*)(const Word* arguments, std::size_t count);

Word pack_float64(double value) noexcept;
double unpack_float64(Word bits) noexcept;
Word pack_runtime_helper(RuntimeHelper helper) noexcept;
RuntimeHelper unpack_runtime_helper(Word bits) noexcept;

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
  kFloatAdd,
  kFloatSubtract,
  kFloatMultiply,
  kFloatDivide,
  kCall,
  kSafepoint,
};

enum class ValueType : std::uint8_t {
  kWord,
  kFloat64,
};

struct Node final {
  Opcode opcode{Opcode::kConstant};
  Value lhs;
  Value rhs;
  Word immediate{0};
  ValueType type{ValueType::kWord};
  std::uint32_t argument_begin{0};
  std::uint32_t argument_count{0};
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
  Value return_value_;
};

class FunctionBuilder final {
 public:
  explicit FunctionBuilder(std::size_t parameter_count);
  explicit FunctionBuilder(std::vector<ValueType> parameter_types);

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
  Value float64_add(Value lhs, Value rhs);
  Value float64_subtract(Value lhs, Value rhs);
  Value float64_multiply(Value lhs, Value rhs);
  Value float64_divide(Value lhs, Value rhs);
  Value call(RuntimeHelper helper, std::vector<Value> arguments,
             ValueType result_type = ValueType::kWord);
  Value safepoint(std::size_t site);
  Status set_return(Value value);

  Function build() && noexcept;

 private:
  Value append_binary(Opcode opcode, Value lhs, Value rhs);

  Function function_;
};

Status verify(const Function& function);

}  // namespace unijit::ir

#endif  // UNIJIT_IR_FUNCTION_H
