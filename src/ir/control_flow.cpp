#include "unijit/ir/control_flow.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace unijit::ir {
namespace {

bool valid_block(const ControlFlowFunction &function, Block block) {
  return block.valid() && block.id() < function.blocks().size();
}

bool valid_value(const ControlFlowFunction &function, Value value) {
  return value.valid() && value.id() < function.nodes().size();
}

bool is_binary(ControlOpcode opcode) {
  switch (opcode) {
  case ControlOpcode::kAdd:
  case ControlOpcode::kSubtract:
  case ControlOpcode::kMultiply:
  case ControlOpcode::kFloatAdd:
  case ControlOpcode::kFloatSubtract:
  case ControlOpcode::kFloatMultiply:
  case ControlOpcode::kFloatDivide:
  case ControlOpcode::kFloatLessThan:
  case ControlOpcode::kFloatLessEqual:
  case ControlOpcode::kLessThan:
  case ControlOpcode::kLessEqual:
    return true;
  case ControlOpcode::kParameter:
  case ControlOpcode::kBlockParameter:
  case ControlOpcode::kConstant:
  case ControlOpcode::kGuardFloatNonzero:
  case ControlOpcode::kSafepoint:
    return false;
  }
  return false;
}

Status invalid_control_flow(std::size_t location, const char *message) {
  return {StatusCode::kInvalidIr, message, location};
}

Status verify_impl(const ControlFlowFunction &function) {
  const auto &blocks = function.blocks();
  const auto &nodes = function.nodes();
  if (blocks.empty() || function.entry_block() != Block{0}) {
    return invalid_control_flow(0, "control-flow graph has no entry block");
  }
  if (function.parameter_count() != blocks[0].parameters.size()) {
    return invalid_control_flow(
        0, "entry block parameters do not match the function signature");
  }

  const std::size_t no_owner = blocks.size();
  std::vector<std::size_t> owner(nodes.size(), no_owner);
  std::vector<std::size_t> position(nodes.size(), 0);
  std::vector<std::vector<std::size_t>> predecessors(blocks.size());

  bool has_return = false;
  ValueType return_type = ValueType::kWord;
  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    const BasicBlock &block = blocks[block_index];
    if (block.parameters.size() > block.instructions.size()) {
      return invalid_control_flow(block_index,
                                  "block parameters are not instructions");
    }
    for (std::size_t instruction_index = 0;
         instruction_index < block.instructions.size(); ++instruction_index) {
      const Value value = block.instructions[instruction_index];
      if (!valid_value(function, value) || owner[value.id()] != no_owner) {
        return invalid_control_flow(block_index,
                                    "SSA value has invalid block ownership");
      }
      owner[value.id()] = block_index;
      position[value.id()] = instruction_index;

      const ControlNode &node = nodes[value.id()];
      if (instruction_index < block.parameters.size()) {
        if (block.parameters[instruction_index] != value) {
          return invalid_control_flow(block_index,
                                      "block parameter order is inconsistent");
        }
        const ControlOpcode expected = block_index == 0
                                           ? ControlOpcode::kParameter
                                           : ControlOpcode::kBlockParameter;
        if (node.opcode != expected ||
            node.immediate != static_cast<Word>(instruction_index)) {
          return invalid_control_flow(block_index,
                                      "block parameter node is malformed");
        }
        if (block_index == 0 &&
            node.type != function.parameter_type(instruction_index)) {
          return invalid_control_flow(
              block_index, "entry parameter type is inconsistent");
        }
      } else if (node.opcode == ControlOpcode::kParameter ||
                 node.opcode == ControlOpcode::kBlockParameter) {
        return invalid_control_flow(block_index,
                                    "parameter node appears in block body");
      }
    }

    const ControlTerminator &terminator = block.terminator;
    if (terminator.opcode == TerminatorOpcode::kNone) {
      return invalid_control_flow(block_index, "basic block has no terminator");
    }
    const auto note_edge = [&](const ControlEdge &edge) -> Status {
      if (!valid_block(function, edge.target) || edge.target.id() == 0) {
        return invalid_control_flow(block_index,
                                    "terminator has an invalid target block");
      }
      const BasicBlock &target = blocks[edge.target.id()];
      if (edge.arguments.size() != target.parameters.size()) {
        return invalid_control_flow(
            block_index, "edge argument count does not match target block");
      }
      for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
        if (!valid_value(function, edge.arguments[index]) ||
            function.value_type(edge.arguments[index]) !=
                function.value_type(target.parameters[index])) {
          return invalid_control_flow(
              block_index, "edge argument type does not match block parameter");
        }
      }
      predecessors[edge.target.id()].push_back(block_index);
      return Status::ok_status();
    };

    if (terminator.opcode == TerminatorOpcode::kJump) {
      const Status status = note_edge(terminator.true_edge);
      if (!status.ok()) {
        return status;
      }
    } else if (terminator.opcode == TerminatorOpcode::kBranch) {
      const Status true_status = note_edge(terminator.true_edge);
      if (!true_status.ok()) {
        return true_status;
      }
      const Status false_status = note_edge(terminator.false_edge);
      if (!false_status.ok()) {
        return false_status;
      }
    }
  }

  if (std::find(owner.begin(), owner.end(), no_owner) != owner.end()) {
    return invalid_control_flow(0, "SSA value is not owned by a basic block");
  }

  std::vector<bool> reachable(blocks.size(), false);
  reachable[0] = true;
  bool reachability_changed = true;
  while (reachability_changed) {
    reachability_changed = false;
    for (std::size_t block_index = 1; block_index < blocks.size();
         ++block_index) {
      if (reachable[block_index]) {
        continue;
      }
      for (const std::size_t predecessor : predecessors[block_index]) {
        if (reachable[predecessor]) {
          reachable[block_index] = true;
          reachability_changed = true;
          break;
        }
      }
    }
  }
  if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
    return invalid_control_flow(
        0, "control-flow graph contains unreachable blocks");
  }

  std::vector<std::vector<bool>> dominates(
      blocks.size(), std::vector<bool>(blocks.size(), true));
  std::fill(dominates[0].begin(), dominates[0].end(), false);
  dominates[0][0] = true;
  bool dominance_changed = true;
  while (dominance_changed) {
    dominance_changed = false;
    for (std::size_t block_index = 1; block_index < blocks.size();
         ++block_index) {
      std::vector<bool> next(blocks.size(), true);
      for (const std::size_t predecessor : predecessors[block_index]) {
        for (std::size_t candidate = 0; candidate < blocks.size();
             ++candidate) {
          next[candidate] =
              next[candidate] && dominates[predecessor][candidate];
        }
      }
      next[block_index] = true;
      if (next != dominates[block_index]) {
        dominates[block_index] = std::move(next);
        dominance_changed = true;
      }
    }
  }

  const auto available = [&](Value value, std::size_t use_block,
                             std::size_t use_position) -> bool {
    if (!valid_value(function, value)) {
      return false;
    }
    const std::size_t definition_block = owner[value.id()];
    if (definition_block == use_block) {
      return position[value.id()] < use_position;
    }
    return dominates[use_block][definition_block] != false;
  };

  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    const BasicBlock &block = blocks[block_index];
    for (std::size_t instruction_index = block.parameters.size();
         instruction_index < block.instructions.size(); ++instruction_index) {
      const Value value = block.instructions[instruction_index];
      const ControlNode &node = nodes[value.id()];
      if (node.opcode == ControlOpcode::kConstant) {
        continue;
      }
      if (node.opcode == ControlOpcode::kGuardFloatNonzero) {
        if (node.immediate < 0 || node.rhs.valid() ||
            node.type != ValueType::kWord ||
            !available(node.lhs, block_index, instruction_index) ||
            function.value_type(node.lhs) != ValueType::kFloat64) {
          return invalid_control_flow(
              value.id(), "control-flow Float64 guard is malformed");
        }
        continue;
      }
      if (node.opcode == ControlOpcode::kSafepoint) {
        if (node.immediate < 0 || node.lhs.valid() || node.rhs.valid() ||
            node.type != ValueType::kWord) {
          return invalid_control_flow(value.id(),
                                      "control-flow safepoint is malformed");
        }
        continue;
      }
      if (!is_binary(node.opcode)) {
        return invalid_control_flow(value.id(), "unknown control-flow opcode");
      }
      if (!available(node.lhs, block_index, instruction_index) ||
          !available(node.rhs, block_index, instruction_index)) {
        return invalid_control_flow(
            value.id(), "SSA operand does not dominate its instruction");
      }
      const ValueType lhs_type = function.value_type(node.lhs);
      const ValueType rhs_type = function.value_type(node.rhs);
      if (node.opcode == ControlOpcode::kFloatAdd ||
          node.opcode == ControlOpcode::kFloatSubtract ||
          node.opcode == ControlOpcode::kFloatMultiply ||
          node.opcode == ControlOpcode::kFloatDivide) {
        if (node.type != ValueType::kFloat64 ||
            lhs_type != ValueType::kFloat64 ||
            rhs_type != ValueType::kFloat64) {
          return invalid_control_flow(
              value.id(), "Float64 CFG arithmetic has incompatible types");
        }
      } else if (node.opcode == ControlOpcode::kFloatLessThan ||
                 node.opcode == ControlOpcode::kFloatLessEqual) {
        if (node.type != ValueType::kWord ||
            lhs_type != ValueType::kFloat64 ||
            rhs_type != ValueType::kFloat64) {
          return invalid_control_flow(
              value.id(), "Float64 CFG comparison has incompatible types");
        }
      } else if (node.type != ValueType::kWord ||
                 lhs_type != ValueType::kWord ||
                 rhs_type != ValueType::kWord) {
        return invalid_control_flow(
            value.id(), "word CFG operation has incompatible types");
      }
    }

    const std::size_t terminator_position = block.instructions.size();
    const ControlTerminator &terminator = block.terminator;
    if (terminator.opcode == TerminatorOpcode::kReturn ||
        terminator.opcode == TerminatorOpcode::kBranch) {
      if (!available(terminator.value, block_index, terminator_position)) {
        return invalid_control_flow(
            block_index, "terminator value is unavailable in its block");
      }
    }
    if (terminator.opcode == TerminatorOpcode::kReturn) {
      const ValueType current_return_type =
          function.value_type(terminator.value);
      if (has_return && current_return_type != return_type) {
        return invalid_control_flow(
            block_index, "CFG return values have inconsistent types");
      }
      has_return = true;
      return_type = current_return_type;
    }
    if (terminator.opcode == TerminatorOpcode::kBranch &&
        function.value_type(terminator.value) != ValueType::kWord) {
      return invalid_control_flow(block_index,
                                  "branch condition must be a word value");
    }
    const auto verify_edge_values = [&](const ControlEdge &edge) -> bool {
      return std::all_of(
          edge.arguments.begin(), edge.arguments.end(), [&](Value value) {
            return available(value, block_index, terminator_position);
          });
    };
    if (terminator.opcode == TerminatorOpcode::kJump &&
        !verify_edge_values(terminator.true_edge)) {
      return invalid_control_flow(block_index,
                                  "jump argument is unavailable in its block");
    }
    if (terminator.opcode == TerminatorOpcode::kBranch &&
        (!verify_edge_values(terminator.true_edge) ||
         !verify_edge_values(terminator.false_edge))) {
      return invalid_control_flow(
          block_index, "branch argument is unavailable in its block");
    }
  }
  return Status::ok_status();
}

std::uint64_t to_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

Word evaluate_node(ControlOpcode opcode, Word lhs, Word rhs) noexcept {
  if (opcode == ControlOpcode::kAdd) {
    return from_bits(to_bits(lhs) + to_bits(rhs));
  }
  if (opcode == ControlOpcode::kSubtract) {
    return from_bits(to_bits(lhs) - to_bits(rhs));
  }
  if (opcode == ControlOpcode::kMultiply) {
    return from_bits(to_bits(lhs) * to_bits(rhs));
  }
  if (opcode == ControlOpcode::kFloatAdd) {
    return pack_float64(unpack_float64(lhs) + unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatSubtract) {
    return pack_float64(unpack_float64(lhs) - unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatMultiply) {
    return pack_float64(unpack_float64(lhs) * unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatDivide) {
    return pack_float64(unpack_float64(lhs) / unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatLessThan) {
    return unpack_float64(lhs) < unpack_float64(rhs) ? 1 : 0;
  }
  if (opcode == ControlOpcode::kFloatLessEqual) {
    return unpack_float64(lhs) <= unpack_float64(rhs) ? 1 : 0;
  }
  if (opcode == ControlOpcode::kLessThan) {
    return lhs < rhs ? 1 : 0;
  }
  return lhs <= rhs ? 1 : 0;
}

} // namespace

ControlFlowBuilder::ControlFlowBuilder(std::size_t parameter_count)
    : ControlFlowBuilder(
          std::vector<ValueType>(parameter_count, ValueType::kWord)) {}

ControlFlowBuilder::ControlFlowBuilder(
    std::vector<ValueType> parameter_types) {
  function_.parameter_count_ = parameter_types.size();
  function_.parameter_types_ = std::move(parameter_types);
  function_.entry_block_ = Block{0};
  function_.blocks_.emplace_back();
  insertion_block_ = function_.entry_block_;
  function_.nodes_.reserve(function_.parameter_count_);
  BasicBlock &entry = function_.blocks_[0];
  entry.parameters.reserve(function_.parameter_count_);
  entry.instructions.reserve(function_.parameter_count_);
  for (std::size_t index = 0; index < function_.parameter_count_; ++index) {
    const Value value = append_node(ControlNode{
        ControlOpcode::kParameter, {}, {}, static_cast<Word>(index),
        function_.parameter_types_[index]});
    entry.parameters.push_back(value);
  }
}

Value ControlFlowBuilder::parameter(std::size_t index) const noexcept {
  const BasicBlock &entry = function_.blocks_[function_.entry_block_.id()];
  return index < entry.parameters.size() ? entry.parameters[index] : Value{};
}

Value ControlFlowBuilder::block_parameter(Block block,
                                          std::size_t index) const noexcept {
  if (!block.valid() || block.id() >= function_.blocks_.size()) {
    return {};
  }
  const BasicBlock &target = function_.blocks_[block.id()];
  return index < target.parameters.size() ? target.parameters[index] : Value{};
}

Block ControlFlowBuilder::create_block(std::size_t parameter_count) {
  return create_block(
      std::vector<ValueType>(parameter_count, ValueType::kWord));
}

Block ControlFlowBuilder::create_block(
    std::vector<ValueType> parameter_types) {
  if (function_.blocks_.size() >= Block::kInvalidId) {
    return {};
  }
  const Block block{static_cast<std::uint32_t>(function_.blocks_.size())};
  function_.blocks_.emplace_back();
  BasicBlock &created = function_.blocks_.back();
  created.parameters.reserve(parameter_types.size());
  created.instructions.reserve(parameter_types.size());
  const Block previous = insertion_block_;
  insertion_block_ = block;
  for (std::size_t index = 0; index < parameter_types.size(); ++index) {
    const Value value = append_node(ControlNode{
        ControlOpcode::kBlockParameter, {}, {}, static_cast<Word>(index),
        parameter_types[index]});
    created.parameters.push_back(value);
  }
  insertion_block_ = previous;
  return block;
}

Status ControlFlowBuilder::set_insertion_block(Block block) {
  if (!block.valid() || block.id() >= function_.blocks_.size()) {
    return {StatusCode::kInvalidArgument, "insertion block does not exist"};
  }
  insertion_block_ = block;
  return Status::ok_status();
}

Value ControlFlowBuilder::append_node(ControlNode node) {
  if (!insertion_block_.valid() ||
      insertion_block_.id() >= function_.blocks_.size() ||
      function_.nodes_.size() >= Value::kInvalidId) {
    return {};
  }
  BasicBlock &block = function_.blocks_[insertion_block_.id()];
  if (block.terminator.opcode != TerminatorOpcode::kNone) {
    return {};
  }
  const Value value{static_cast<std::uint32_t>(function_.nodes_.size())};
  function_.nodes_.push_back(node);
  block.instructions.push_back(value);
  return value;
}

Value ControlFlowBuilder::append_binary(ControlOpcode opcode, Value lhs,
                                        Value rhs, ValueType type) {
  return append_node(ControlNode{opcode, lhs, rhs, 0, type});
}

Value ControlFlowBuilder::constant(Word value) {
  return append_node(ControlNode{ControlOpcode::kConstant, {}, {}, value,
                                 ValueType::kWord});
}

Value ControlFlowBuilder::float64_constant(double value) {
  return float64_constant_bits(pack_float64(value));
}

Value ControlFlowBuilder::float64_constant_bits(Word bits) {
  return append_node(ControlNode{ControlOpcode::kConstant, {}, {}, bits,
                                 ValueType::kFloat64});
}

Value ControlFlowBuilder::add(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kAdd, lhs, rhs);
}

Value ControlFlowBuilder::subtract(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kSubtract, lhs, rhs);
}

Value ControlFlowBuilder::multiply(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kMultiply, lhs, rhs);
}

Value ControlFlowBuilder::float64_add(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatAdd, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_subtract(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatSubtract, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_multiply(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatMultiply, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_divide(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatDivide, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_less_than(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatLessThan, lhs, rhs,
                       ValueType::kWord);
}

Value ControlFlowBuilder::float64_less_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatLessEqual, lhs, rhs,
                       ValueType::kWord);
}

Value ControlFlowBuilder::less_than(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kLessThan, lhs, rhs);
}

Value ControlFlowBuilder::less_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kLessEqual, lhs, rhs);
}

Value ControlFlowBuilder::guard_float64_nonzero(Value value,
                                                std::size_t site) {
  if (site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  return append_node(ControlNode{ControlOpcode::kGuardFloatNonzero, value, {},
                                 static_cast<Word>(site), ValueType::kWord});
}

Value ControlFlowBuilder::safepoint(std::size_t site) {
  if (site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  return append_node(ControlNode{ControlOpcode::kSafepoint, {}, {},
                                 static_cast<Word>(site), ValueType::kWord});
}

Status
ControlFlowBuilder::validate_edge(Block target,
                                  const std::vector<Value> &arguments) const {
  if (!target.valid() || target.id() >= function_.blocks_.size() ||
      target == function_.entry_block_) {
    return {StatusCode::kInvalidArgument, "edge target is invalid"};
  }
  if (arguments.size() != function_.blocks_[target.id()].parameters.size()) {
    return {StatusCode::kInvalidArgument,
            "edge argument count does not match target block"};
  }
  const BasicBlock &target_block = function_.blocks_[target.id()];
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!arguments[index].valid() ||
        arguments[index].id() >= function_.nodes_.size() ||
        function_.nodes_[arguments[index].id()].type !=
            function_.nodes_[target_block.parameters[index].id()].type) {
      return {StatusCode::kInvalidArgument,
              "edge argument type does not match target block"};
    }
  }
  return Status::ok_status();
}

Status ControlFlowBuilder::set_terminator(ControlTerminator terminator) {
  if (!insertion_block_.valid() ||
      insertion_block_.id() >= function_.blocks_.size()) {
    return {StatusCode::kInvalidArgument, "insertion block does not exist"};
  }
  BasicBlock &block = function_.blocks_[insertion_block_.id()];
  if (block.terminator.opcode != TerminatorOpcode::kNone) {
    return {StatusCode::kInvalidArgument, "basic block is already terminated"};
  }
  block.terminator = std::move(terminator);
  return Status::ok_status();
}

Status ControlFlowBuilder::set_return(Value value) {
  if (!value.valid() || value.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "return value does not exist"};
  }
  ControlTerminator terminator;
  terminator.opcode = TerminatorOpcode::kReturn;
  terminator.value = value;
  return set_terminator(std::move(terminator));
}

Status ControlFlowBuilder::jump(Block target, std::vector<Value> arguments) {
  const Status edge_status = validate_edge(target, arguments);
  if (!edge_status.ok()) {
    return edge_status;
  }
  ControlTerminator terminator;
  terminator.opcode = TerminatorOpcode::kJump;
  terminator.true_edge = {target, std::move(arguments)};
  return set_terminator(std::move(terminator));
}

Status ControlFlowBuilder::branch(Value condition, Block true_target,
                                  std::vector<Value> true_arguments,
                                  Block false_target,
                                  std::vector<Value> false_arguments) {
  if (!condition.valid() || condition.id() >= function_.nodes_.size()) {
    return {StatusCode::kInvalidArgument, "branch condition does not exist"};
  }
  const Status true_status = validate_edge(true_target, true_arguments);
  if (!true_status.ok()) {
    return true_status;
  }
  const Status false_status = validate_edge(false_target, false_arguments);
  if (!false_status.ok()) {
    return false_status;
  }
  ControlTerminator terminator;
  terminator.opcode = TerminatorOpcode::kBranch;
  terminator.value = condition;
  terminator.true_edge = {true_target, std::move(true_arguments)};
  terminator.false_edge = {false_target, std::move(false_arguments)};
  return set_terminator(std::move(terminator));
}

ControlFlowFunction ControlFlowBuilder::build() && noexcept {
  return std::move(function_);
}

Status verify(const ControlFlowFunction &function) {
  try {
    return verify_impl(function);
  } catch (const std::bad_alloc &) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate control-flow verification state"};
  }
}

EvaluationResult
ControlFlowInterpreter::evaluate(const ControlFlowFunction &function,
                                 const Word *args, std::size_t arg_count,
                                 std::size_t maximum_block_executions,
                                 runtime::ExecutionContext *context) {
  const Status verification = verify(function);
  if (!verification.ok()) {
    return {verification, 0};
  }
  if (arg_count != function.parameter_count()) {
    return {{StatusCode::kInvalidArgument,
             "argument count does not match the function signature"},
            0};
  }
  if (arg_count != 0 && args == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "argument storage is null for a non-empty signature"},
            0};
  }
  if (maximum_block_executions == 0) {
    return {{StatusCode::kInvalidArgument,
             "control-flow execution budget must be positive"},
            0};
  }
  if (context != nullptr) {
    context->clear_exit();
  }

  try {
    std::vector<Word> values(function.nodes().size(), 0);
    const BasicBlock &entry = function.blocks()[function.entry_block().id()];
    for (std::size_t index = 0; index < entry.parameters.size(); ++index) {
      values[entry.parameters[index].id()] = args[index];
    }

    Block current = function.entry_block();
    std::size_t executions = 0;
    while (executions++ < maximum_block_executions) {
      const BasicBlock &block = function.blocks()[current.id()];
      for (std::size_t index = block.parameters.size();
           index < block.instructions.size(); ++index) {
        const Value value = block.instructions[index];
        const ControlNode &node = function.nodes()[value.id()];
        if (node.opcode == ControlOpcode::kConstant) {
          values[value.id()] = node.immediate;
        } else if (node.opcode == ControlOpcode::kGuardFloatNonzero) {
          values[value.id()] = 0;
          const Word guarded = values[node.lhs.id()];
          if (unpack_float64(guarded) == 0.0) {
            const auto site = static_cast<std::size_t>(node.immediate);
            if (context != nullptr) {
              context->record_exit(runtime::ExitReason::kRuntime, site,
                                   guarded);
            }
            return {{StatusCode::kRuntimeExit,
                     "control-flow Float64 guard requested a runtime exit",
                     site},
                    0};
          }
        } else if (node.opcode == ControlOpcode::kSafepoint) {
          values[value.id()] = 0;
          if (context != nullptr && context->exit_poll_requested()) {
            const auto site = static_cast<std::size_t>(node.immediate);
            context->record_exit(runtime::ExitReason::kSafepoint, site);
            return {{StatusCode::kExecutionInterrupted,
                     "execution interrupted at a control-flow safepoint",
                     site},
                    0};
          }
        } else {
          values[value.id()] = evaluate_node(node.opcode, values[node.lhs.id()],
                                             values[node.rhs.id()]);
        }
      }

      const ControlTerminator &terminator = block.terminator;
      if (terminator.opcode == TerminatorOpcode::kReturn) {
        return {Status::ok_status(), values[terminator.value.id()]};
      }
      const ControlEdge &edge =
          terminator.opcode == TerminatorOpcode::kJump
              ? terminator.true_edge
              : (values[terminator.value.id()] != 0 ? terminator.true_edge
                                                    : terminator.false_edge);
      std::vector<Word> incoming;
      incoming.reserve(edge.arguments.size());
      for (const Value argument : edge.arguments) {
        incoming.push_back(values[argument.id()]);
      }
      const BasicBlock &target = function.blocks()[edge.target.id()];
      for (std::size_t index = 0; index < incoming.size(); ++index) {
        values[target.parameters[index].id()] = incoming[index];
      }
      current = edge.target;
    }
    return {{StatusCode::kResourceExhausted,
             "control-flow execution budget was exhausted"},
            0};
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate control-flow interpreter state"},
            0};
  }
}

} // namespace unijit::ir
