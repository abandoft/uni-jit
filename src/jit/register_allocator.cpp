#include "jit/register_allocator.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace unijit::jit::detail {
namespace {

void note_use(std::vector<std::size_t>* last_use, ir::Value value,
              std::size_t use_index) {
  auto& entry = (*last_use)[value.id()];
  entry = std::max(entry, use_index);
}

RegisterAllocation allocate_impl(const ir::Function& function,
                                 std::size_t register_count,
                                 std::size_t maximum_spill_slots) {
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
        node.opcode == ir::Opcode::kFloatAdd ||
        node.opcode == ir::Opcode::kFloatSubtract ||
        node.opcode == ir::Opcode::kFloatMultiply) {
      note_use(&last_use, node.lhs, index);
      note_use(&last_use, node.rhs, index);
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
    const ir::ControlFlowFunction& function, std::size_t register_count) {
  if (register_count == 0) {
    return {{StatusCode::kInvalidArgument,
             "control-flow allocation requires an allocatable register"},
            {}, {}, {}};
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
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          note_local_use(node.lhs);
          note_local_use(node.rhs);
          break;
        case ir::ControlOpcode::kParameter:
        case ir::ControlOpcode::kBlockParameter:
        case ir::ControlOpcode::kConstant:
          break;
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

    std::vector<std::size_t> free_registers;
    free_registers.reserve(register_count);
    for (std::size_t index = register_count; index > 0; --index) {
      free_registers.push_back(index - 1);
    }
    std::vector<std::size_t> active;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
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
      const ir::Value value = block.instructions[index];
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
        case ir::ControlOpcode::kLessThan:
        case ir::ControlOpcode::kLessEqual:
          note_nonlocal_use(block_index, node.lhs);
          note_nonlocal_use(block_index, node.rhs);
          break;
        case ir::ControlOpcode::kParameter:
        case ir::ControlOpcode::kBlockParameter:
        case ir::ControlOpcode::kConstant:
          break;
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
          std::move(owners), std::move(requires_stack)};
}

ControlFlowEdgeMoves plan_control_flow_edge_impl(
    const ir::ControlFlowFunction& function, const ir::ControlEdge& edge,
    const ControlFlowRegisterAllocation& allocation,
    std::size_t current_block) {
  const ir::BasicBlock& target = function.blocks()[edge.target.id()];
  std::vector<ControlFlowRegisterMove> pending;
  pending.reserve(edge.arguments.size());
  for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
    const ir::Value parameter = target.parameters[index];
    const std::size_t destination = allocation.register_indices[parameter.id()];
    if (destination == ValueLocation::kNone) {
      return {false, {}};
    }

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
    pending.push_back({source_kind, source, destination});
  }

  ControlFlowEdgeMoves result{true, {}};
  result.moves.reserve(pending.size() + 1);
  while (!pending.empty()) {
    auto ready = pending.end();
    for (auto candidate = pending.begin(); candidate != pending.end();
         ++candidate) {
      bool destination_is_source = false;
      for (const ControlFlowRegisterMove& move : pending) {
        if (move.source_kind == ControlFlowMoveSource::kRegister &&
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
    result.moves.push_back({ControlFlowMoveSource::kRegister, saved_register,
                            ValueLocation::kNone});
    for (ControlFlowRegisterMove& move : pending) {
      if (move.source_kind == ControlFlowMoveSource::kRegister &&
          move.source_index == saved_register) {
        move.source_kind = ControlFlowMoveSource::kTemporary;
        move.source_index = 0;
      }
    }
  }
  return result;
}

}  // namespace

RegisterAllocation allocate_linear_scan(const ir::Function& function,
                                        std::size_t register_count,
                                        std::size_t maximum_spill_slots) {
  try {
    return allocate_impl(function, register_count, maximum_spill_slots);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate linear-scan state"},
            {}, 0, {}};
  }
}

ControlFlowRegisterAllocation allocate_control_flow_registers(
    const ir::ControlFlowFunction& function, std::size_t register_count) {
  try {
    return allocate_control_flow_impl(function, register_count);
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate control-flow register state"},
            {}, {}, {}};
  }
}

ControlFlowEdgeMoves plan_control_flow_edge_moves(
    const ir::ControlFlowFunction& function, const ir::ControlEdge& edge,
    const ControlFlowRegisterAllocation& allocation,
    std::size_t current_block) {
  return plan_control_flow_edge_impl(function, edge, allocation,
                                     current_block);
}

}  // namespace unijit::jit::detail
