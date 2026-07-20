#ifndef UNIJIT_IR_FUNCTION_H
#define UNIJIT_IR_FUNCTION_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/status.h"

namespace unijit::ir {

using Word = std::int64_t;

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
};

struct Node final {
  Opcode opcode{Opcode::kConstant};
  Value lhs;
  Value rhs;
  Word immediate{0};
};

class Function final {
 public:
  Function() = default;

  std::size_t parameter_count() const noexcept { return parameter_count_; }
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  Value return_value() const noexcept { return return_value_; }

 private:
  friend class FunctionBuilder;

  std::size_t parameter_count_{0};
  std::vector<Node> nodes_;
  Value return_value_;
};

class FunctionBuilder final {
 public:
  explicit FunctionBuilder(std::size_t parameter_count);

  std::size_t parameter_count() const noexcept {
    return function_.parameter_count_;
  }

  Value parameter(std::size_t index) const noexcept;
  Value constant(Word value);
  Value add(Value lhs, Value rhs);
  Value subtract(Value lhs, Value rhs);
  Value multiply(Value lhs, Value rhs);
  Status set_return(Value value);

  Function build() && noexcept;

 private:
  Value append_binary(Opcode opcode, Value lhs, Value rhs);

  Function function_;
};

Status verify(const Function& function);

}  // namespace unijit::ir

#endif  // UNIJIT_IR_FUNCTION_H
