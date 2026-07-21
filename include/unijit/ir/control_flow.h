#ifndef UNIJIT_IR_CONTROL_FLOW_H
#define UNIJIT_IR_CONTROL_FLOW_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/status.h"

namespace unijit::ir {

class Block final {
public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr Block() noexcept = default;
  explicit constexpr Block(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(Block lhs, Block rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(Block lhs, Block rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  std::uint32_t id_{kInvalidId};
};

enum class ControlOpcode : std::uint8_t {
  kParameter,
  kBlockParameter,
  kConstant,
  kAdd,
  kSubtract,
  kMultiply,
  kFloatAdd,
  kFloatSubtract,
  kFloatNegate,
  kFloatMultiply,
  kFloatDivide,
  kFloatLessThan,
  kFloatLessEqual,
  kFloatEqual,
  kFloatNotEqual,
  kLessThan,
  kLessEqual,
  kCall,
  kGuardFloatNonzero,
  kSafepoint,
};

struct ControlNode final {
  ControlOpcode opcode{ControlOpcode::kConstant};
  Value lhs;
  Value rhs;
  Word immediate{0};
  ValueType type{ValueType::kWord};
  std::uint32_t argument_begin{0};
  std::uint32_t argument_count{0};
};

struct ControlEdge final {
  Block target;
  std::vector<Value> arguments;
};

enum class TerminatorOpcode : std::uint8_t {
  kNone,
  kReturn,
  kJump,
  kBranch,
};

struct ControlTerminator final {
  TerminatorOpcode opcode{TerminatorOpcode::kNone};
  Value value;
  ControlEdge true_edge;
  ControlEdge false_edge;
};

struct BasicBlock final {
  std::vector<Value> parameters;
  std::vector<Value> instructions;
  ControlTerminator terminator;
};

class ControlFlowFunction final {
public:
  ControlFlowFunction() = default;

  std::size_t parameter_count() const noexcept { return parameter_count_; }
  ValueType parameter_type(std::size_t index) const noexcept {
    return index < parameter_types_.size() ? parameter_types_[index]
                                           : ValueType::kWord;
  }
  ValueType value_type(Value value) const noexcept {
    return value.valid() && value.id() < nodes_.size()
               ? nodes_[value.id()].type
               : ValueType::kWord;
  }
  ValueType return_type() const noexcept {
    for (const BasicBlock &block : blocks_) {
      if (block.terminator.opcode == TerminatorOpcode::kReturn) {
        return value_type(block.terminator.value);
      }
    }
    return ValueType::kWord;
  }
  Block entry_block() const noexcept { return entry_block_; }
  const std::vector<ControlNode> &nodes() const noexcept { return nodes_; }
  const std::vector<Value> &call_arguments() const noexcept {
    return call_arguments_;
  }
  const std::vector<BasicBlock> &blocks() const noexcept { return blocks_; }

private:
  friend class ControlFlowBuilder;

  std::size_t parameter_count_{0};
  std::vector<ValueType> parameter_types_;
  Block entry_block_;
  std::vector<ControlNode> nodes_;
  std::vector<Value> call_arguments_;
  std::vector<BasicBlock> blocks_;
};

class ControlFlowBuilder final {
public:
  explicit ControlFlowBuilder(std::size_t parameter_count);
  explicit ControlFlowBuilder(std::vector<ValueType> parameter_types);

  Block entry_block() const noexcept { return function_.entry_block_; }
  Block insertion_block() const noexcept { return insertion_block_; }
  Value parameter(std::size_t index) const noexcept;
  Value block_parameter(Block block, std::size_t index) const noexcept;

  Block create_block(std::size_t parameter_count);
  Block create_block(std::vector<ValueType> parameter_types);
  Status set_insertion_block(Block block);

  Value constant(Word value);
  Value float64_constant(double value);
  Value float64_constant_bits(Word bits);
  Value add(Value lhs, Value rhs);
  Value subtract(Value lhs, Value rhs);
  Value multiply(Value lhs, Value rhs);
  Value float64_add(Value lhs, Value rhs);
  Value float64_subtract(Value lhs, Value rhs);
  Value float64_negate(Value value);
  Value float64_multiply(Value lhs, Value rhs);
  Value float64_divide(Value lhs, Value rhs);
  Value float64_less_than(Value lhs, Value rhs);
  Value float64_less_equal(Value lhs, Value rhs);
  Value float64_equal(Value lhs, Value rhs);
  Value float64_not_equal(Value lhs, Value rhs);
  Value less_than(Value lhs, Value rhs);
  Value less_equal(Value lhs, Value rhs);
  Value call(RuntimeHelper helper, std::vector<Value> arguments,
             ValueType result_type = ValueType::kWord);
  Value guard_float64_nonzero(Value value, std::size_t site);
  Value safepoint(std::size_t site);

  Status set_return(Value value);
  Status jump(Block target, std::vector<Value> arguments);
  Status branch(Value condition, Block true_target,
                std::vector<Value> true_arguments, Block false_target,
                std::vector<Value> false_arguments);

  ControlFlowFunction build() && noexcept;

private:
  Value append_node(ControlNode node);
  Value append_unary(ControlOpcode opcode, Value value,
                     ValueType type = ValueType::kWord);
  Value append_binary(ControlOpcode opcode, Value lhs, Value rhs,
                      ValueType type = ValueType::kWord);
  Status validate_edge(Block target, const std::vector<Value> &arguments) const;
  Status set_terminator(ControlTerminator terminator);

  ControlFlowFunction function_;
  Block insertion_block_;
};

Status verify(const ControlFlowFunction &function);

class ControlFlowInterpreter final {
public:
  static EvaluationResult
  evaluate(const ControlFlowFunction &function, const Word *args,
           std::size_t arg_count,
           std::size_t maximum_block_executions = 1000000,
           runtime::ExecutionContext *context = nullptr);

  static EvaluationResult
  evaluate(const ControlFlowFunction &function, const std::vector<Word> &args,
           std::size_t maximum_block_executions = 1000000,
           runtime::ExecutionContext *context = nullptr) {
    return evaluate(function, args.data(), args.size(),
                    maximum_block_executions, context);
  }
};

} // namespace unijit::ir

#endif // UNIJIT_IR_CONTROL_FLOW_H
