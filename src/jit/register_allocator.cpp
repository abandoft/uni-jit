#include "jit/register_allocator.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

#include "unijit/runtime/execution_context.h"

namespace unijit::jit::detail {
namespace {

constexpr std::size_t kMaximumStackMapValues = 256U * 1024U;

template <typename Visitor>
void visit_control_operands(const ir::ControlFlowFunction& function,
                            const ir::ControlNode& node, Visitor&& visitor) {
  switch (node.opcode) {
    case ir::ControlOpcode::kAdd:
    case ir::ControlOpcode::kSubtract:
    case ir::ControlOpcode::kMultiply:
    case ir::ControlOpcode::kBitwiseAnd:
    case ir::ControlOpcode::kBitwiseOr:
    case ir::ControlOpcode::kBitwiseXor:
    case ir::ControlOpcode::kShiftLeft:
    case ir::ControlOpcode::kFloorDivide:
    case ir::ControlOpcode::kFloorModulo:
    case ir::ControlOpcode::kFloatAdd:
    case ir::ControlOpcode::kFloatSubtract:
    case ir::ControlOpcode::kFloatMultiply:
    case ir::ControlOpcode::kFloatDivide:
    case ir::ControlOpcode::kFloatLessThan:
    case ir::ControlOpcode::kFloatLessEqual:
    case ir::ControlOpcode::kFloatEqual:
    case ir::ControlOpcode::kFloatNotEqual:
    case ir::ControlOpcode::kLessThan:
    case ir::ControlOpcode::kLessEqual:
      visitor(node.lhs);
      visitor(node.rhs);
      break;
    case ir::ControlOpcode::kNegate:
    case ir::ControlOpcode::kBitwiseNot:
    case ir::ControlOpcode::kFloatNegate:
    case ir::ControlOpcode::kGuardWordNonzero:
    case ir::ControlOpcode::kGuardFloatNonzero:
      visitor(node.lhs);
      break;
    case ir::ControlOpcode::kCall:
      for (std::size_t index = 0; index < node.argument_count; ++index) {
        visitor(function.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + index]);
      }
      break;
    case ir::ControlOpcode::kParameter:
    case ir::ControlOpcode::kBlockParameter:
    case ir::ControlOpcode::kConstant:
    case ir::ControlOpcode::kSafepoint:
      break;
  }
}

void note_use(std::vector<std::size_t>* last_use, ir::Value value,
              std::size_t use_index) {
  auto& entry = (*last_use)[value.id()];
  entry = std::max(entry, use_index);
}

RegisterAllocation allocate_impl(const ir::Function& function,
                                 std::size_t register_count,
                                 std::size_t maximum_spill_slots,
                                 const StackMapRequirements& requirements) {
  if (register_count == 0) {
    return {{StatusCode::kInvalidArgument,
             "linear scan requires at least one allocatable register"},
            {}, 0, {}};
  }

  const std::size_t value_count = function.nodes().size();
  std::vector<std::size_t> last_use(value_count);
  for (std::size_t index = 0; index < value_count; ++index) {
    last_use[index] = index;
    const ir::Node& node = function.nodes()[index];
    if (node.opcode == ir::Opcode::kAdd ||
        node.opcode == ir::Opcode::kSubtract ||
        node.opcode == ir::Opcode::kMultiply ||
        node.opcode == ir::Opcode::kBitwiseAnd ||
        node.opcode == ir::Opcode::kBitwiseOr ||
        node.opcode == ir::Opcode::kBitwiseXor ||
        node.opcode == ir::Opcode::kShiftLeft ||
        node.opcode == ir::Opcode::kFloorDivide ||
        node.opcode == ir::Opcode::kFloorModulo ||
        node.opcode == ir::Opcode::kFloatAdd ||
        node.opcode == ir::Opcode::kFloatSubtract ||
        node.opcode == ir::Opcode::kFloatMultiply ||
        node.opcode == ir::Opcode::kFloatDivide ||
        node.opcode == ir::Opcode::kFloatLessThan ||
        node.opcode == ir::Opcode::kFloatLessEqual ||
        node.opcode == ir::Opcode::kFloatEqual ||
        node.opcode == ir::Opcode::kFloatNotEqual) {
      note_use(&last_use, node.lhs, index);
      note_use(&last_use, node.rhs, index);
    } else if (node.opcode == ir::Opcode::kNegate ||
               node.opcode == ir::Opcode::kBitwiseNot ||
               node.opcode == ir::Opcode::kFloatNegate ||
               node.opcode == ir::Opcode::kGuardWordNonzero ||
               node.opcode == ir::Opcode::kGuardFloatNonzero) {
      note_use(&last_use, node.lhs, index);
    } else if (node.opcode == ir::Opcode::kCall) {
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        note_use(&last_use,
                 function.call_arguments()[
                     static_cast<std::size_t>(node.argument_begin) +
                     argument_index],
                 index);
      }
    }
    if (node.opcode == ir::Opcode::kSafepoint ||
        node.opcode == ir::Opcode::kGuardWordNonzero ||
        node.opcode == ir::Opcode::kGuardFloatNonzero) {
      const StackMapRequirement* requirement = find_stack_map_requirement(
          requirements, static_cast<std::size_t>(node.immediate));
      if (requirement != nullptr) {
        for (const ir::Value value : requirement->values) {
          note_use(&last_use, value, index);
        }
      }
    }
  }
  note_use(&last_use, function.return_value(), value_count);

  std::vector<ValueLocation> locations(value_count);
  std::vector<std::size_t> active;
  std::vector<std::size_t> free_registers;
  free_registers.reserve(register_count);
  for (std::size_t index = register_count; index > 0; --index) {
    free_registers.push_back(index - 1);
  }
  std::size_t spill_slots = 0;

  for (std::size_t index = 0; index < value_count; ++index) {
    auto active_it = active.begin();
    while (active_it != active.end()) {
      const std::size_t active_value = *active_it;
      if (last_use[active_value] < index) {
        free_registers.push_back(locations[active_value].register_index);
        active_it = active.erase(active_it);
      } else {
        ++active_it;
      }
    }

    if (!free_registers.empty()) {
      locations[index].register_index = free_registers.back();
      free_registers.pop_back();
      active.push_back(index);
      continue;
    }

    const auto victim_it = std::max_element(
        active.begin(), active.end(), [&](std::size_t lhs, std::size_t rhs) {
          return last_use[lhs] < last_use[rhs];
        });
    if (victim_it != active.end() && last_use[*victim_it] > last_use[index]) {
      const std::size_t victim = *victim_it;
      locations[index].register_index = locations[victim].register_index;
      locations[victim].register_index = ValueLocation::kNone;
      locations[victim].spill_slot = spill_slots++;
      *victim_it = index;
    } else {
      locations[index].spill_slot = spill_slots++;
    }
  }

  for (std::size_t call_index = 0; call_index < value_count; ++call_index) {
    if (function.nodes()[call_index].opcode != ir::Opcode::kCall) {
      continue;
    }
    for (std::size_t value_index = 0; value_index < call_index;
         ++value_index) {
      ValueLocation& location = locations[value_index];
      if (location.in_register() && last_use[value_index] > call_index &&
          location.spill_slot == ValueLocation::kNone) {
        location.spill_slot = spill_slots++;
      }
    }
  }

  if (spill_slots > maximum_spill_slots) {
    return {{StatusCode::kResourceExhausted,
             "linear scan exceeded the backend spill-frame limit"},
            {}, 0, {}};
  }
  return {Status::ok_status(), std::move(locations), spill_slots,
          std::move(last_use)};
}

ControlFlowRegisterAllocation allocate_control_flow_impl(
    const ir::ControlFlowFunction& function, std::size_t word_register_count,
    std::size_t float_register_count,
    const StackMapRequirements& requirements) {
  if (word_register_count == 0 || float_register_count == 0) {
    return {{StatusCode::kInvalidArgument,
             "control-flow allocation requires Word and Float64 registers"},
            {}, {}, {}, {}};
  }

  const std::size_t value_count = function.nodes().size();
  const std::size_t no_owner = function.blocks().size();
  std::vector<std::size_t> owners(value_count, no_owner);
  std::vector<std::size_t> positions(value_count, ValueLocation::kNone);
  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const ir::Value value = block.instructions[index];
      owners[value.id()] = block_index;
      positions[value.id()] = index;
    }
  }

  std::vector<std::size_t> register_indices(value_count,
                                             ValueLocation::kNone);
  std::vector<std::vector<ir::Value>> live_across_calls(value_count);
  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    std::vector<std::size_t> last_use(block.instructions.size(), 0);
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      last_use[index] = index;
      const ir::ControlNode& node =
          function.nodes()[block.instructions[index].id()];
      const auto note_local_use = [&](ir::Value value) {
        if (value.valid() && owners[value.id()] == block_index) {
          const std::size_t definition = positions[value.id()];
          last_use[definition] = std::max(last_use[definition], index);
        }
      };
      switch (node.opcode) {
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kBitwiseAnd:
        case ir::ControlOpcode::kBitwiseOr:
        case ir::ControlOpcode::kBitwiseXor:
        case ir::ControlOpcode::kShiftLeft:
        case ir::ControlOpcode::kFloorDivide:
        case ir::ControlOpcode::kFloorModulo:
        case ir::ControlOpcode::kFloatAdd:
        case ir::ControlOpcode::kFloatSubtract:
        case ir::ControlOpcode::kFloatMultiply:
        case ir::ControlOpcode::kFloatDivide:
        case ir::ControlOpcode::kFloatLessThan:
        case ir::ControlOpcode::kFloatLessEqual:
        case ir::ControlOpcode::kFloatEqual:
        case ir::ControlOpcode::kFloatNotEqual:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          note_local_use(node.lhs);
          note_local_use(node.rhs);
          break;
        case ir::ControlOpcode::kNegate:
        case ir::ControlOpcode::kBitwiseNot:
        case ir::ControlOpcode::kFloatNegate:
        case ir::ControlOpcode::kGuardWordNonzero:
        case ir::ControlOpcode::kGuardFloatNonzero:
          note_local_use(node.lhs);
          break;
        case ir::ControlOpcode::kCall:
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            note_local_use(function.call_arguments()[
                static_cast<std::size_t>(node.argument_begin) +
                argument_index]);
          }
          break;
        case ir::ControlOpcode::kParameter:
        case ir::ControlOpcode::kBlockParameter:
        case ir::ControlOpcode::kConstant:
        case ir::ControlOpcode::kSafepoint:
          break;
      }
      if (node.opcode == ir::ControlOpcode::kSafepoint ||
          node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
          node.opcode == ir::ControlOpcode::kGuardFloatNonzero) {
        const StackMapRequirement* requirement = find_stack_map_requirement(
            requirements, static_cast<std::size_t>(node.immediate));
        if (requirement != nullptr) {
          for (const ir::Value required : requirement->values) {
            note_local_use(required);
          }
        }
      }
    }

    const std::size_t terminator_position = block.instructions.size();
    const auto note_terminator_use = [&](ir::Value value) {
      if (value.valid() && owners[value.id()] == block_index) {
        last_use[positions[value.id()]] = terminator_position;
      }
    };
    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kReturn ||
        terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_terminator_use(terminator.value);
    }
    const auto note_edge = [&](const ir::ControlEdge& edge) {
      for (const ir::Value argument : edge.arguments) {
        note_terminator_use(argument);
      }
    };
    if (terminator.opcode == ir::TerminatorOpcode::kJump) {
      note_edge(terminator.true_edge);
    } else if (terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_edge(terminator.true_edge);
      note_edge(terminator.false_edge);
    }

    std::vector<std::size_t> free_word_registers;
    free_word_registers.reserve(word_register_count);
    for (std::size_t index = word_register_count; index > 0; --index) {
      free_word_registers.push_back(index - 1);
    }
    std::vector<std::size_t> free_float_registers;
    free_float_registers.reserve(float_register_count);
    for (std::size_t index = float_register_count; index > 0; --index) {
      free_float_registers.push_back(index - 1);
    }
    std::vector<std::size_t> active_word;
    std::vector<std::size_t> active_float;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const ir::Value value = block.instructions[index];
      if (function.nodes()[value.id()].opcode == ir::ControlOpcode::kCall) {
        std::vector<ir::Value>& live = live_across_calls[value.id()];
        live.reserve(active_word.size() + active_float.size());
        const auto note_live = [&](const std::vector<std::size_t>& active) {
          for (const std::size_t position : active) {
            if (last_use[position] > index) {
              live.push_back(block.instructions[position]);
            }
          }
        };
        note_live(active_word);
        note_live(active_float);
      }
      const bool is_float =
          function.value_type(value) == ir::ValueType::kFloat64;
      std::vector<std::size_t>& free_registers =
          is_float ? free_float_registers : free_word_registers;
      std::vector<std::size_t>& active =
          is_float ? active_float : active_word;
      auto active_iterator = active.begin();
      while (active_iterator != active.end()) {
        const std::size_t active_position = *active_iterator;
        if (last_use[active_position] < index) {
          const ir::Value active_value =
              block.instructions[active_position];
          free_registers.push_back(register_indices[active_value.id()]);
          active_iterator = active.erase(active_iterator);
        } else {
          ++active_iterator;
        }
      }
      if (free_registers.empty()) {
        continue;
      }
      register_indices[value.id()] = free_registers.back();
      free_registers.pop_back();
      active.push_back(index);
    }
  }

  std::vector<bool> requires_stack(value_count, false);
  const auto note_nonlocal_use = [&](std::size_t block_index,
                                     ir::Value value) {
    if (value.valid() && owners[value.id()] != block_index) {
      requires_stack[value.id()] = true;
    }
  };
  for (std::size_t block_index = 0; block_index < function.blocks().size();
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (const ir::Value value : block.instructions) {
      const ir::ControlNode& node = function.nodes()[value.id()];
      switch (node.opcode) {
        case ir::ControlOpcode::kAdd:
        case ir::ControlOpcode::kSubtract:
        case ir::ControlOpcode::kMultiply:
        case ir::ControlOpcode::kBitwiseAnd:
        case ir::ControlOpcode::kBitwiseOr:
        case ir::ControlOpcode::kBitwiseXor:
        case ir::ControlOpcode::kShiftLeft:
        case ir::ControlOpcode::kFloorDivide:
        case ir::ControlOpcode::kFloorModulo:
        case ir::ControlOpcode::kFloatAdd:
        case ir::ControlOpcode::kFloatSubtract:
        case ir::ControlOpcode::kFloatMultiply:
        case ir::ControlOpcode::kFloatDivide:
        case ir::ControlOpcode::kFloatLessThan:
        case ir::ControlOpcode::kFloatLessEqual:
        case ir::ControlOpcode::kFloatEqual:
        case ir::ControlOpcode::kFloatNotEqual:
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          note_nonlocal_use(block_index, node.lhs);
          note_nonlocal_use(block_index, node.rhs);
          break;
        case ir::ControlOpcode::kNegate:
        case ir::ControlOpcode::kBitwiseNot:
        case ir::ControlOpcode::kFloatNegate:
        case ir::ControlOpcode::kGuardWordNonzero:
        case ir::ControlOpcode::kGuardFloatNonzero:
          note_nonlocal_use(block_index, node.lhs);
          break;
        case ir::ControlOpcode::kCall:
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            note_nonlocal_use(
                block_index,
                function.call_arguments()[
                    static_cast<std::size_t>(node.argument_begin) +
                    argument_index]);
          }
          break;
        case ir::ControlOpcode::kParameter:
        case ir::ControlOpcode::kBlockParameter:
        case ir::ControlOpcode::kConstant:
        case ir::ControlOpcode::kSafepoint:
          break;
      }
      if (node.opcode == ir::ControlOpcode::kSafepoint ||
          node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
          node.opcode == ir::ControlOpcode::kGuardFloatNonzero) {
        const StackMapRequirement* requirement = find_stack_map_requirement(
            requirements, static_cast<std::size_t>(node.immediate));
        if (requirement != nullptr) {
          for (const ir::Value required : requirement->values) {
            note_nonlocal_use(block_index, required);
          }
        }
      }
    }
    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kReturn ||
        terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_nonlocal_use(block_index, terminator.value);
    }
    const auto note_edge = [&](const ir::ControlEdge& edge) {
      for (const ir::Value argument : edge.arguments) {
        note_nonlocal_use(block_index, argument);
      }
    };
    if (terminator.opcode == ir::TerminatorOpcode::kJump) {
      note_edge(terminator.true_edge);
    } else if (terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_edge(terminator.true_edge);
      note_edge(terminator.false_edge);
    }
  }

  return {Status::ok_status(), std::move(register_indices),
          std::move(owners), std::move(requires_stack),
          std::move(live_across_calls)};
}

ControlFlowEdgeMoves plan_control_flow_edge_impl(
    const ir::ControlFlowFunction& function, const ir::ControlEdge& edge,
    const ControlFlowRegisterAllocation& allocation,
    std::size_t current_block) {
  const ir::BasicBlock& target = function.blocks()[edge.target.id()];
  std::vector<ControlFlowRegisterMove> pending;
  pending.reserve(edge.arguments.size());
  std::vector<ControlFlowRegisterMove> destinations;
  destinations.reserve(edge.arguments.size());
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    const ir::Value parameter = target.parameters[index];
    const ir::ValueType type = function.value_type(parameter);
    const std::size_t destination = allocation.register_indices[parameter.id()];
    const bool duplicate_destination = std::any_of(
        destinations.begin(), destinations.end(),
        [&](const ControlFlowRegisterMove& existing) {
          return existing.type == type &&
                 existing.destination_index == destination;
        });
    if (destination == ValueLocation::kNone || duplicate_destination) {
      return {false, {}};
    }
    destinations.push_back({ControlFlowMoveSource::kRegister, 0, destination,
                            type});

    const ir::Value argument = edge.arguments[index];
    const bool source_in_register =
        allocation.owner_blocks[argument.id()] == current_block &&
        allocation.register_indices[argument.id()] != ValueLocation::kNone;
    const ControlFlowMoveSource source_kind =
        source_in_register ? ControlFlowMoveSource::kRegister
                           : ControlFlowMoveSource::kStack;
    const std::size_t source =
        source_in_register ? allocation.register_indices[argument.id()]
                           : argument.id();
    if (source_kind == ControlFlowMoveSource::kRegister &&
        source == destination) {
      continue;
    }
    pending.push_back({source_kind, source, destination, type});
  }

  ControlFlowEdgeMoves result{true, {}};
  result.moves.reserve(pending.size() + 1);
  while (!pending.empty()) {
    auto ready = pending.end();
    for (auto candidate = pending.begin(); candidate != pending.end();
         ++candidate) {
      bool destination_is_source = false;
      for (const ControlFlowRegisterMove& move : pending) {
        if (move.type == candidate->type &&
            move.source_kind == ControlFlowMoveSource::kRegister &&
            move.source_index == candidate->destination_index) {
          destination_is_source = true;
          break;
        }
      }
      if (!destination_is_source) {
        ready = candidate;
        break;
      }
    }

    if (ready != pending.end()) {
      result.moves.push_back(*ready);
      pending.erase(ready);
      continue;
    }

    const std::size_t saved_register = pending.front().destination_index;
    const ir::ValueType saved_type = pending.front().type;
    result.moves.push_back({ControlFlowMoveSource::kRegister, saved_register,
                            ValueLocation::kNone, saved_type});
    for (ControlFlowRegisterMove& move : pending) {
      if (move.type == saved_type &&
          move.source_kind == ControlFlowMoveSource::kRegister &&
          move.source_index == saved_register) {
        move.source_kind = ControlFlowMoveSource::kTemporary;
        move.source_index = 0;
      }
    }
  }
  return result;
}

StackMapLiveness plan_straight_stack_map_liveness_impl(
    const ir::Function& function, const RegisterAllocation& allocation) {
  std::vector<std::vector<ir::Value>> live_values(function.nodes().size());
  std::size_t total_live_values = 0;
  for (std::size_t node_index = 0; node_index < function.nodes().size();
       ++node_index) {
    const ir::Opcode opcode = function.nodes()[node_index].opcode;
    if (opcode != ir::Opcode::kSafepoint &&
        opcode != ir::Opcode::kGuardWordNonzero &&
        opcode != ir::Opcode::kGuardFloatNonzero) {
      continue;
    }
    std::vector<ir::Value>& site_values = live_values[node_index];
    for (std::size_t value_index = 0; value_index < node_index;
         ++value_index) {
      if (allocation.last_uses[value_index] >= node_index) {
        if (site_values.size() ==
            runtime::ExecutionContext::kMaximumCapturedValues) {
          return {{StatusCode::kResourceExhausted,
                   "straight-line stack map exceeds the capture capacity"},
                  {}};
        }
        if (total_live_values == kMaximumStackMapValues) {
          return {{StatusCode::kResourceExhausted,
                   "straight-line stack maps exceed the metadata limit"},
                  {}};
        }
        site_values.emplace_back(static_cast<std::uint32_t>(value_index));
        ++total_live_values;
      }
    }
  }
  return {Status::ok_status(), std::move(live_values)};
}

StackMapLiveness plan_control_stack_map_liveness_impl(
    const ir::ControlFlowFunction& function,
    const StackMapRequirements& requirements) {
  const std::size_t block_count = function.blocks().size();
  const std::size_t value_count = function.nodes().size();
  using ValueSet = std::vector<bool>;
  std::vector<ValueSet> uses(block_count, ValueSet(value_count, false));
  std::vector<ValueSet> definitions(block_count,
                                    ValueSet(value_count, false));
  std::vector<ValueSet> live_in(block_count, ValueSet(value_count, false));
  std::vector<ValueSet> live_out(block_count, ValueSet(value_count, false));

  const auto note_use = [](ValueSet* used, const ValueSet& defined,
                           ir::Value value) {
    if (value.valid() && !defined[value.id()]) {
      (*used)[value.id()] = true;
    }
  };
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    ValueSet defined(value_count, false);
    for (const ir::Value value : block.instructions) {
      const ir::ControlNode& node = function.nodes()[value.id()];
      visit_control_operands(function, node,
                             [&](ir::Value operand) {
                               note_use(&uses[block_index], defined, operand);
                             });
      if (node.opcode == ir::ControlOpcode::kSafepoint ||
          node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
          node.opcode == ir::ControlOpcode::kGuardFloatNonzero) {
        const StackMapRequirement* requirement = find_stack_map_requirement(
            requirements, static_cast<std::size_t>(node.immediate));
        if (requirement != nullptr) {
          for (const ir::Value required : requirement->values) {
            note_use(&uses[block_index], defined, required);
          }
        }
      }
      if (node.opcode != ir::ControlOpcode::kBlockParameter) {
        defined[value.id()] = true;
        definitions[block_index][value.id()] = true;
      }
    }
    if (block.terminator.opcode == ir::TerminatorOpcode::kReturn ||
        block.terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_use(&uses[block_index], defined, block.terminator.value);
    }
  }

  const auto add_edge_live = [&](ValueSet* output,
                                 const ir::ControlEdge& edge) {
    const ir::BasicBlock& target = function.blocks()[edge.target.id()];
    const ValueSet& successor_live = live_in[edge.target.id()];
    for (std::size_t value_index = 0; value_index < value_count;
         ++value_index) {
      if (!successor_live[value_index]) {
        continue;
      }
      bool substituted = false;
      for (std::size_t parameter_index = 0;
           parameter_index < target.parameters.size(); ++parameter_index) {
        if (target.parameters[parameter_index].id() == value_index) {
          (*output)[edge.arguments[parameter_index].id()] = true;
          substituted = true;
          break;
        }
      }
      if (!substituted) {
        (*output)[value_index] = true;
      }
    }
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverse = block_count; reverse > 0; --reverse) {
      const std::size_t block_index = reverse - 1;
      const ir::ControlTerminator& terminator =
          function.blocks()[block_index].terminator;
      ValueSet next_out(value_count, false);
      if (terminator.opcode == ir::TerminatorOpcode::kJump) {
        add_edge_live(&next_out, terminator.true_edge);
      } else if (terminator.opcode == ir::TerminatorOpcode::kBranch) {
        add_edge_live(&next_out, terminator.true_edge);
        add_edge_live(&next_out, terminator.false_edge);
      }

      ValueSet next_in = uses[block_index];
      for (std::size_t value_index = 0; value_index < value_count;
           ++value_index) {
        if (next_out[value_index] &&
            !definitions[block_index][value_index]) {
          next_in[value_index] = true;
        }
      }
      if (next_out != live_out[block_index] ||
          next_in != live_in[block_index]) {
        live_out[block_index] = std::move(next_out);
        live_in[block_index] = std::move(next_in);
        changed = true;
      }
    }
  }

  std::vector<std::vector<ir::Value>> live_values(value_count);
  std::size_t total_live_values = 0;
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    ValueSet live(value_count, false);
    const ir::ControlTerminator& terminator = block.terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kJump) {
      add_edge_live(&live, terminator.true_edge);
    } else if (terminator.opcode == ir::TerminatorOpcode::kBranch) {
      add_edge_live(&live, terminator.true_edge);
      add_edge_live(&live, terminator.false_edge);
      live[terminator.value.id()] = true;
    } else if (terminator.opcode == ir::TerminatorOpcode::kReturn) {
      live[terminator.value.id()] = true;
    }

    for (std::size_t reverse = block.instructions.size(); reverse > 0;
         --reverse) {
      const ir::Value value = block.instructions[reverse - 1];
      const ir::ControlNode& node = function.nodes()[value.id()];
      live[value.id()] = false;
      visit_control_operands(function, node,
                             [&](ir::Value operand) { live[operand.id()] = true; });
      const StackMapRequirement* requirement =
          node.opcode == ir::ControlOpcode::kSafepoint ||
                  node.opcode == ir::ControlOpcode::kGuardWordNonzero ||
                  node.opcode == ir::ControlOpcode::kGuardFloatNonzero
              ? find_stack_map_requirement(
                    requirements, static_cast<std::size_t>(node.immediate))
              : nullptr;
      if (requirement != nullptr) {
        for (const ir::Value required : requirement->values) {
          live[required.id()] = true;
        }
      }
      if (node.opcode != ir::ControlOpcode::kSafepoint &&
          node.opcode != ir::ControlOpcode::kGuardWordNonzero &&
          node.opcode != ir::ControlOpcode::kGuardFloatNonzero) {
        continue;
      }
      std::vector<ir::Value>& site_values = live_values[value.id()];
      for (std::size_t value_index = 0; value_index < value_count;
           ++value_index) {
        if (live[value_index]) {
          if (site_values.size() ==
              runtime::ExecutionContext::kMaximumCapturedValues) {
            return {{StatusCode::kResourceExhausted,
                     "CFG stack map exceeds the capture capacity"},
                    {}};
          }
          if (total_live_values == kMaximumStackMapValues) {
            return {{StatusCode::kResourceExhausted,
                     "CFG stack maps exceed the metadata limit"},
                    {}};
          }
          site_values.emplace_back(static_cast<std::uint32_t>(value_index));
          ++total_live_values;
        }
      }
    }
  }
  return {Status::ok_status(), std::move(live_values)};
}

}  // namespace

RegisterAllocation allocate_linear_scan(const ir::Function& function,
                                        std::size_t register_count,
                                        std::size_t maximum_spill_slots,
                                        const StackMapRequirements&
                                            requirements) {
  try {
    return allocate_impl(function, register_count, maximum_spill_slots,
                         requirements);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate linear-scan state"},
            {}, 0, {}};
  }
}

ControlFlowRegisterAllocation allocate_control_flow_registers(
    const ir::ControlFlowFunction& function, std::size_t word_register_count,
    std::size_t float_register_count,
    const StackMapRequirements& requirements) {
  try {
    return allocate_control_flow_impl(function, word_register_count,
                                      float_register_count, requirements);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate control-flow register state"},
            {}, {}, {}, {}};
  }
}

ControlFlowEdgeMoves plan_control_flow_edge_moves(
    const ir::ControlFlowFunction& function, const ir::ControlEdge& edge,
    const ControlFlowRegisterAllocation& allocation,
    std::size_t current_block) {
  return plan_control_flow_edge_impl(function, edge, allocation,
                                     current_block);
}

StackMapLiveness plan_stack_map_liveness(
    const ir::Function& function, const RegisterAllocation& allocation) {
  try {
    return plan_straight_stack_map_liveness_impl(function, allocation);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate straight-line stack-map liveness"},
            {}};
  }
}

StackMapLiveness plan_stack_map_liveness(
    const ir::ControlFlowFunction& function,
    const StackMapRequirements& requirements) {
  try {
    return plan_control_stack_map_liveness_impl(function, requirements);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate CFG stack-map liveness"},
            {}};
  }
}

}  // namespace unijit::jit::detail
