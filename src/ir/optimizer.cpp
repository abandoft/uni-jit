#include "unijit/ir/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unijit::ir {
namespace {

struct PassResult final {
  Function function;
  std::vector<Value> mapping;
  std::vector<OptimizationExitState> exit_states;
  std::size_t constants_folded{0};
  std::size_t algebraic_simplifications{0};
  bool changed{false};
};

std::uint64_t to_bits(Word value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Word from_bits(std::uint64_t bits) noexcept {
  Word result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

Word fold_binary(Opcode opcode, Word lhs, Word rhs) noexcept {
  const std::uint64_t lhs_bits = to_bits(lhs);
  const std::uint64_t rhs_bits = to_bits(rhs);
  if (opcode == Opcode::kAdd) {
    return from_bits(lhs_bits + rhs_bits);
  }
  if (opcode == Opcode::kSubtract) {
    return from_bits(lhs_bits - rhs_bits);
  }
  if (opcode == Opcode::kMultiply) {
    return from_bits(lhs_bits * rhs_bits);
  }
  if (opcode == Opcode::kBitwiseAnd) {
    return from_bits(lhs_bits & rhs_bits);
  }
  if (opcode == Opcode::kBitwiseOr) {
    return from_bits(lhs_bits | rhs_bits);
  }
  if (opcode == Opcode::kShiftLeft) {
    if (rhs < 0) {
      const std::uint64_t magnitude = UINT64_C(0) - rhs_bits;
      return magnitude >= 64U ? 0 : from_bits(lhs_bits >> magnitude);
    }
    return rhs_bits >= 64U ? 0 : from_bits(lhs_bits << rhs_bits);
  }
  if (opcode == Opcode::kFloorDivide) {
    return floor_divide_word(lhs, rhs);
  }
  if (opcode == Opcode::kFloorModulo) {
    return floor_modulo_word(lhs, rhs);
  }
  return from_bits(lhs_bits ^ rhs_bits);
}

Word fold_float_binary(Opcode opcode, Word lhs, Word rhs) noexcept {
  const double lhs_value = unpack_float64(lhs);
  const double rhs_value = unpack_float64(rhs);
  if (opcode == Opcode::kFloatAdd) {
    return pack_float64(lhs_value + rhs_value);
  }
  if (opcode == Opcode::kFloatSubtract) {
    return pack_float64(lhs_value - rhs_value);
  }
  if (opcode == Opcode::kFloatMultiply) {
    return pack_float64(lhs_value * rhs_value);
  }
  if (opcode == Opcode::kFloatDivide) {
    return pack_float64(lhs_value / rhs_value);
  }
  if (opcode == Opcode::kFloatLessThan) {
    return lhs_value < rhs_value ? 1 : 0;
  }
  if (opcode == Opcode::kFloatLessEqual) {
    return lhs_value <= rhs_value ? 1 : 0;
  }
  if (opcode == Opcode::kFloatEqual) {
    return lhs_value == rhs_value ? 1 : 0;
  }
  return lhs_value != rhs_value ? 1 : 0;
}

bool is_binary(Opcode opcode) noexcept {
  return opcode == Opcode::kAdd || opcode == Opcode::kSubtract ||
         opcode == Opcode::kMultiply || opcode == Opcode::kBitwiseAnd ||
         opcode == Opcode::kBitwiseOr || opcode == Opcode::kBitwiseXor ||
         opcode == Opcode::kShiftLeft || opcode == Opcode::kFloorDivide ||
         opcode == Opcode::kFloorModulo ||
         opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide ||
         opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_unary(Opcode opcode) noexcept {
  return opcode == Opcode::kNegate || opcode == Opcode::kBitwiseNot ||
         opcode == Opcode::kFloatNegate;
}

Word fold_unary(Opcode opcode, Word value) noexcept {
  if (opcode == Opcode::kNegate) {
    return from_bits(UINT64_C(0) - to_bits(value));
  }
  if (opcode == Opcode::kBitwiseNot) {
    return from_bits(~to_bits(value));
  }
  return from_bits(to_bits(value) ^ (UINT64_C(1) << 63U));
}

Value emit_unary(FunctionBuilder* builder, Opcode opcode, Value value) {
  if (opcode == Opcode::kNegate) {
    return builder->negate(value);
  }
  if (opcode == Opcode::kBitwiseNot) {
    return builder->bitwise_not(value);
  }
  return builder->float64_negate(value);
}

bool is_float_binary(Opcode opcode) noexcept {
  return opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide ||
         opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_float_comparison(Opcode opcode) noexcept {
  return opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_nonzero_guard(Opcode opcode) noexcept {
  return opcode == Opcode::kGuardWordNonzero ||
         opcode == Opcode::kGuardFloatNonzero;
}

bool is_nonzero_guard(ControlOpcode opcode) noexcept {
  return opcode == ControlOpcode::kGuardWordNonzero ||
         opcode == ControlOpcode::kGuardFloatNonzero;
}

bool is_control_binary(ControlOpcode opcode) noexcept {
  switch (opcode) {
    case ControlOpcode::kAdd:
    case ControlOpcode::kSubtract:
    case ControlOpcode::kMultiply:
    case ControlOpcode::kBitwiseAnd:
    case ControlOpcode::kBitwiseOr:
    case ControlOpcode::kBitwiseXor:
    case ControlOpcode::kShiftLeft:
    case ControlOpcode::kFloorDivide:
    case ControlOpcode::kFloorModulo:
    case ControlOpcode::kFloatAdd:
    case ControlOpcode::kFloatSubtract:
    case ControlOpcode::kFloatMultiply:
    case ControlOpcode::kFloatDivide:
    case ControlOpcode::kFloatLessThan:
    case ControlOpcode::kFloatLessEqual:
    case ControlOpcode::kFloatEqual:
    case ControlOpcode::kFloatNotEqual:
    case ControlOpcode::kLessThan:
    case ControlOpcode::kLessEqual:
      return true;
    default:
      return false;
  }
}

bool is_control_unary(ControlOpcode opcode) noexcept {
  return opcode == ControlOpcode::kNegate ||
         opcode == ControlOpcode::kBitwiseNot ||
         opcode == ControlOpcode::kFloatNegate;
}

Word fold_control_unary(ControlOpcode opcode, Word value) noexcept {
  if (opcode == ControlOpcode::kNegate) {
    return from_bits(UINT64_C(0) - to_bits(value));
  }
  if (opcode == ControlOpcode::kBitwiseNot) {
    return from_bits(~to_bits(value));
  }
  return from_bits(to_bits(value) ^ (UINT64_C(1) << 63U));
}

Value emit_control_unary(ControlFlowBuilder* builder, ControlOpcode opcode,
                         Value value) {
  if (opcode == ControlOpcode::kNegate) {
    return builder->negate(value);
  }
  if (opcode == ControlOpcode::kBitwiseNot) {
    return builder->bitwise_not(value);
  }
  return builder->float64_negate(value);
}

bool is_control_float_binary(ControlOpcode opcode) noexcept {
  return opcode == ControlOpcode::kFloatAdd ||
         opcode == ControlOpcode::kFloatSubtract ||
         opcode == ControlOpcode::kFloatMultiply ||
         opcode == ControlOpcode::kFloatDivide ||
         opcode == ControlOpcode::kFloatLessThan ||
         opcode == ControlOpcode::kFloatLessEqual ||
         opcode == ControlOpcode::kFloatEqual ||
         opcode == ControlOpcode::kFloatNotEqual;
}

bool is_control_comparison(ControlOpcode opcode) noexcept {
  return opcode == ControlOpcode::kFloatLessThan ||
         opcode == ControlOpcode::kFloatLessEqual ||
         opcode == ControlOpcode::kFloatEqual ||
         opcode == ControlOpcode::kFloatNotEqual ||
         opcode == ControlOpcode::kLessThan ||
         opcode == ControlOpcode::kLessEqual;
}

Word fold_control_binary(ControlOpcode opcode, Word lhs, Word rhs) noexcept {
  if (is_control_float_binary(opcode)) {
    const double lhs_value = unpack_float64(lhs);
    const double rhs_value = unpack_float64(rhs);
    if (opcode == ControlOpcode::kFloatAdd) {
      return pack_float64(lhs_value + rhs_value);
    }
    if (opcode == ControlOpcode::kFloatSubtract) {
      return pack_float64(lhs_value - rhs_value);
    }
    if (opcode == ControlOpcode::kFloatMultiply) {
      return pack_float64(lhs_value * rhs_value);
    }
    if (opcode == ControlOpcode::kFloatDivide) {
      return pack_float64(lhs_value / rhs_value);
    }
    if (opcode == ControlOpcode::kFloatLessThan) {
      return lhs_value < rhs_value ? 1 : 0;
    }
    if (opcode == ControlOpcode::kFloatLessEqual) {
      return lhs_value <= rhs_value ? 1 : 0;
    }
    if (opcode == ControlOpcode::kFloatEqual) {
      return lhs_value == rhs_value ? 1 : 0;
    }
    return lhs_value != rhs_value ? 1 : 0;
  }
  if (opcode == ControlOpcode::kAdd) {
    return from_bits(to_bits(lhs) + to_bits(rhs));
  }
  if (opcode == ControlOpcode::kSubtract) {
    return from_bits(to_bits(lhs) - to_bits(rhs));
  }
  if (opcode == ControlOpcode::kMultiply) {
    return from_bits(to_bits(lhs) * to_bits(rhs));
  }
  if (opcode == ControlOpcode::kBitwiseAnd) {
    return from_bits(to_bits(lhs) & to_bits(rhs));
  }
  if (opcode == ControlOpcode::kBitwiseOr) {
    return from_bits(to_bits(lhs) | to_bits(rhs));
  }
  if (opcode == ControlOpcode::kBitwiseXor) {
    return from_bits(to_bits(lhs) ^ to_bits(rhs));
  }
  if (opcode == ControlOpcode::kShiftLeft) {
    const std::uint64_t rhs_bits = to_bits(rhs);
    if (rhs < 0) {
      const std::uint64_t magnitude = UINT64_C(0) - rhs_bits;
      return magnitude >= 64U ? 0 : from_bits(to_bits(lhs) >> magnitude);
    }
    return rhs_bits >= 64U ? 0 : from_bits(to_bits(lhs) << rhs_bits);
  }
  if (opcode == ControlOpcode::kFloorDivide) {
    return floor_divide_word(lhs, rhs);
  }
  if (opcode == ControlOpcode::kFloorModulo) {
    return floor_modulo_word(lhs, rhs);
  }
  if (opcode == ControlOpcode::kLessThan) {
    return lhs < rhs ? 1 : 0;
  }
  return lhs <= rhs ? 1 : 0;
}

Value emit_control_binary(ControlFlowBuilder* builder,
                          ControlOpcode opcode, Value lhs, Value rhs) {
  switch (opcode) {
    case ControlOpcode::kAdd:
      return builder->add(lhs, rhs);
    case ControlOpcode::kSubtract:
      return builder->subtract(lhs, rhs);
    case ControlOpcode::kMultiply:
      return builder->multiply(lhs, rhs);
    case ControlOpcode::kBitwiseAnd:
      return builder->bitwise_and(lhs, rhs);
    case ControlOpcode::kBitwiseOr:
      return builder->bitwise_or(lhs, rhs);
    case ControlOpcode::kBitwiseXor:
      return builder->bitwise_xor(lhs, rhs);
    case ControlOpcode::kShiftLeft:
      return builder->shift_left(lhs, rhs);
    case ControlOpcode::kFloorDivide:
      return builder->floor_divide(lhs, rhs);
    case ControlOpcode::kFloorModulo:
      return builder->floor_modulo(lhs, rhs);
    case ControlOpcode::kFloatAdd:
      return builder->float64_add(lhs, rhs);
    case ControlOpcode::kFloatSubtract:
      return builder->float64_subtract(lhs, rhs);
    case ControlOpcode::kFloatMultiply:
      return builder->float64_multiply(lhs, rhs);
    case ControlOpcode::kFloatDivide:
      return builder->float64_divide(lhs, rhs);
    case ControlOpcode::kFloatLessThan:
      return builder->float64_less_than(lhs, rhs);
    case ControlOpcode::kFloatLessEqual:
      return builder->float64_less_equal(lhs, rhs);
    case ControlOpcode::kFloatEqual:
      return builder->float64_equal(lhs, rhs);
    case ControlOpcode::kFloatNotEqual:
      return builder->float64_not_equal(lhs, rhs);
    case ControlOpcode::kLessThan:
      return builder->less_than(lhs, rhs);
    case ControlOpcode::kLessEqual:
      return builder->less_equal(lhs, rhs);
    default:
      return {};
  }
}

bool control_value_available(const ControlFlowFunction& function,
                             Value value, Value exit) {
  if (!value.valid() || value.id() >= function.nodes().size() ||
      !exit.valid() || exit.id() >= function.nodes().size()) {
    return false;
  }
  const std::size_t block_count = function.blocks().size();
  const std::size_t no_owner = block_count;
  std::vector<std::size_t> owners(function.nodes().size(), no_owner);
  std::vector<std::size_t> positions(function.nodes().size(), 0);
  std::vector<std::vector<std::size_t>> predecessors(block_count);
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    const BasicBlock& block = function.blocks()[block_index];
    for (std::size_t position = 0; position < block.instructions.size();
         ++position) {
      const Value instruction = block.instructions[position];
      owners[instruction.id()] = block_index;
      positions[instruction.id()] = position;
    }
    const auto note_edge = [&](const ControlEdge& edge) {
      predecessors[edge.target.id()].push_back(block_index);
    };
    if (block.terminator.opcode == TerminatorOpcode::kJump) {
      note_edge(block.terminator.true_edge);
    } else if (block.terminator.opcode == TerminatorOpcode::kBranch) {
      note_edge(block.terminator.true_edge);
      note_edge(block.terminator.false_edge);
    }
  }
  const std::size_t value_block = owners[value.id()];
  const std::size_t exit_block = owners[exit.id()];
  if (value_block == no_owner || exit_block == no_owner) {
    return false;
  }
  if (value_block == exit_block) {
    return positions[value.id()] < positions[exit.id()];
  }

  std::vector<std::vector<bool>> dominates(
      block_count, std::vector<bool>(block_count, true));
  std::fill(dominates[0].begin(), dominates[0].end(), false);
  dominates[0][0] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block_index = 1; block_index < block_count;
         ++block_index) {
      std::vector<bool> next(block_count, true);
      for (const std::size_t predecessor : predecessors[block_index]) {
        for (std::size_t candidate = 0; candidate < block_count;
             ++candidate) {
          next[candidate] =
              next[candidate] && dominates[predecessor][candidate];
        }
      }
      next[block_index] = true;
      if (next != dominates[block_index]) {
        dominates[block_index] = std::move(next);
        changed = true;
      }
    }
  }
  return dominates[exit_block][value_block];
}

PassResult transform_once(
    const Function& input,
    const std::vector<OptimizationExitState>& exit_states) {
  const std::size_t node_count = input.nodes().size();
  std::vector<bool> live(node_count, false);
  live[input.return_value().id()] = true;
  for (std::size_t index = 0; index < node_count; ++index) {
    if (input.nodes()[index].opcode == Opcode::kCall ||
        is_nonzero_guard(input.nodes()[index].opcode) ||
        input.nodes()[index].opcode == Opcode::kSafepoint) {
      live[index] = true;
    }
  }
  for (const OptimizationExitState& exit_state : exit_states) {
    for (const Value value : exit_state.live_values) {
      live[value.id()] = true;
    }
  }
  for (std::size_t reverse = node_count; reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    if (!live[index]) {
      continue;
    }
    const Node& node = input.nodes()[index];
    if (is_unary(node.opcode)) {
      live[node.lhs.id()] = true;
    } else if (is_binary(node.opcode)) {
      live[node.lhs.id()] = true;
      live[node.rhs.id()] = true;
    } else if (node.opcode == Opcode::kCall) {
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        live[argument.id()] = true;
      }
    } else if (is_nonzero_guard(node.opcode)) {
      live[node.lhs.id()] = true;
    }
  }

  std::vector<ValueType> parameter_types;
  parameter_types.reserve(input.parameter_count());
  for (std::size_t index = 0; index < input.parameter_count(); ++index) {
    parameter_types.push_back(input.parameter_type(index));
  }
  FunctionBuilder builder(std::move(parameter_types));
  std::vector<Value> mapped(node_count);
  std::vector<bool> known_constant(node_count, false);
  std::vector<Word> constant_value(node_count, 0);
  for (std::size_t index = 0; index < input.parameter_count(); ++index) {
    mapped[index] = builder.parameter(index);
  }

  std::size_t folded = 0;
  std::size_t simplified = 0;
  bool changed = false;
  for (std::size_t index = input.parameter_count(); index < node_count;
       ++index) {
    if (!live[index]) {
      changed = true;
      continue;
    }
    const Node& node = input.nodes()[index];
    if (node.opcode == Opcode::kConstant) {
      if (node.type == ValueType::kFloat64) {
        mapped[index] = builder.float64_constant_bits(node.immediate);
      } else {
        mapped[index] = builder.constant(node.immediate);
      }
      known_constant[index] = true;
      constant_value[index] = node.immediate;
      continue;
    }

    if (node.opcode == Opcode::kCall) {
      std::vector<Value> arguments;
      arguments.reserve(node.argument_count);
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        arguments.push_back(mapped[argument.id()]);
      }
      mapped[index] = builder.call(unpack_runtime_helper(node.immediate),
                                   std::move(arguments), node.type);
      continue;
    }

    if (node.opcode == Opcode::kSafepoint) {
      mapped[index] = builder.safepoint(
          static_cast<std::size_t>(node.immediate));
      continue;
    }

    if (is_nonzero_guard(node.opcode)) {
      const std::size_t guarded_id = node.lhs.id();
      const bool known_nonzero =
          known_constant[guarded_id] &&
          (node.opcode == Opcode::kGuardWordNonzero
               ? constant_value[guarded_id] != 0
               : unpack_float64(constant_value[guarded_id]) != 0.0);
      if (known_nonzero) {
        mapped[index] = builder.constant(0);
        known_constant[index] = true;
        constant_value[index] = 0;
        ++simplified;
        changed = true;
      } else {
        mapped[index] =
            node.opcode == Opcode::kGuardWordNonzero
                ? builder.guard_word_nonzero(
                      mapped[guarded_id],
                      static_cast<std::size_t>(node.immediate))
                : builder.guard_float64_nonzero(
                      mapped[guarded_id],
                      static_cast<std::size_t>(node.immediate));
      }
      continue;
    }

    if (is_unary(node.opcode)) {
      const std::size_t operand_id = node.lhs.id();
      if (known_constant[operand_id]) {
        constant_value[index] =
            fold_unary(node.opcode, constant_value[operand_id]);
        mapped[index] = node.type == ValueType::kFloat64
                            ? builder.float64_constant_bits(
                                  constant_value[index])
                            : builder.constant(constant_value[index]);
        known_constant[index] = true;
        ++folded;
        changed = true;
      } else if (input.nodes()[operand_id].opcode == node.opcode) {
        mapped[index] = mapped[input.nodes()[operand_id].lhs.id()];
        ++simplified;
        changed = true;
      } else {
        mapped[index] = emit_unary(&builder, node.opcode, mapped[operand_id]);
      }
      continue;
    }

    const std::size_t lhs_id = node.lhs.id();
    const std::size_t rhs_id = node.rhs.id();
    if (is_float_binary(node.opcode)) {
      if (known_constant[lhs_id] && known_constant[rhs_id]) {
        constant_value[index] = fold_float_binary(
            node.opcode, constant_value[lhs_id], constant_value[rhs_id]);
        mapped[index] = is_float_comparison(node.opcode)
                            ? builder.constant(constant_value[index])
                            : builder.float64_constant_bits(
                                  constant_value[index]);
        known_constant[index] = true;
        ++folded;
        changed = true;
      } else if (node.opcode == Opcode::kFloatAdd) {
        mapped[index] = builder.float64_add(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatSubtract) {
        mapped[index] =
            builder.float64_subtract(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatMultiply) {
        mapped[index] =
            builder.float64_multiply(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatDivide) {
        mapped[index] =
            builder.float64_divide(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatLessThan) {
        mapped[index] =
            builder.float64_less_than(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatLessEqual) {
        mapped[index] =
            builder.float64_less_equal(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatEqual) {
        mapped[index] = builder.float64_equal(mapped[lhs_id], mapped[rhs_id]);
      } else {
        mapped[index] =
            builder.float64_not_equal(mapped[lhs_id], mapped[rhs_id]);
      }
      continue;
    }
    if (known_constant[lhs_id] && known_constant[rhs_id]) {
      constant_value[index] = fold_binary(
          node.opcode, constant_value[lhs_id], constant_value[rhs_id]);
      mapped[index] = builder.constant(constant_value[index]);
      known_constant[index] = true;
      ++folded;
      changed = true;
      continue;
    }

    if (node.opcode == Opcode::kBitwiseXor && lhs_id == rhs_id) {
      mapped[index] = builder.constant(0);
      known_constant[index] = true;
      constant_value[index] = 0;
      ++simplified;
      changed = true;
      continue;
    }

    std::size_t replacement = node_count;
    if (node.opcode == Opcode::kAdd) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = rhs_id;
      }
    } else if (node.opcode == Opcode::kSubtract) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kMultiply) {
      if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = rhs_id;
      } else if (known_constant[lhs_id] && constant_value[lhs_id] == 1) {
        replacement = rhs_id;
      } else if (known_constant[rhs_id] && constant_value[rhs_id] == 1) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kBitwiseAnd) {
      if ((known_constant[lhs_id] && constant_value[lhs_id] == 0) ||
          (known_constant[rhs_id] && constant_value[rhs_id] == -1)) {
        replacement = lhs_id;
      } else if ((known_constant[rhs_id] && constant_value[rhs_id] == 0) ||
                 (known_constant[lhs_id] &&
                  constant_value[lhs_id] == -1)) {
        replacement = rhs_id;
      } else if (lhs_id == rhs_id) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kBitwiseOr) {
      if ((known_constant[lhs_id] && constant_value[lhs_id] == -1) ||
          (known_constant[rhs_id] && constant_value[rhs_id] == 0)) {
        replacement = lhs_id;
      } else if ((known_constant[rhs_id] && constant_value[rhs_id] == -1) ||
                 (known_constant[lhs_id] && constant_value[lhs_id] == 0)) {
        replacement = rhs_id;
      } else if (lhs_id == rhs_id) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kBitwiseXor) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = rhs_id;
      }
    } else if (node.opcode == Opcode::kShiftLeft) {
      if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kFloorDivide) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 1) {
        replacement = lhs_id;
      }
    }

    if (replacement != node_count) {
      mapped[index] = mapped[replacement];
      known_constant[index] = known_constant[replacement];
      constant_value[index] = constant_value[replacement];
      ++simplified;
      changed = true;
      continue;
    }

    if (node.opcode == Opcode::kAdd) {
      mapped[index] = builder.add(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kSubtract) {
      mapped[index] = builder.subtract(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kMultiply) {
      mapped[index] = builder.multiply(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kBitwiseAnd) {
      mapped[index] = builder.bitwise_and(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kBitwiseOr) {
      mapped[index] = builder.bitwise_or(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kShiftLeft) {
      mapped[index] = builder.shift_left(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kFloorDivide) {
      mapped[index] = builder.floor_divide(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kFloorModulo) {
      mapped[index] = builder.floor_modulo(mapped[lhs_id], mapped[rhs_id]);
    } else {
      mapped[index] = builder.bitwise_xor(mapped[lhs_id], mapped[rhs_id]);
    }
  }

  const Status return_status =
      builder.set_return(mapped[input.return_value().id()]);
  if (!return_status.ok()) {
    return {};
  }
  Function output = std::move(builder).build();
  std::vector<OptimizationExitState> mapped_exit_states;
  mapped_exit_states.reserve(exit_states.size());
  for (const OptimizationExitState& exit_state : exit_states) {
    const Value mapped_exit = mapped[exit_state.exit.id()];
    if (!mapped_exit.valid() ||
        !is_nonzero_guard(output.nodes()[mapped_exit.id()].opcode)) {
      continue;
    }
    OptimizationExitState mapped_state;
    mapped_state.exit = mapped_exit;
    mapped_state.live_values.reserve(exit_state.live_values.size());
    for (const Value value : exit_state.live_values) {
      const Value mapped_value = mapped[value.id()];
      if (mapped_value.valid() &&
          std::find(mapped_state.live_values.begin(),
                    mapped_state.live_values.end(), mapped_value) ==
              mapped_state.live_values.end()) {
        mapped_state.live_values.push_back(mapped_value);
      }
    }
    mapped_exit_states.push_back(std::move(mapped_state));
  }
  return {std::move(output), std::move(mapped),
          std::move(mapped_exit_states), folded, simplified, changed};
}

struct ControlValueKey final {
  ControlOpcode opcode{ControlOpcode::kConstant};
  ValueType type{ValueType::kWord};
  std::uint32_t lhs{Value::kInvalidId};
  std::uint32_t rhs{Value::kInvalidId};
  Word immediate{0};

  bool operator==(const ControlValueKey& other) const noexcept {
    return opcode == other.opcode && type == other.type && lhs == other.lhs &&
           rhs == other.rhs && immediate == other.immediate;
  }
};

struct ControlValueKeyHash final {
  std::size_t operator()(const ControlValueKey& key) const noexcept {
    std::size_t result = static_cast<std::size_t>(key.opcode);
    const auto combine = [&result](std::uint64_t value) {
      result ^= static_cast<std::size_t>(
          value + 0x9E3779B97F4A7C15ULL + (result << 6U) + (result >> 2U));
    };
    combine(static_cast<std::uint64_t>(key.type));
    combine(key.lhs);
    combine(key.rhs);
    combine(to_bits(key.immediate));
    return result;
  }
};

struct ControlFlowCanonicalizationResult final {
  Status status;
  ControlFlowFunction function;
  std::vector<Value> mapping;
  std::size_t common_subexpressions{0};
  std::size_t branches_folded{0};
};

ControlFlowCanonicalizationResult canonicalize_control_flow(
    const ControlFlowFunction& input,
    const std::vector<OptimizationExitState>& exit_states) {
  const std::size_t node_count = input.nodes().size();
  const std::size_t block_count = input.blocks().size();
  const std::size_t no_owner = block_count;
  std::vector<std::size_t> owners(node_count, no_owner);
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    for (const Value instruction : input.blocks()[block_index].instructions) {
      owners[instruction.id()] = block_index;
    }
  }

  const auto constant_branch_edge = [&input](
                                        const ControlTerminator& terminator)
      -> const ControlEdge* {
    if (terminator.opcode != TerminatorOpcode::kBranch) {
      return nullptr;
    }
    const ControlNode& condition = input.nodes()[terminator.value.id()];
    if (condition.opcode != ControlOpcode::kConstant) {
      return nullptr;
    }
    return condition.immediate != 0 ? &terminator.true_edge
                                    : &terminator.false_edge;
  };

  std::vector<bool> reachable(block_count, false);
  reachable[input.entry_block().id()] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block_index = 0; block_index < block_count;
         ++block_index) {
      if (!reachable[block_index]) {
        continue;
      }
      const ControlTerminator& terminator =
          input.blocks()[block_index].terminator;
      const ControlEdge* folded_edge = constant_branch_edge(terminator);
      const auto mark = [&](const ControlEdge& edge) {
        if (!reachable[edge.target.id()]) {
          reachable[edge.target.id()] = true;
          changed = true;
        }
      };
      if (folded_edge != nullptr) {
        mark(*folded_edge);
      } else if (terminator.opcode == TerminatorOpcode::kJump) {
        mark(terminator.true_edge);
      } else if (terminator.opcode == TerminatorOpcode::kBranch) {
        mark(terminator.true_edge);
        mark(terminator.false_edge);
      }
    }
  }

  std::vector<ValueType> parameter_types;
  parameter_types.reserve(input.parameter_count());
  for (std::size_t index = 0; index < input.parameter_count(); ++index) {
    parameter_types.push_back(input.parameter_type(index));
  }
  ControlFlowBuilder builder(std::move(parameter_types));
  std::vector<Block> blocks(block_count);
  blocks[input.entry_block().id()] = builder.entry_block();
  std::vector<Value> mapped(node_count);
  const BasicBlock& input_entry = input.blocks()[input.entry_block().id()];
  for (std::size_t index = 0; index < input_entry.parameters.size(); ++index) {
    mapped[input_entry.parameters[index].id()] = builder.parameter(index);
  }
  for (std::size_t block_index = 1; block_index < block_count; ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    const BasicBlock& input_block = input.blocks()[block_index];
    std::vector<ValueType> block_parameter_types;
    block_parameter_types.reserve(input_block.parameters.size());
    for (const Value parameter : input_block.parameters) {
      block_parameter_types.push_back(input.value_type(parameter));
    }
    blocks[block_index] =
        builder.create_block(std::move(block_parameter_types));
    if (!blocks[block_index].valid()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer could not retain a reachable CFG block",
               block_index},
              {}, {}, 0, 0};
    }
    for (std::size_t index = 0; index < input_block.parameters.size();
         ++index) {
      mapped[input_block.parameters[index].id()] =
          builder.block_parameter(blocks[block_index], index);
    }
  }

  std::vector<bool> live(node_count, false);
  for (std::size_t index = 0; index < node_count; ++index) {
    const ControlOpcode opcode = input.nodes()[index].opcode;
    const bool reachable_owner =
        owners[index] != no_owner && reachable[owners[index]];
    live[index] = reachable_owner &&
                  (opcode == ControlOpcode::kParameter ||
                   opcode == ControlOpcode::kBlockParameter ||
                   opcode == ControlOpcode::kCall ||
                   is_nonzero_guard(opcode) ||
                   opcode == ControlOpcode::kSafepoint);
  }
  const auto mark_edge = [&live](const ControlEdge& edge) {
    for (const Value argument : edge.arguments) {
      live[argument.id()] = true;
    }
  };
  std::size_t branches_folded = 0;
  for (std::size_t block_index = 0; block_index < block_count; ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    const ControlTerminator& terminator = input.blocks()[block_index].terminator;
    const ControlEdge* folded_edge = constant_branch_edge(terminator);
    if (folded_edge != nullptr) {
      ++branches_folded;
      mark_edge(*folded_edge);
    } else {
      if (terminator.opcode == TerminatorOpcode::kReturn ||
          terminator.opcode == TerminatorOpcode::kBranch) {
        live[terminator.value.id()] = true;
      }
      if (terminator.opcode == TerminatorOpcode::kJump) {
        mark_edge(terminator.true_edge);
      } else if (terminator.opcode == TerminatorOpcode::kBranch) {
        mark_edge(terminator.true_edge);
        mark_edge(terminator.false_edge);
      }
    }
  }
  for (const OptimizationExitState& exit_state : exit_states) {
    if (exit_state.exit.valid() &&
        owners[exit_state.exit.id()] != no_owner &&
        reachable[owners[exit_state.exit.id()]]) {
      for (const Value value : exit_state.live_values) {
        live[value.id()] = true;
      }
    }
  }
  for (std::size_t reverse = node_count; reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    if (!live[index]) {
      continue;
    }
    const ControlNode& node = input.nodes()[index];
    if (node.opcode == ControlOpcode::kCall) {
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        live[argument.id()] = true;
      }
    } else if (is_nonzero_guard(node.opcode)) {
      live[node.lhs.id()] = true;
    } else if (is_control_unary(node.opcode)) {
      live[node.lhs.id()] = true;
    } else if (is_control_binary(node.opcode)) {
      live[node.lhs.id()] = true;
      live[node.rhs.id()] = true;
    }
  }

  std::vector<std::unordered_map<ControlValueKey, Value, ControlValueKeyHash>>
      available(block_count);
  std::size_t common_subexpressions = 0;
  for (std::size_t index = 0; index < node_count; ++index) {
    const ControlNode& node = input.nodes()[index];
    if (node.opcode == ControlOpcode::kParameter ||
        node.opcode == ControlOpcode::kBlockParameter || !live[index]) {
      continue;
    }
    const std::size_t owner = owners[index];
    if (owner == no_owner || !reachable[owner] ||
        !builder.set_insertion_block(blocks[owner]).ok()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer lost a reachable CFG node", index},
              {}, {}, 0, 0};
    }

    ControlValueKey key;
    bool reusable = false;
    if (node.opcode == ControlOpcode::kConstant) {
      key = {node.opcode, node.type, Value::kInvalidId, Value::kInvalidId,
             node.immediate};
      reusable = true;
    } else if (is_control_unary(node.opcode)) {
      key = {node.opcode, node.type, mapped[node.lhs.id()].id(),
             Value::kInvalidId, 0};
      reusable = true;
    } else if (is_control_binary(node.opcode)) {
      key = {node.opcode, node.type, mapped[node.lhs.id()].id(),
             mapped[node.rhs.id()].id(), 0};
      reusable = true;
    }
    if (reusable) {
      const auto existing = available[owner].find(key);
      if (existing != available[owner].end()) {
        mapped[index] = existing->second;
        ++common_subexpressions;
        continue;
      }
    }

    if (node.opcode == ControlOpcode::kConstant) {
      mapped[index] = node.type == ValueType::kFloat64
                          ? builder.float64_constant_bits(node.immediate)
                          : builder.constant(node.immediate);
    } else if (node.opcode == ControlOpcode::kCall) {
      std::vector<Value> arguments;
      arguments.reserve(node.argument_count);
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        arguments.push_back(mapped[argument.id()]);
      }
      mapped[index] = builder.call(unpack_runtime_helper(node.immediate),
                                   std::move(arguments), node.type);
    } else if (node.opcode == ControlOpcode::kSafepoint) {
      mapped[index] =
          builder.safepoint(static_cast<std::size_t>(node.immediate));
    } else if (is_nonzero_guard(node.opcode)) {
      mapped[index] =
          node.opcode == ControlOpcode::kGuardWordNonzero
              ? builder.guard_word_nonzero(
                    mapped[node.lhs.id()],
                    static_cast<std::size_t>(node.immediate))
              : builder.guard_float64_nonzero(
                    mapped[node.lhs.id()],
                    static_cast<std::size_t>(node.immediate));
    } else if (is_control_unary(node.opcode)) {
      mapped[index] =
          emit_control_unary(&builder, node.opcode, mapped[node.lhs.id()]);
    } else if (is_control_binary(node.opcode)) {
      mapped[index] = emit_control_binary(
          &builder, node.opcode, mapped[node.lhs.id()], mapped[node.rhs.id()]);
    }
    if (!mapped[index].valid()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer could not canonicalize a CFG node", index},
              {}, {}, 0, 0};
    }
    if (node.opcode == ControlOpcode::kCall ||
        is_nonzero_guard(node.opcode) ||
        node.opcode == ControlOpcode::kSafepoint) {
      available[owner].clear();
    }
    if (reusable) {
      available[owner].emplace(key, mapped[index]);
    }
  }

  const auto map_edge_values = [&mapped](const ControlEdge& edge) {
    std::vector<Value> values;
    values.reserve(edge.arguments.size());
    for (const Value argument : edge.arguments) {
      values.push_back(mapped[argument.id()]);
    }
    return values;
  };
  for (std::size_t block_index = 0; block_index < block_count; ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    const Status insertion = builder.set_insertion_block(blocks[block_index]);
    if (!insertion.ok()) {
      return {insertion, {}, {}, 0, 0};
    }
    const ControlTerminator& terminator = input.blocks()[block_index].terminator;
    const ControlEdge* folded_edge = constant_branch_edge(terminator);
    Status status;
    if (folded_edge != nullptr) {
      status = builder.jump(blocks[folded_edge->target.id()],
                            map_edge_values(*folded_edge));
    } else if (terminator.opcode == TerminatorOpcode::kReturn) {
      status = builder.set_return(mapped[terminator.value.id()]);
    } else if (terminator.opcode == TerminatorOpcode::kJump) {
      status = builder.jump(blocks[terminator.true_edge.target.id()],
                            map_edge_values(terminator.true_edge));
    } else if (terminator.opcode == TerminatorOpcode::kBranch) {
      status = builder.branch(
          mapped[terminator.value.id()],
          blocks[terminator.true_edge.target.id()],
          map_edge_values(terminator.true_edge),
          blocks[terminator.false_edge.target.id()],
          map_edge_values(terminator.false_edge));
    } else {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer encountered an unterminated reachable CFG block",
               block_index},
              {}, {}, 0, 0};
    }
    if (!status.ok()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer could not canonicalize a CFG terminator",
               block_index},
              {}, {}, 0, 0};
    }
  }

  ControlFlowFunction output = std::move(builder).build();
  const Status output_verification = verify(output);
  if (!output_verification.ok()) {
    return {{StatusCode::kCodeGenerationFailed,
             "optimizer produced invalid canonical CFG",
             output_verification.location()},
            {}, {}, 0, 0};
  }
  return {Status::ok_status(), std::move(output), std::move(mapped),
          common_subexpressions, branches_folded};
}

}  // namespace

OptimizationResult Optimizer::run(
    const Function& function,
    const std::vector<OptimizationExitState>& exit_states) {
  const Status verification = verify(function);
  if (!verification.ok()) {
    return {verification, {}, {}, {}};
  }
  for (const OptimizationExitState& exit_state : exit_states) {
    if (!exit_state.exit.valid() ||
        exit_state.exit.id() >= function.nodes().size() ||
        !is_nonzero_guard(
            function.nodes()[exit_state.exit.id()].opcode)) {
      return {{StatusCode::kInvalidArgument,
               "optimizer exit state does not identify a guard"},
              {}, {}, {}};
    }
    for (const Value value : exit_state.live_values) {
      if (!value.valid() || value.id() >= exit_state.exit.id()) {
        return {{StatusCode::kInvalidArgument,
                 "optimizer exit state value is unavailable at its guard",
                 exit_state.exit.id()},
                {}, {}, {}};
      }
    }
  }

  try {
    const std::size_t input_nodes = function.nodes().size();
    PassResult pass = transform_once(function, exit_states);
    std::vector<Value> value_mapping = pass.mapping;
    std::size_t folded = pass.constants_folded;
    std::size_t simplified = pass.algebraic_simplifications;

    constexpr std::size_t kMaximumCanonicalizationPasses = 8;
    for (std::size_t iteration = 1;
         pass.changed && iteration < kMaximumCanonicalizationPasses;
         ++iteration) {
      PassResult next = transform_once(pass.function, pass.exit_states);
      for (Value& value : value_mapping) {
        value = value.valid() && value.id() < next.mapping.size()
                    ? next.mapping[value.id()]
                    : Value{};
      }
      folded += next.constants_folded;
      simplified += next.algebraic_simplifications;
      pass = std::move(next);
    }

    const Status output_verification = verify(pass.function);
    if (!output_verification.ok()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer produced invalid SSA", output_verification.location()},
              {}, {}, {}};
    }
    const std::size_t output_nodes = pass.function.nodes().size();
    return {Status::ok_status(), std::move(pass.function),
            {input_nodes, output_nodes, folded, simplified},
            std::move(value_mapping)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate SSA optimization state"},
            {}, {}, {}};
  }
}

ControlFlowOptimizationResult Optimizer::run(
    const ControlFlowFunction& function,
    const std::vector<OptimizationExitState>& exit_states) {
  const Status verification = verify(function);
  if (!verification.ok()) {
    return {verification, {}, {}, {}};
  }

  try {
    const std::size_t node_count = function.nodes().size();
    const std::size_t block_count = function.blocks().size();
    std::vector<ValueType> parameter_types;
    parameter_types.reserve(function.parameter_count());
    for (std::size_t index = 0; index < function.parameter_count(); ++index) {
      parameter_types.push_back(function.parameter_type(index));
    }
    ControlFlowBuilder builder(std::move(parameter_types));
    std::vector<Block> blocks(block_count);
    blocks[function.entry_block().id()] = builder.entry_block();
    std::vector<Value> mapped(node_count);
    const BasicBlock& input_entry =
        function.blocks()[function.entry_block().id()];
    for (std::size_t index = 0; index < input_entry.parameters.size();
         ++index) {
      mapped[input_entry.parameters[index].id()] = builder.parameter(index);
    }
    for (std::size_t block_index = 1; block_index < block_count;
         ++block_index) {
      const BasicBlock& input_block = function.blocks()[block_index];
      std::vector<ValueType> block_parameter_types;
      block_parameter_types.reserve(input_block.parameters.size());
      for (const Value parameter : input_block.parameters) {
        block_parameter_types.push_back(function.value_type(parameter));
      }
      blocks[block_index] =
          builder.create_block(std::move(block_parameter_types));
      if (!blocks[block_index].valid()) {
        return {{StatusCode::kCodeGenerationFailed,
                 "optimizer could not create a control-flow block",
                 block_index},
                {}, {}, {}};
      }
      for (std::size_t index = 0; index < input_block.parameters.size();
           ++index) {
        mapped[input_block.parameters[index].id()] =
            builder.block_parameter(blocks[block_index], index);
      }
    }

    const std::size_t no_owner = block_count;
    std::vector<std::size_t> owners(node_count, no_owner);
    for (std::size_t block_index = 0; block_index < block_count;
         ++block_index) {
      for (const Value instruction :
           function.blocks()[block_index].instructions) {
        owners[instruction.id()] = block_index;
      }
    }
    for (const OptimizationExitState& exit_state : exit_states) {
      if (!exit_state.exit.valid() ||
          exit_state.exit.id() >= function.nodes().size() ||
          !is_nonzero_guard(
              function.nodes()[exit_state.exit.id()].opcode)) {
        return {{StatusCode::kInvalidArgument,
                 "CFG optimizer exit state does not identify a guard"},
                {}, {}, {}};
      }
      for (const Value value : exit_state.live_values) {
        if (!control_value_available(function, value, exit_state.exit)) {
          return {{StatusCode::kInvalidArgument,
                   "CFG optimizer exit state value does not dominate its guard",
                   exit_state.exit.id()},
                  {}, {}, {}};
        }
      }
    }

    std::vector<bool> known_constant(node_count, false);
    std::vector<Word> constant_value(node_count, 0);
    std::vector<bool> folded_node(node_count, false);
    std::vector<std::size_t> replacement(node_count, node_count);
    for (std::size_t index = 0; index < node_count; ++index) {
      const ControlNode& node = function.nodes()[index];
      if (node.opcode == ControlOpcode::kConstant) {
        known_constant[index] = true;
        constant_value[index] = node.immediate;
      } else if (is_control_unary(node.opcode)) {
        const std::size_t operand_id = node.lhs.id();
        if (known_constant[operand_id]) {
          constant_value[index] =
              fold_control_unary(node.opcode, constant_value[operand_id]);
          known_constant[index] = true;
          folded_node[index] = true;
        } else if (function.nodes()[operand_id].opcode == node.opcode) {
          replacement[index] = function.nodes()[operand_id].lhs.id();
          known_constant[index] = known_constant[replacement[index]];
          constant_value[index] = constant_value[replacement[index]];
        }
      } else if (is_control_binary(node.opcode)) {
        const std::size_t lhs_id = node.lhs.id();
        const std::size_t rhs_id = node.rhs.id();
        if (known_constant[lhs_id] && known_constant[rhs_id]) {
          constant_value[index] = fold_control_binary(
              node.opcode, constant_value[lhs_id], constant_value[rhs_id]);
          known_constant[index] = true;
          folded_node[index] = true;
        } else if (node.opcode == ControlOpcode::kAdd) {
          if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
            replacement[index] = lhs_id;
          } else if (known_constant[lhs_id] &&
                     constant_value[lhs_id] == 0) {
            replacement[index] = rhs_id;
          }
        } else if (node.opcode == ControlOpcode::kSubtract) {
          if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
            replacement[index] = lhs_id;
          }
        } else if (node.opcode == ControlOpcode::kMultiply) {
          if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
            replacement[index] = lhs_id;
          } else if (known_constant[rhs_id] &&
                     constant_value[rhs_id] == 0) {
            replacement[index] = rhs_id;
          } else if (known_constant[lhs_id] &&
                     constant_value[lhs_id] == 1) {
            replacement[index] = rhs_id;
          } else if (known_constant[rhs_id] &&
                     constant_value[rhs_id] == 1) {
            replacement[index] = lhs_id;
          }
        } else if (node.opcode == ControlOpcode::kBitwiseAnd) {
          if ((known_constant[lhs_id] && constant_value[lhs_id] == 0) ||
              (known_constant[rhs_id] && constant_value[rhs_id] == -1) ||
              lhs_id == rhs_id) {
            replacement[index] = lhs_id;
          } else if ((known_constant[rhs_id] &&
                      constant_value[rhs_id] == 0) ||
                     (known_constant[lhs_id] &&
                      constant_value[lhs_id] == -1)) {
            replacement[index] = rhs_id;
          }
        } else if (node.opcode == ControlOpcode::kBitwiseOr) {
          if ((known_constant[lhs_id] && constant_value[lhs_id] == -1) ||
              (known_constant[rhs_id] && constant_value[rhs_id] == 0) ||
              lhs_id == rhs_id) {
            replacement[index] = lhs_id;
          } else if ((known_constant[rhs_id] &&
                      constant_value[rhs_id] == -1) ||
                     (known_constant[lhs_id] &&
                      constant_value[lhs_id] == 0)) {
            replacement[index] = rhs_id;
          }
        } else if (node.opcode == ControlOpcode::kBitwiseXor) {
          if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
            replacement[index] = lhs_id;
          } else if (known_constant[lhs_id] &&
                     constant_value[lhs_id] == 0) {
            replacement[index] = rhs_id;
          }
        } else if (node.opcode == ControlOpcode::kShiftLeft) {
          if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
            replacement[index] = lhs_id;
          } else if (known_constant[rhs_id] &&
                     constant_value[rhs_id] == 0) {
            replacement[index] = lhs_id;
          }
        }
        if (replacement[index] != node_count) {
          known_constant[index] = known_constant[replacement[index]];
          constant_value[index] = constant_value[replacement[index]];
        }
      }
    }

    std::vector<bool> live(node_count, false);
    for (std::size_t index = 0; index < node_count; ++index) {
      const ControlOpcode opcode = function.nodes()[index].opcode;
      live[index] = opcode == ControlOpcode::kParameter ||
                    opcode == ControlOpcode::kBlockParameter ||
                    opcode == ControlOpcode::kCall ||
                    is_nonzero_guard(opcode) ||
                    opcode == ControlOpcode::kSafepoint;
    }
    const auto mark_edge = [&live](const ControlEdge& edge) {
      for (const Value argument : edge.arguments) {
        live[argument.id()] = true;
      }
    };
    for (const BasicBlock& block : function.blocks()) {
      if (block.terminator.opcode == TerminatorOpcode::kReturn ||
          block.terminator.opcode == TerminatorOpcode::kBranch) {
        live[block.terminator.value.id()] = true;
      }
      if (block.terminator.opcode == TerminatorOpcode::kJump) {
        mark_edge(block.terminator.true_edge);
      } else if (block.terminator.opcode == TerminatorOpcode::kBranch) {
        mark_edge(block.terminator.true_edge);
        mark_edge(block.terminator.false_edge);
      }
    }
    for (const OptimizationExitState& exit_state : exit_states) {
      for (const Value value : exit_state.live_values) {
        live[value.id()] = true;
      }
    }
    for (std::size_t reverse = node_count; reverse > 0; --reverse) {
      const std::size_t index = reverse - 1;
      if (!live[index]) {
        continue;
      }
      const ControlNode& node = function.nodes()[index];
      if (node.opcode == ControlOpcode::kCall) {
        for (std::size_t argument_index = 0;
             argument_index < node.argument_count; ++argument_index) {
          const Value argument = function.call_arguments()[
              static_cast<std::size_t>(node.argument_begin) + argument_index];
          live[argument.id()] = true;
        }
      } else if (is_nonzero_guard(node.opcode)) {
        live[node.lhs.id()] = true;
      } else if (is_control_unary(node.opcode)) {
        if (replacement[index] != node_count) {
          live[replacement[index]] = true;
        } else if (!folded_node[index]) {
          live[node.lhs.id()] = true;
        }
      } else if (is_control_binary(node.opcode)) {
        if (replacement[index] != node_count) {
          live[replacement[index]] = true;
        } else if (!folded_node[index]) {
          live[node.lhs.id()] = true;
          live[node.rhs.id()] = true;
        }
      }
    }

    std::size_t folded = 0;
    std::size_t simplified = 0;
    for (std::size_t index = 0; index < node_count; ++index) {
      const ControlNode& node = function.nodes()[index];
      if (node.opcode == ControlOpcode::kParameter ||
          node.opcode == ControlOpcode::kBlockParameter || !live[index]) {
        continue;
      }
      if (owners[index] == no_owner ||
          !builder.set_insertion_block(blocks[owners[index]]).ok()) {
        return {{StatusCode::kCodeGenerationFailed,
                 "optimizer lost control-flow node ownership", index},
                {}, {}, {}};
      }
      if (node.opcode == ControlOpcode::kConstant) {
        mapped[index] = node.type == ValueType::kFloat64
                            ? builder.float64_constant_bits(node.immediate)
                            : builder.constant(node.immediate);
      } else if (node.opcode == ControlOpcode::kCall) {
        std::vector<Value> arguments;
        arguments.reserve(node.argument_count);
        for (std::size_t argument_index = 0;
             argument_index < node.argument_count; ++argument_index) {
          const Value argument = function.call_arguments()[
              static_cast<std::size_t>(node.argument_begin) + argument_index];
          arguments.push_back(mapped[argument.id()]);
        }
        mapped[index] = builder.call(unpack_runtime_helper(node.immediate),
                                     std::move(arguments), node.type);
      } else if (node.opcode == ControlOpcode::kSafepoint) {
        mapped[index] =
            builder.safepoint(static_cast<std::size_t>(node.immediate));
      } else if (is_nonzero_guard(node.opcode)) {
        mapped[index] =
            node.opcode == ControlOpcode::kGuardWordNonzero
                ? builder.guard_word_nonzero(
                      mapped[node.lhs.id()],
                      static_cast<std::size_t>(node.immediate))
                : builder.guard_float64_nonzero(
                      mapped[node.lhs.id()],
                      static_cast<std::size_t>(node.immediate));
      } else if (is_control_unary(node.opcode)) {
        if (folded_node[index]) {
          mapped[index] = node.type == ValueType::kFloat64
                              ? builder.float64_constant_bits(
                                    constant_value[index])
                              : builder.constant(constant_value[index]);
          ++folded;
        } else if (replacement[index] != node_count) {
          mapped[index] = mapped[replacement[index]];
          ++simplified;
        } else {
          mapped[index] = emit_control_unary(
              &builder, node.opcode, mapped[node.lhs.id()]);
        }
      } else if (is_control_binary(node.opcode)) {
        const std::size_t lhs_id = node.lhs.id();
        const std::size_t rhs_id = node.rhs.id();
        if (folded_node[index]) {
          mapped[index] =
              is_control_float_binary(node.opcode) &&
                      !is_control_comparison(node.opcode)
                  ? builder.float64_constant_bits(constant_value[index])
                  : builder.constant(constant_value[index]);
          ++folded;
        } else if (replacement[index] != node_count) {
          mapped[index] = mapped[replacement[index]];
          ++simplified;
        } else {
          mapped[index] = emit_control_binary(
              &builder, node.opcode, mapped[lhs_id], mapped[rhs_id]);
        }
      }
      if (!mapped[index].valid()) {
        return {{StatusCode::kCodeGenerationFailed,
                 "optimizer could not rebuild a control-flow node", index},
                {}, {}, {}};
      }
    }

    const auto map_edge_values = [&mapped](const ControlEdge& edge) {
      std::vector<Value> values;
      values.reserve(edge.arguments.size());
      for (const Value argument : edge.arguments) {
        values.push_back(mapped[argument.id()]);
      }
      return values;
    };
    for (std::size_t block_index = 0; block_index < block_count;
         ++block_index) {
      const Status insertion =
          builder.set_insertion_block(blocks[block_index]);
      if (!insertion.ok()) {
        return {insertion, {}, {}, {}};
      }
      const ControlTerminator& terminator =
          function.blocks()[block_index].terminator;
      Status status;
      if (terminator.opcode == TerminatorOpcode::kReturn) {
        status = builder.set_return(mapped[terminator.value.id()]);
      } else if (terminator.opcode == TerminatorOpcode::kJump) {
        status = builder.jump(blocks[terminator.true_edge.target.id()],
                              map_edge_values(terminator.true_edge));
      } else if (terminator.opcode == TerminatorOpcode::kBranch) {
        status = builder.branch(
            mapped[terminator.value.id()],
            blocks[terminator.true_edge.target.id()],
            map_edge_values(terminator.true_edge),
            blocks[terminator.false_edge.target.id()],
            map_edge_values(terminator.false_edge));
      } else {
        return {{StatusCode::kCodeGenerationFailed,
                 "optimizer encountered an unterminated control-flow block",
                 block_index},
                {}, {}, {}};
      }
      if (!status.ok()) {
        return {{StatusCode::kCodeGenerationFailed,
                 "optimizer could not rebuild a control-flow terminator",
                 block_index},
                {}, {}, {}};
      }
    }

    ControlFlowFunction output = std::move(builder).build();
    const Status output_verification = verify(output);
    if (!output_verification.ok()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer produced invalid control-flow SSA",
               output_verification.location()},
              {}, {}, {}};
    }
    std::vector<OptimizationExitState> mapped_exit_states;
    mapped_exit_states.reserve(exit_states.size());
    for (const OptimizationExitState& exit_state : exit_states) {
      const Value mapped_exit = mapped[exit_state.exit.id()];
      if (!mapped_exit.valid() ||
          !is_nonzero_guard(output.nodes()[mapped_exit.id()].opcode)) {
        continue;
      }
      OptimizationExitState mapped_state;
      mapped_state.exit = mapped_exit;
      mapped_state.live_values.reserve(exit_state.live_values.size());
      for (const Value value : exit_state.live_values) {
        const Value mapped_value = mapped[value.id()];
        if (mapped_value.valid() &&
            std::find(mapped_state.live_values.begin(),
                      mapped_state.live_values.end(), mapped_value) ==
                mapped_state.live_values.end()) {
          mapped_state.live_values.push_back(mapped_value);
        }
      }
      mapped_exit_states.push_back(std::move(mapped_state));
    }

    ControlFlowCanonicalizationResult canonical =
        canonicalize_control_flow(output, mapped_exit_states);
    if (!canonical.status.ok()) {
      return {canonical.status, {}, {}, {}};
    }
    for (Value& value : mapped) {
      value = value.valid() && value.id() < canonical.mapping.size()
                  ? canonical.mapping[value.id()]
                  : Value{};
    }
    const std::size_t output_nodes = canonical.function.nodes().size();
    return {Status::ok_status(), std::move(canonical.function),
            {node_count, output_nodes, folded, simplified,
             canonical.common_subexpressions, canonical.branches_folded},
            std::move(mapped)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate control-flow optimization state"},
            {}, {}, {}};
  }
}

}  // namespace unijit::ir
