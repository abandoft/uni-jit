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

#include "ir/memory_access.h"
#include "ir/object_access.h"

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
  case ControlOpcode::kEqual:
  case ControlOpcode::kNotEqual:
    return true;
  case ControlOpcode::kParameter:
  case ControlOpcode::kBlockParameter:
  case ControlOpcode::kConstant:
  case ControlOpcode::kNegate:
  case ControlOpcode::kBitwiseNot:
  case ControlOpcode::kByteSwap:
  case ControlOpcode::kFloatNegate:
  case ControlOpcode::kCall:
  case ControlOpcode::kGuardWordNonzero:
  case ControlOpcode::kGuardFloatNonzero:
  case ControlOpcode::kSafepoint:
  case ControlOpcode::kLoadWord:
  case ControlOpcode::kStoreWord:
  case ControlOpcode::kLoadFloat:
  case ControlOpcode::kStoreFloat:
  case ControlOpcode::kLoadFrame:
  case ControlOpcode::kStoreFrame:
  case ControlOpcode::kLoadObject:
  case ControlOpcode::kStoreObject:
    return false;
  }
  return false;
}

bool is_unary(ControlOpcode opcode) {
  return opcode == ControlOpcode::kNegate ||
         opcode == ControlOpcode::kBitwiseNot ||
         opcode == ControlOpcode::kByteSwap ||
         opcode == ControlOpcode::kFloatNegate;
}

bool is_memory(ControlOpcode opcode) {
  return opcode == ControlOpcode::kLoadWord ||
         opcode == ControlOpcode::kStoreWord ||
         opcode == ControlOpcode::kLoadFloat ||
         opcode == ControlOpcode::kStoreFloat;
}

bool is_float_memory(ControlOpcode opcode) {
  return opcode == ControlOpcode::kLoadFloat ||
         opcode == ControlOpcode::kStoreFloat;
}

bool is_memory_load(ControlOpcode opcode) {
  return opcode == ControlOpcode::kLoadWord ||
         opcode == ControlOpcode::kLoadFloat;
}

bool is_memory_store(ControlOpcode opcode) {
  return opcode == ControlOpcode::kStoreWord ||
         opcode == ControlOpcode::kStoreFloat;
}

bool is_frame(ControlOpcode opcode) {
  return opcode == ControlOpcode::kLoadFrame ||
         opcode == ControlOpcode::kStoreFrame;
}

bool is_object(ControlOpcode opcode) {
  return opcode == ControlOpcode::kLoadObject ||
         opcode == ControlOpcode::kStoreObject;
}

bool valid_value_type(ValueType type) {
  return type == ValueType::kWord || type == ValueType::kFloat64;
}

bool valid_memory_width(MemoryWidth width) {
  return width == MemoryWidth::k8 || width == MemoryWidth::k16 ||
         width == MemoryWidth::k32 || width == MemoryWidth::k64;
}

bool valid_byte_order(MemoryByteOrder order) {
  return order == MemoryByteOrder::kNative ||
         order == MemoryByteOrder::kLittleEndian ||
         order == MemoryByteOrder::kBigEndian;
}

bool is_runtime_exit(ControlOpcode opcode) {
  return opcode == ControlOpcode::kGuardWordNonzero ||
         opcode == ControlOpcode::kGuardFloatNonzero ||
         opcode == ControlOpcode::kSafepoint || is_memory(opcode);
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
  if (function.memory_region_count() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return invalid_control_flow(
        0, "memory region count exceeds the CFG index range");
  }
  if (function.frame_slots().size() > FrameSlot::kInvalidId) {
    return invalid_control_flow(
        0, "frame slot count exceeds the CFG index range");
  }
  for (const FrameSlotDescriptor &slot : function.frame_slots()) {
    if (!valid_value_type(slot.type)) {
      return invalid_control_flow(0, "CFG frame slot has an invalid type");
    }
  }
  if (function.trusted_objects().size() > TrustedObjectSlot::kInvalidId) {
    return invalid_control_flow(
        0, "trusted object count exceeds the CFG index range");
  }
  for (const TrustedObjectDescriptor &object : function.trusted_objects()) {
    if (object.layout_identity == 0 || object.byte_size < sizeof(Word) ||
        object.byte_size > TrustedObjectDescriptor::kMaximumByteSize ||
        (object.byte_size % alignof(Word)) != 0) {
      return invalid_control_flow(
          0, "CFG trusted object descriptor is malformed");
    }
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
      if (!is_memory(node.opcode) &&
          node.memory_access != MemoryAccessDescriptor::kInvalidIndex) {
        return invalid_control_flow(
            value.id(), "non-memory CFG node has a memory descriptor");
      }
      if (!is_frame(node.opcode) && node.frame_slot != FrameSlot::kInvalidId) {
        return invalid_control_flow(
            value.id(), "non-frame CFG node has a frame slot");
      }
      if (!is_object(node.opcode) &&
          node.trusted_object != TrustedObjectSlot::kInvalidId) {
        return invalid_control_flow(
            value.id(), "non-object CFG node has a trusted object");
      }
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

  std::vector<bool> claimed_call_arguments(function.call_arguments().size(),
                                           false);
  std::vector<bool> claimed_memory_accesses(function.memory_accesses().size(),
                                            false);

  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    const BasicBlock &block = blocks[block_index];
    for (std::size_t instruction_index = block.parameters.size();
         instruction_index < block.instructions.size(); ++instruction_index) {
      const Value value = block.instructions[instruction_index];
      const ControlNode &node = nodes[value.id()];
      if (node.opcode == ControlOpcode::kConstant) {
        if (node.argument_begin != 0 || node.argument_count != 0) {
          return invalid_control_flow(value.id(),
                                      "constant has runtime-call arguments");
        }
        continue;
      }
      if (node.opcode == ControlOpcode::kCall) {
        const std::size_t begin = node.argument_begin;
        const std::size_t count = node.argument_count;
        if (node.lhs.valid() || node.rhs.valid() ||
            unpack_runtime_helper(node.immediate) == nullptr ||
            begin > function.call_arguments().size() ||
            count > function.call_arguments().size() - begin) {
          return invalid_control_flow(value.id(),
                                      "control-flow runtime call is malformed");
        }
        for (std::size_t argument_index = 0; argument_index < count;
             ++argument_index) {
          const std::size_t flat_index = begin + argument_index;
          const Value argument = function.call_arguments()[flat_index];
          if (claimed_call_arguments[flat_index] ||
              !available(argument, block_index, instruction_index)) {
            return invalid_control_flow(
                value.id(),
                "runtime call argument is duplicated or unavailable");
          }
          claimed_call_arguments[flat_index] = true;
        }
        continue;
      }
      if (is_memory(node.opcode)) {
        if (node.memory_access >= function.memory_accesses().size() ||
            claimed_memory_accesses[node.memory_access]) {
          return invalid_control_flow(
              value.id(), "CFG memory descriptor is invalid or duplicated");
        }
        claimed_memory_accesses[node.memory_access] = true;
        const MemoryAccessDescriptor& access =
            function.memory_accesses()[node.memory_access];
        const std::size_t width = memory_width_bytes(access.width);
        const bool float_memory = is_float_memory(node.opcode);
        const ValueType result_type =
            float_memory ? ValueType::kFloat64 : ValueType::kWord;
        if (access.region >= function.memory_region_count() ||
            !valid_memory_width(access.width) || access.alignment == 0 ||
            access.alignment > width ||
            (access.alignment & (access.alignment - 1U)) != 0 ||
            !valid_byte_order(access.byte_order) ||
            ((float_memory || is_memory_store(node.opcode)) &&
             access.sign_extend) ||
            (float_memory && access.width != MemoryWidth::k32 &&
             access.width != MemoryWidth::k64) ||
            node.immediate < 0 || node.type != result_type ||
            !available(node.lhs, block_index, instruction_index) ||
            function.value_type(node.lhs) != ValueType::kWord ||
            node.argument_begin != 0 || node.argument_count != 0) {
          return invalid_control_flow(
              value.id(), "bounded CFG memory operation is malformed");
        }
        if (is_memory_load(node.opcode) && node.rhs.valid()) {
          return invalid_control_flow(
              value.id(), "bounded CFG memory load has a stored value");
        }
        if (is_memory_store(node.opcode) &&
            (!available(node.rhs, block_index, instruction_index) ||
             function.value_type(node.rhs) != result_type)) {
          return invalid_control_flow(
              value.id(), "bounded CFG memory store value is malformed");
        }
        for (std::size_t previous = 0; previous < value.id(); ++previous) {
          if (is_runtime_exit(nodes[previous].opcode) &&
              nodes[previous].immediate == node.immediate) {
            return invalid_control_flow(
                value.id(), "control-flow runtime exit site is duplicated");
          }
        }
        continue;
      }
      if (is_frame(node.opcode)) {
        if (node.frame_slot >= function.frame_slots().size() ||
            node.immediate != 0 || node.rhs.valid() ||
            node.argument_begin != 0 || node.argument_count != 0) {
          return invalid_control_flow(value.id(),
                                      "CFG frame operation is malformed");
        }
        const ValueType slot_type =
            function.frame_slots()[node.frame_slot].type;
        if (node.type != slot_type) {
          return invalid_control_flow(value.id(),
                                      "CFG frame operation has an invalid type");
        }
        if (node.opcode == ControlOpcode::kLoadFrame) {
          if (node.lhs.valid()) {
            return invalid_control_flow(value.id(),
                                        "CFG frame load has a stored value");
          }
        } else if (!available(node.lhs, block_index, instruction_index) ||
                   function.value_type(node.lhs) != slot_type) {
          return invalid_control_flow(value.id(),
                                      "CFG frame store value is malformed");
        }
        continue;
      }
      if (is_object(node.opcode)) {
        if (node.trusted_object >= function.trusted_objects().size() ||
            node.immediate < 0 ||
            (static_cast<std::size_t>(node.immediate) % alignof(Word)) != 0 ||
            node.rhs.valid() || node.argument_begin != 0 ||
            node.argument_count != 0 || !valid_value_type(node.type)) {
          return invalid_control_flow(
              value.id(), "CFG trusted object operation is malformed");
        }
        const std::size_t offset =
            static_cast<std::size_t>(node.immediate);
        const std::size_t byte_size =
            function.trusted_objects()[node.trusted_object].byte_size;
        if (offset > byte_size || sizeof(Word) > byte_size - offset) {
          return invalid_control_flow(
              value.id(), "CFG trusted object field is outside its layout");
        }
        if (node.opcode == ControlOpcode::kLoadObject) {
          if (node.lhs.valid()) {
            return invalid_control_flow(
                value.id(), "CFG trusted object load has a stored value");
          }
        } else if (!available(node.lhs, block_index, instruction_index) ||
                   function.value_type(node.lhs) != node.type) {
          return invalid_control_flow(
              value.id(), "CFG trusted object store value is malformed");
        }
        continue;
      }
      if (node.opcode == ControlOpcode::kGuardWordNonzero ||
          node.opcode == ControlOpcode::kGuardFloatNonzero) {
        const bool is_float =
            node.opcode == ControlOpcode::kGuardFloatNonzero;
        if (node.immediate < 0 || node.rhs.valid() ||
            node.type != ValueType::kWord ||
            node.argument_begin != 0 || node.argument_count != 0 ||
            !available(node.lhs, block_index, instruction_index) ||
            function.value_type(node.lhs) !=
                (is_float ? ValueType::kFloat64 : ValueType::kWord)) {
          return invalid_control_flow(
              value.id(), "control-flow nonzero guard is malformed");
        }
        for (std::size_t previous = 0; previous < value.id(); ++previous) {
          if (is_runtime_exit(nodes[previous].opcode) &&
              nodes[previous].immediate == node.immediate) {
            return invalid_control_flow(
                value.id(), "control-flow runtime exit site is duplicated");
          }
        }
        continue;
      }
      if (node.opcode == ControlOpcode::kSafepoint) {
        if (node.immediate < 0 || node.lhs.valid() || node.rhs.valid() ||
            node.type != ValueType::kWord || node.argument_begin != 0 ||
            node.argument_count != 0) {
          return invalid_control_flow(value.id(),
                                      "control-flow safepoint is malformed");
        }
        for (std::size_t previous = 0; previous < value.id(); ++previous) {
          if (is_runtime_exit(nodes[previous].opcode) &&
              nodes[previous].immediate == node.immediate) {
            return invalid_control_flow(
                value.id(), "control-flow runtime exit site is duplicated");
          }
        }
        continue;
      }
      if (is_unary(node.opcode)) {
        const ValueType operand_type =
            node.opcode == ControlOpcode::kFloatNegate
                ? ValueType::kFloat64
                : ValueType::kWord;
        const bool valid_immediate =
            node.opcode == ControlOpcode::kByteSwap
                ? node.immediate == static_cast<Word>(MemoryWidth::k16) ||
                      node.immediate == static_cast<Word>(MemoryWidth::k32) ||
                      node.immediate == static_cast<Word>(MemoryWidth::k64)
                : node.immediate == 0;
        if (!available(node.lhs, block_index, instruction_index) ||
            node.rhs.valid() || node.type != operand_type ||
            function.value_type(node.lhs) != operand_type || !valid_immediate || node.argument_begin != 0 ||
            node.argument_count != 0) {
          return invalid_control_flow(
              value.id(), "control-flow unary operation is malformed");
        }
        continue;
      }
      if (!is_binary(node.opcode)) {
        return invalid_control_flow(value.id(), "unknown control-flow opcode");
      }
      if (!available(node.lhs, block_index, instruction_index) ||
          !available(node.rhs, block_index, instruction_index) ||
          node.argument_begin != 0 || node.argument_count != 0) {
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
                 node.opcode == ControlOpcode::kFloatLessEqual ||
                 node.opcode == ControlOpcode::kFloatEqual ||
                 node.opcode == ControlOpcode::kFloatNotEqual) {
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
  if (std::find(claimed_call_arguments.begin(),
                claimed_call_arguments.end(), false) !=
      claimed_call_arguments.end()) {
    return invalid_control_flow(
        0, "control-flow graph contains unreferenced call arguments");
  }
  if (std::find(claimed_memory_accesses.begin(),
                claimed_memory_accesses.end(), false) !=
      claimed_memory_accesses.end()) {
    return invalid_control_flow(
        0, "control-flow graph contains unreferenced memory descriptors");
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
    const std::uint64_t amount_bits = to_bits(rhs);
    if (rhs < 0) {
      const std::uint64_t magnitude = UINT64_C(0) - amount_bits;
      return magnitude >= 64U ? 0
                              : from_bits(to_bits(lhs) >> magnitude);
    }
    return amount_bits >= 64U ? 0
                              : from_bits(to_bits(lhs) << amount_bits);
  }
  if (opcode == ControlOpcode::kFloorDivide) {
    return floor_divide_word(lhs, rhs);
  }
  if (opcode == ControlOpcode::kFloorModulo) {
    return floor_modulo_word(lhs, rhs);
  }
  if (opcode == ControlOpcode::kNegate) {
    return from_bits(UINT64_C(0) - to_bits(lhs));
  }
  if (opcode == ControlOpcode::kBitwiseNot) {
    return from_bits(~to_bits(lhs));
  }
  if (opcode == ControlOpcode::kFloatAdd) {
    return pack_float64(unpack_float64(lhs) + unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatSubtract) {
    return pack_float64(unpack_float64(lhs) - unpack_float64(rhs));
  }
  if (opcode == ControlOpcode::kFloatNegate) {
    return from_bits(to_bits(lhs) ^ (UINT64_C(1) << 63U));
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
  if (opcode == ControlOpcode::kFloatEqual) {
    return unpack_float64(lhs) == unpack_float64(rhs) ? 1 : 0;
  }
  if (opcode == ControlOpcode::kFloatNotEqual) {
    return unpack_float64(lhs) != unpack_float64(rhs) ? 1 : 0;
  }
  if (opcode == ControlOpcode::kLessThan) {
    return lhs < rhs ? 1 : 0;
  }
  if (opcode == ControlOpcode::kLessEqual) {
    return lhs <= rhs ? 1 : 0;
  }
  if (opcode == ControlOpcode::kEqual) {
    return lhs == rhs ? 1 : 0;
  }
  return lhs != rhs ? 1 : 0;
}

} // namespace

ControlFlowBuilder::ControlFlowBuilder(std::size_t parameter_count,
                                       std::size_t memory_region_count)
    : ControlFlowBuilder(
          std::vector<ValueType>(parameter_count, ValueType::kWord),
          memory_region_count) {}

ControlFlowBuilder::ControlFlowBuilder(
    std::vector<ValueType> parameter_types,
    std::size_t memory_region_count) {
  function_.parameter_count_ = parameter_types.size();
  function_.parameter_types_ = std::move(parameter_types);
  function_.memory_region_count_ = memory_region_count;
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

Value ControlFlowBuilder::append_unary(ControlOpcode opcode, Value value,
                                       ValueType type) {
  return append_node(ControlNode{opcode, value, {}, 0, type});
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

Value ControlFlowBuilder::bitwise_and(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kBitwiseAnd, lhs, rhs);
}

Value ControlFlowBuilder::bitwise_or(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kBitwiseOr, lhs, rhs);
}

Value ControlFlowBuilder::bitwise_xor(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kBitwiseXor, lhs, rhs);
}

Value ControlFlowBuilder::shift_left(Value value, Value amount) {
  return append_binary(ControlOpcode::kShiftLeft, value, amount);
}

Value ControlFlowBuilder::floor_divide(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloorDivide, lhs, rhs);
}

Value ControlFlowBuilder::floor_modulo(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloorModulo, lhs, rhs);
}

Value ControlFlowBuilder::negate(Value value) {
  return append_unary(ControlOpcode::kNegate, value, ValueType::kWord);
}

Value ControlFlowBuilder::bitwise_not(Value value) {
  return append_unary(ControlOpcode::kBitwiseNot, value, ValueType::kWord);
}

Value ControlFlowBuilder::byte_swap(Value value, MemoryWidth width) {
  const Value result =
      append_unary(ControlOpcode::kByteSwap, value, ValueType::kWord);
  if (result.valid()) {
    function_.nodes_[result.id()].immediate = static_cast<Word>(width);
  }
  return result;
}

Value ControlFlowBuilder::float64_add(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatAdd, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_subtract(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatSubtract, lhs, rhs,
                       ValueType::kFloat64);
}

Value ControlFlowBuilder::float64_negate(Value value) {
  return append_unary(ControlOpcode::kFloatNegate, value,
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

Value ControlFlowBuilder::float64_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatEqual, lhs, rhs,
                       ValueType::kWord);
}

Value ControlFlowBuilder::float64_not_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kFloatNotEqual, lhs, rhs,
                       ValueType::kWord);
}

Value ControlFlowBuilder::less_than(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kLessThan, lhs, rhs);
}

Value ControlFlowBuilder::less_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kLessEqual, lhs, rhs);
}

Value ControlFlowBuilder::equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kEqual, lhs, rhs);
}

Value ControlFlowBuilder::not_equal(Value lhs, Value rhs) {
  return append_binary(ControlOpcode::kNotEqual, lhs, rhs);
}

Value ControlFlowBuilder::call(RuntimeHelper helper,
                               std::vector<Value> arguments,
                               ValueType result_type) {
  if (arguments.size() > std::numeric_limits<std::uint32_t>::max() ||
      function_.call_arguments_.size() >
          std::numeric_limits<std::uint32_t>::max() - arguments.size()) {
    return {};
  }
  const auto argument_begin =
      static_cast<std::uint32_t>(function_.call_arguments_.size());
  const auto argument_count = static_cast<std::uint32_t>(arguments.size());
  function_.call_arguments_.insert(function_.call_arguments_.end(),
                                   arguments.begin(), arguments.end());
  const Value result = append_node(
      ControlNode{ControlOpcode::kCall, {}, {}, pack_runtime_helper(helper),
                  result_type, argument_begin, argument_count});
  if (!result.valid()) {
    function_.call_arguments_.resize(argument_begin);
  }
  return result;
}

Value ControlFlowBuilder::guard_float64_nonzero(Value value,
                                                std::size_t site) {
  if (site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  return append_node(ControlNode{ControlOpcode::kGuardFloatNonzero, value, {},
                                 static_cast<Word>(site), ValueType::kWord});
}

Value ControlFlowBuilder::guard_word_nonzero(Value value, std::size_t site) {
  if (site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  return append_node(ControlNode{ControlOpcode::kGuardWordNonzero, value, {},
                                 static_cast<Word>(site), ValueType::kWord});
}

Value ControlFlowBuilder::safepoint(std::size_t site) {
  if (site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  return append_node(ControlNode{ControlOpcode::kSafepoint, {}, {},
                                 static_cast<Word>(site), ValueType::kWord});
}

Value ControlFlowBuilder::load_word(Value byte_offset,
                                    MemoryAccessDescriptor access,
                                    std::size_t site) {
  if (function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const Value result = append_node(
      ControlNode{ControlOpcode::kLoadWord, byte_offset, {},
                  static_cast<Word>(site), ValueType::kWord, 0, 0,
                  access_index});
  if (!result.valid()) {
    function_.memory_accesses_.pop_back();
  }
  return result;
}

Value ControlFlowBuilder::store_word(Value byte_offset, Value value,
                                     MemoryAccessDescriptor access,
                                     std::size_t site) {
  if (function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const Value result = append_node(
      ControlNode{ControlOpcode::kStoreWord, byte_offset, value,
                  static_cast<Word>(site), ValueType::kWord, 0, 0, access_index});
  if (!result.valid()) {
    function_.memory_accesses_.pop_back();
  }
  return result;
}

Value ControlFlowBuilder::load_float(Value byte_offset,
                                     MemoryAccessDescriptor access,
                                     std::size_t site) {
  if (function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const Value result = append_node(ControlNode{ControlOpcode::kLoadFloat,
                                               byte_offset,
                                               {},
                                               static_cast<Word>(site),
                                               ValueType::kFloat64,
                                               0,
                                               0,
                                               access_index});
  if (!result.valid()) {
    function_.memory_accesses_.pop_back();
  }
  return result;
}

Value ControlFlowBuilder::store_float(Value byte_offset, Value value,
                                      MemoryAccessDescriptor access,
                                      std::size_t site) {
  if (function_.memory_accesses_.size() >=
          MemoryAccessDescriptor::kInvalidIndex ||
      site > static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  access.sign_extend = false;
  const auto access_index =
      static_cast<std::uint32_t>(function_.memory_accesses_.size());
  function_.memory_accesses_.push_back(access);
  const Value result = append_node(ControlNode{
      ControlOpcode::kStoreFloat, byte_offset, value, static_cast<Word>(site),
      ValueType::kFloat64, 0, 0,
                  access_index});
  if (!result.valid()) {
    function_.memory_accesses_.pop_back();
  }
  return result;
}

FrameSlot ControlFlowBuilder::create_frame_slot(ValueType type,
                                                bool sensitive) {
  if (function_.frame_slots_.size() >= FrameSlot::kInvalidId) {
    return {};
  }
  const auto id = static_cast<std::uint32_t>(function_.frame_slots_.size());
  function_.frame_slots_.push_back(FrameSlotDescriptor{type, sensitive});
  return FrameSlot{id};
}

Value ControlFlowBuilder::load_frame(FrameSlot slot) {
  const ValueType type = slot.valid() && slot.id() < function_.frame_slots_.size()
                             ? function_.frame_slots_[slot.id()].type
                             : ValueType::kWord;
  ControlNode node;
  node.opcode = ControlOpcode::kLoadFrame;
  node.type = type;
  node.frame_slot = slot.id();
  return append_node(node);
}

Value ControlFlowBuilder::store_frame(FrameSlot slot, Value value) {
  const ValueType type = slot.valid() && slot.id() < function_.frame_slots_.size()
                             ? function_.frame_slots_[slot.id()].type
                             : ValueType::kWord;
  ControlNode node;
  node.opcode = ControlOpcode::kStoreFrame;
  node.lhs = value;
  node.type = type;
  node.frame_slot = slot.id();
  return append_node(node);
}

TrustedObjectSlot ControlFlowBuilder::create_trusted_object(
    std::uint64_t layout_identity, std::size_t byte_size) {
  if (function_.trusted_objects_.size() >= TrustedObjectSlot::kInvalidId) {
    return {};
  }
  const auto id =
      static_cast<std::uint32_t>(function_.trusted_objects_.size());
  function_.trusted_objects_.push_back(
      TrustedObjectDescriptor{layout_identity, byte_size});
  return TrustedObjectSlot{id};
}

Value ControlFlowBuilder::load_object(TrustedObjectSlot object,
                                      std::size_t byte_offset,
                                      ValueType type) {
  if (byte_offset >
      static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  ControlNode node;
  node.opcode = ControlOpcode::kLoadObject;
  node.immediate = static_cast<Word>(byte_offset);
  node.type = type;
  node.trusted_object = object.id();
  return append_node(node);
}

Value ControlFlowBuilder::store_object(TrustedObjectSlot object,
                                       std::size_t byte_offset, Value value) {
  if (byte_offset >
      static_cast<std::size_t>(std::numeric_limits<Word>::max())) {
    return {};
  }
  const ValueType type =
      value.valid() && value.id() < function_.nodes_.size()
          ? function_.nodes_[value.id()].type
          : ValueType::kWord;
  ControlNode node;
  node.opcode = ControlOpcode::kStoreObject;
  node.lhs = value;
  node.immediate = static_cast<Word>(byte_offset);
  node.type = type;
  node.trusted_object = object.id();
  return append_node(node);
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
    std::vector<bool> trusted_object_writable(
        function.trusted_objects().size(), false);
    for (const ControlNode &node : function.nodes()) {
      if (node.opcode == ControlOpcode::kStoreObject &&
          node.trusted_object < trusted_object_writable.size()) {
        trusted_object_writable[node.trusted_object] = true;
      }
    }
    const Status object_status = detail::validate_trusted_object_bindings(
        function.trusted_objects(), trusted_object_writable, context);
    if (!object_status.ok()) {
      return {object_status, 0};
    }
    std::vector<Word> values(function.nodes().size(), 0);
    std::vector<Word> frame_values(function.frame_slots().size(), 0);
    std::vector<Word> helper_arguments;
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
        } else if (node.opcode == ControlOpcode::kCall) {
          helper_arguments.resize(node.argument_count);
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            const Value argument = function.call_arguments()[
                static_cast<std::size_t>(node.argument_begin) +
                argument_index];
            helper_arguments[argument_index] = values[argument.id()];
          }
          values[value.id()] = unpack_runtime_helper(node.immediate)(
              helper_arguments.data(), helper_arguments.size());
        } else if (node.opcode == ControlOpcode::kGuardWordNonzero ||
                   node.opcode == ControlOpcode::kGuardFloatNonzero) {
          values[value.id()] = 0;
          const Word guarded = values[node.lhs.id()];
          const bool is_zero =
              node.opcode == ControlOpcode::kGuardWordNonzero
                  ? guarded == 0
                  : unpack_float64(guarded) == 0.0;
          if (is_zero) {
            const auto site = static_cast<std::size_t>(node.immediate);
            if (context != nullptr) {
              context->record_exit(runtime::ExitReason::kRuntime, site,
                                   guarded);
            }
            return {{StatusCode::kRuntimeExit,
                     "control-flow nonzero guard requested a runtime exit",
                     site},
                    0};
          }
        } else if (node.opcode == ControlOpcode::kSafepoint) {
          values[value.id()] = 0;
          if (context != nullptr) {
            context->record_safepoint_poll();
          }
          if (context != nullptr && context->exit_poll_requested()) {
            const auto site = static_cast<std::size_t>(node.immediate);
            context->record_exit(runtime::ExitReason::kSafepoint, site);
            return {{StatusCode::kExecutionInterrupted,
                     "execution interrupted at a control-flow safepoint",
                     site},
                    0};
          }
        } else if (node.opcode == ControlOpcode::kLoadWord) {
          const detail::MemoryAccessResult result = detail::load_bounded_word(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], static_cast<std::size_t>(node.immediate),
              context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kStoreWord) {
          const detail::MemoryAccessResult result = detail::store_bounded_word(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], values[node.rhs.id()],
              static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kLoadFloat) {
          const detail::MemoryAccessResult result = detail::load_bounded_float(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], static_cast<std::size_t>(node.immediate),
              context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kStoreFloat) {
          const detail::MemoryAccessResult result = detail::store_bounded_float(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], values[node.rhs.id()],
              static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kLoadFrame) {
          values[value.id()] = frame_values[node.frame_slot];
        } else if (node.opcode == ControlOpcode::kStoreFrame) {
          values[value.id()] = values[node.lhs.id()];
          frame_values[node.frame_slot] = values[value.id()];
        } else if (node.opcode == ControlOpcode::kLoadObject) {
          const detail::ObjectAccessResult result =
              detail::load_trusted_object(
                  TrustedObjectSlot{node.trusted_object},
                  static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kStoreObject) {
          const detail::ObjectAccessResult result =
              detail::store_trusted_object(
                  TrustedObjectSlot{node.trusted_object},
                  static_cast<std::size_t>(node.immediate),
                  values[node.lhs.id()], context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[value.id()] = result.value;
        } else if (node.opcode == ControlOpcode::kByteSwap) {
          values[value.id()] = byte_swap_word(
              values[node.lhs.id()], static_cast<MemoryWidth>(node.immediate));
        } else {
          values[value.id()] = evaluate_node(
              node.opcode, values[node.lhs.id()],
              node.rhs.valid() ? values[node.rhs.id()] : 0);
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
