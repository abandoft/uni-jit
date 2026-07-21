#include "unijit/ir/function.h"

#include <cstddef>
#include <string>

namespace unijit::ir {
namespace {

Status invalid_node(std::size_t index, const char* message) {
  return {StatusCode::kInvalidIr, message, index};
}

bool is_binary(Opcode opcode) {
  return opcode == Opcode::kAdd || opcode == Opcode::kSubtract ||
         opcode == Opcode::kMultiply || opcode == Opcode::kBitwiseAnd ||
         opcode == Opcode::kBitwiseOr || opcode == Opcode::kBitwiseXor ||
         opcode == Opcode::kShiftLeft || opcode == Opcode::kFloorDivide ||
         opcode == Opcode::kFloorModulo || opcode == Opcode::kLessThan ||
         opcode == Opcode::kLessEqual || opcode == Opcode::kEqual ||
         opcode == Opcode::kNotEqual ||
         opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide ||
         opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_unary(Opcode opcode) {
  return opcode == Opcode::kNegate || opcode == Opcode::kBitwiseNot ||
         opcode == Opcode::kByteSwap || opcode == Opcode::kFloatNegate;
}

bool has_float_operands(Opcode opcode) {
  return opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide ||
         opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_float_comparison(Opcode opcode) {
  return opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_memory(Opcode opcode) {
  return opcode == Opcode::kLoadWord || opcode == Opcode::kStoreWord ||
         opcode == Opcode::kLoadFloat || opcode == Opcode::kStoreFloat;
}

bool is_float_memory(Opcode opcode) {
  return opcode == Opcode::kLoadFloat || opcode == Opcode::kStoreFloat;
}

bool is_memory_load(Opcode opcode) {
  return opcode == Opcode::kLoadWord || opcode == Opcode::kLoadFloat;
}

bool is_memory_store(Opcode opcode) {
  return opcode == Opcode::kStoreWord || opcode == Opcode::kStoreFloat;
}

bool is_frame(Opcode opcode) {
  return opcode == Opcode::kLoadFrame || opcode == Opcode::kStoreFrame;
}

bool is_object(Opcode opcode) {
  return opcode == Opcode::kLoadObject || opcode == Opcode::kStoreObject;
}

bool valid_vector_unary(Word immediate) {
  return immediate == static_cast<Word>(VectorUnaryOperation::kBitwiseNot);
}

bool valid_vector_binary(Word immediate, ValueType type) {
  if (immediate < static_cast<Word>(VectorBinaryOperation::kAdd) ||
      immediate > static_cast<Word>(VectorBinaryOperation::kDivide)) {
    return false;
  }
  const auto operation = static_cast<VectorBinaryOperation>(immediate);
  if (operation == VectorBinaryOperation::kBitwiseAnd ||
      operation == VectorBinaryOperation::kBitwiseOr ||
      operation == VectorBinaryOperation::kBitwiseXor) {
    return is_vector_value_type(type);
  }
  if (operation == VectorBinaryOperation::kDivide) {
    return is_float_vector_type(type);
  }
  return is_integer_vector_type(type) || is_float_vector_type(type);
}

bool valid_vector_comparison(Word immediate, ValueType type) {
  if (immediate < static_cast<Word>(VectorComparison::kEqual) ||
      immediate > static_cast<Word>(VectorComparison::kOrderedFloatLessEqual)) {
    return false;
  }
  const auto comparison = static_cast<VectorComparison>(immediate);
  if (is_float_vector_type(type)) {
    return comparison == VectorComparison::kOrderedFloatEqual ||
           comparison == VectorComparison::kOrderedFloatLessThan ||
           comparison == VectorComparison::kOrderedFloatLessEqual;
  }
  return is_integer_vector_type(type) &&
         comparison != VectorComparison::kOrderedFloatEqual &&
         comparison != VectorComparison::kOrderedFloatLessThan &&
         comparison != VectorComparison::kOrderedFloatLessEqual;
}

bool valid_vector_widen(ValueType source, ValueType result) {
  return is_integer_vector_type(source) && is_integer_vector_type(result) &&
         vector_lane_bits(result) == vector_lane_bits(source) * 2U &&
         vector_lane_count(result) * 2U == vector_lane_count(source);
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

bool is_runtime_exit(Opcode opcode) {
  return opcode == Opcode::kGuardWordNonzero ||
         opcode == Opcode::kGuardFloatNonzero ||
         opcode == Opcode::kSafepoint || is_memory(opcode);
}

}  // namespace

Status verify(const Function& function) {
  const auto& nodes = function.nodes();
  if (function.parameter_count() > nodes.size()) {
    return {StatusCode::kInvalidIr,
            "parameter count exceeds the number of SSA values"};
  }
  if (function.memory_region_count() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return {StatusCode::kInvalidIr,
            "memory region count exceeds the IR index range"};
  }
  if (function.frame_slots().size() > FrameSlot::kInvalidId) {
    return {StatusCode::kInvalidIr,
            "frame slot count exceeds the IR index range"};
  }
  for (const FrameSlotDescriptor& slot : function.frame_slots()) {
    if (!is_scalar_value_type(slot.type)) {
      return {StatusCode::kInvalidIr, "frame slot has an invalid value type"};
    }
  }
  if (function.trusted_objects().size() > TrustedObjectSlot::kInvalidId) {
    return {StatusCode::kInvalidIr,
            "trusted object count exceeds the IR index range"};
  }
  for (const TrustedObjectDescriptor& object : function.trusted_objects()) {
    if (object.layout_identity == 0 || object.byte_size < sizeof(Word) ||
        object.byte_size > TrustedObjectDescriptor::kMaximumByteSize ||
        (object.byte_size % alignof(Word)) != 0) {
      return {StatusCode::kInvalidIr,
              "trusted object descriptor is malformed"};
    }
  }

  std::size_t expected_call_argument = 0;
  std::size_t expected_memory_access = 0;
  std::size_t expected_vector_constant = 0;
  std::size_t expected_vector_shuffle = 0;
  std::size_t expected_vector_select = 0;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const Node& node = nodes[index];
    if (!is_memory(node.opcode) &&
        node.memory_access != MemoryAccessDescriptor::kInvalidIndex) {
      return invalid_node(index,
                          "non-memory node has a memory descriptor");
    }
    if (!is_frame(node.opcode) && node.frame_slot != FrameSlot::kInvalidId) {
      return invalid_node(index, "non-frame node has a frame slot");
    }
    if (!is_object(node.opcode) &&
        node.trusted_object != TrustedObjectSlot::kInvalidId) {
      return invalid_node(index, "non-object node has a trusted object");
    }
    if (index < function.parameter_count()) {
      if (node.opcode != Opcode::kParameter ||
          node.immediate != static_cast<Word>(index) ||
          node.type != function.parameter_type(index) ||
          !is_scalar_value_type(node.type) ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "parameter nodes must lead the function");
      }
      continue;
    }

    if (node.opcode == Opcode::kParameter) {
      return invalid_node(index, "parameter node appears after another value");
    }
    if (node.opcode == Opcode::kConstant) {
      if (!is_scalar_value_type(node.type) || node.lhs.valid() ||
          node.rhs.valid() || node.argument_begin != 0 ||
          node.argument_count != 0) {
        return invalid_node(index, "constant node has call arguments");
      }
      continue;
    }
    if (node.opcode == Opcode::kCall) {
      if (unpack_runtime_helper(node.immediate) == nullptr ||
          !is_scalar_value_type(node.type) || node.lhs.valid() ||
          node.rhs.valid()) {
        return invalid_node(index, "runtime call has no helper target");
      }
      if (node.argument_begin != expected_call_argument ||
          static_cast<std::size_t>(node.argument_count) >
              function.call_arguments().size() - expected_call_argument) {
        return invalid_node(index, "runtime call argument range is invalid");
      }
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument =
            function.call_arguments()[expected_call_argument + argument_index];
        if (!argument.valid() || argument.id() >= index) {
          return invalid_node(
              index, "runtime call arguments must be earlier SSA values");
        }
        if (!is_scalar_value_type(nodes[argument.id()].type)) {
          return invalid_node(index,
                              "runtime call argument must be scalar");
        }
      }
      expected_call_argument += node.argument_count;
      continue;
    }
    if (node.opcode == Opcode::kVectorConstant) {
      if (!is_vector_value_type(node.type) || node.immediate < 0 ||
          static_cast<std::size_t>(node.immediate) !=
              expected_vector_constant ||
          expected_vector_constant >= function.vector_constants().size() ||
          node.lhs.valid() || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0 ||
          (is_mask_vector_type(node.type) &&
           !vector_mask_is_canonical(
               function.vector_constants()[expected_vector_constant],
               node.type))) {
        return invalid_node(index, "vector constant is malformed");
      }
      ++expected_vector_constant;
      continue;
    }
    if (node.opcode == Opcode::kVectorSplat) {
      if ((!is_integer_vector_type(node.type) &&
           !is_float_vector_type(node.type)) ||
          node.immediate != 0 || !node.lhs.valid() ||
          node.lhs.id() >= index ||
          nodes[node.lhs.id()].type != ValueType::kWord || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector splat is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorExtractLane) {
      const std::size_t lane = static_cast<std::size_t>(node.immediate & 0xff);
      const bool sign_extend = (node.immediate & 0x100) != 0;
      if (node.immediate < 0 || (node.immediate & ~Word{0x1ff}) != 0 ||
          !node.lhs.valid() || node.lhs.id() >= index ||
          !is_vector_value_type(nodes[node.lhs.id()].type) ||
          lane >= vector_lane_count(nodes[node.lhs.id()].type) ||
          (sign_extend &&
           !is_integer_vector_type(nodes[node.lhs.id()].type)) ||
          node.type != ValueType::kWord || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector lane extraction is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorInsertLane) {
      if (node.immediate < 0 || !node.lhs.valid() ||
          node.lhs.id() >= index || !node.rhs.valid() ||
          node.rhs.id() >= index ||
          (!is_integer_vector_type(node.type) &&
           !is_float_vector_type(node.type)) ||
          nodes[node.lhs.id()].type != node.type ||
          nodes[node.rhs.id()].type != ValueType::kWord ||
          static_cast<std::size_t>(node.immediate) >=
              vector_lane_count(node.type) ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector lane insertion is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorUnary) {
      if (!valid_vector_unary(node.immediate) ||
          !is_vector_value_type(node.type) || !node.lhs.valid() ||
          node.lhs.id() >= index || nodes[node.lhs.id()].type != node.type ||
          node.rhs.valid() || node.argument_begin != 0 ||
          node.argument_count != 0) {
        return invalid_node(index, "vector unary operation is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorBinary) {
      if (!valid_vector_binary(node.immediate, node.type) ||
          !node.lhs.valid() || node.lhs.id() >= index ||
          !node.rhs.valid() || node.rhs.id() >= index ||
          nodes[node.lhs.id()].type != node.type ||
          nodes[node.rhs.id()].type != node.type ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector binary operation is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorCompare) {
      if (!node.lhs.valid() || node.lhs.id() >= index ||
          !node.rhs.valid() || node.rhs.id() >= index ||
          nodes[node.lhs.id()].type != nodes[node.rhs.id()].type ||
          !valid_vector_comparison(node.immediate,
                                   nodes[node.lhs.id()].type) ||
          node.type != vector_mask_type(nodes[node.lhs.id()].type) ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector comparison is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorSelect) {
      if (node.immediate < 0 ||
          static_cast<std::size_t>(node.immediate) !=
              expected_vector_select ||
          expected_vector_select >=
              function.vector_select_arguments().size() ||
          !node.lhs.valid() ||
          node.lhs.id() >= index || !node.rhs.valid() ||
          node.rhs.id() >= index ||
          !function.vector_select_arguments()[expected_vector_select].valid() ||
          function.vector_select_arguments()[expected_vector_select].id() >=
              index ||
          !is_mask_vector_type(nodes[node.lhs.id()].type) ||
          !is_vector_value_type(node.type) ||
          nodes[node.rhs.id()].type != node.type ||
          nodes[function.vector_select_arguments()[expected_vector_select].id()]
                  .type != node.type ||
          !vector_shapes_match(nodes[node.lhs.id()].type, node.type) ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector selection is malformed");
      }
      ++expected_vector_select;
      continue;
    }
    if (node.opcode == Opcode::kVectorLaneSignMask) {
      if (node.immediate != 0 || !node.lhs.valid() ||
          node.lhs.id() >= index ||
          !is_vector_value_type(nodes[node.lhs.id()].type) ||
          node.type != ValueType::kWord || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector lane-sign extraction is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kVectorShuffle) {
      if (node.immediate < 0 ||
          static_cast<std::size_t>(node.immediate) !=
              expected_vector_shuffle ||
          expected_vector_shuffle >= function.vector_shuffles().size() ||
          !node.lhs.valid() || node.lhs.id() >= index ||
          !is_vector_value_type(node.type) ||
          nodes[node.lhs.id()].type != node.type || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "vector shuffle is malformed");
      }
      const VectorShuffle& shuffle =
          function.vector_shuffles()[expected_vector_shuffle];
      if (shuffle.lane_count != vector_lane_count(node.type)) {
        return invalid_node(index, "vector shuffle lane count is invalid");
      }
      for (std::size_t lane = 0; lane < shuffle.lane_count; ++lane) {
        if (shuffle.lanes[lane] >= shuffle.lane_count) {
          return invalid_node(index, "vector shuffle lane is out of range");
        }
      }
      ++expected_vector_shuffle;
      continue;
    }
    if (node.opcode == Opcode::kVectorWiden) {
      if (node.immediate < 0 || (node.immediate & ~Word{0x101}) != 0 ||
          !node.lhs.valid() || node.lhs.id() >= index ||
          !valid_vector_widen(nodes[node.lhs.id()].type, node.type) ||
          node.rhs.valid() || node.argument_begin != 0 ||
          node.argument_count != 0) {
        return invalid_node(index, "vector widening is malformed");
      }
      continue;
    }
    if (is_memory(node.opcode)) {
      if (node.memory_access != expected_memory_access ||
          expected_memory_access >= function.memory_accesses().size()) {
        return invalid_node(index,
                            "memory descriptor range is inconsistent");
      }
      const MemoryAccessDescriptor& access =
          function.memory_accesses()[expected_memory_access++];
      const std::size_t width = memory_width_bytes(access.width);
      const bool float_memory = is_float_memory(node.opcode);
      const ValueType result_type =
          float_memory ? ValueType::kFloat64 : ValueType::kWord;
      if (access.region >= function.memory_region_count() ||
          !valid_memory_width(access.width) || access.alignment == 0 ||
          access.alignment > width ||
          (access.alignment & (access.alignment - 1U)) != 0 ||
          !valid_byte_order(access.byte_order) ||
          ((float_memory || is_memory_store(node.opcode)) && access.sign_extend) ||
          (float_memory && access.width != MemoryWidth::k32 &&
           access.width != MemoryWidth::k64) ||
          node.immediate < 0 || node.type != result_type ||
          !node.lhs.valid() || node.lhs.id() >= index ||
          nodes[node.lhs.id()].type != ValueType::kWord ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "bounded memory operation is malformed");
      }
      if (is_memory_load(node.opcode) && node.rhs.valid()) {
        return invalid_node(index, "bounded memory load has a stored value");
      }
      if (is_memory_store(node.opcode) &&
          (!node.rhs.valid() || node.rhs.id() >= index ||
           nodes[node.rhs.id()].type != result_type)) {
        return invalid_node(index,
                            "bounded memory store value is malformed");
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (is_runtime_exit(nodes[previous].opcode) &&
            nodes[previous].immediate == node.immediate) {
          return invalid_node(index, "runtime exit site is duplicated");
        }
      }
      continue;
    }
    if (is_frame(node.opcode)) {
      if (node.frame_slot >= function.frame_slots().size() ||
          node.immediate != 0 || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "frame operation is malformed");
      }
      const ValueType slot_type = function.frame_slots()[node.frame_slot].type;
      if (node.type != slot_type) {
        return invalid_node(index, "frame operation has an invalid type");
      }
      if (node.opcode == Opcode::kLoadFrame) {
        if (node.lhs.valid()) {
          return invalid_node(index, "frame load has a stored value");
        }
      } else if (!node.lhs.valid() || node.lhs.id() >= index ||
                 nodes[node.lhs.id()].type != slot_type) {
        return invalid_node(index, "frame store value is malformed");
      }
      continue;
    }
    if (is_object(node.opcode)) {
      if (node.trusted_object >= function.trusted_objects().size() ||
          node.immediate < 0 ||
          (static_cast<std::size_t>(node.immediate) % alignof(Word)) != 0 ||
          node.rhs.valid() || node.argument_begin != 0 ||
          node.argument_count != 0 || !is_scalar_value_type(node.type)) {
        return invalid_node(index, "trusted object operation is malformed");
      }
      const std::size_t offset = static_cast<std::size_t>(node.immediate);
      const std::size_t byte_size =
          function.trusted_objects()[node.trusted_object].byte_size;
      if (offset > byte_size || sizeof(Word) > byte_size - offset) {
        return invalid_node(index,
                            "trusted object field is outside its layout");
      }
      if (node.opcode == Opcode::kLoadObject) {
        if (node.lhs.valid()) {
          return invalid_node(index,
                              "trusted object load has a stored value");
        }
      } else if (!node.lhs.valid() || node.lhs.id() >= index ||
                 nodes[node.lhs.id()].type != node.type) {
        return invalid_node(index,
                            "trusted object store value is malformed");
      }
      continue;
    }
    if (node.opcode == Opcode::kGuardWordNonzero ||
        node.opcode == Opcode::kGuardFloatNonzero) {
      const bool is_float = node.opcode == Opcode::kGuardFloatNonzero;
      if (node.immediate < 0 || node.type != ValueType::kWord ||
          !node.lhs.valid() || node.lhs.id() >= index || node.rhs.valid() ||
          node.argument_begin != 0 || node.argument_count != 0 ||
          nodes[node.lhs.id()].type !=
              (is_float ? ValueType::kFloat64 : ValueType::kWord)) {
        return invalid_node(index, "nonzero guard is malformed");
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (is_runtime_exit(nodes[previous].opcode) &&
            nodes[previous].immediate == node.immediate) {
          return invalid_node(index, "runtime exit site is duplicated");
        }
      }
      continue;
    }
    if (node.opcode == Opcode::kSafepoint) {
      if (node.immediate < 0 || node.type != ValueType::kWord ||
          node.lhs.valid() || node.rhs.valid() || node.argument_begin != 0 ||
          node.argument_count != 0) {
        return invalid_node(index, "safepoint node is malformed");
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (is_runtime_exit(nodes[previous].opcode) &&
            nodes[previous].immediate == node.immediate) {
          return invalid_node(index, "runtime exit site is duplicated");
        }
      }
      continue;
    }
    if (is_unary(node.opcode)) {
      const ValueType operand_type = node.opcode == Opcode::kFloatNegate
                                         ? ValueType::kFloat64
                                         : ValueType::kWord;
      const bool valid_immediate =
          node.opcode == Opcode::kByteSwap
              ? node.immediate == static_cast<Word>(MemoryWidth::k16) ||
                    node.immediate == static_cast<Word>(MemoryWidth::k32) ||
                    node.immediate == static_cast<Word>(MemoryWidth::k64)
              : node.immediate == 0;
      if (!node.lhs.valid() || node.lhs.id() >= index || node.rhs.valid() ||
          node.type != operand_type ||
          nodes[node.lhs.id()].type != operand_type || !valid_immediate || node.argument_begin != 0 ||
          node.argument_count != 0) {
        return invalid_node(index, "unary operation is malformed");
      }
      continue;
    }
    if (!is_binary(node.opcode)) {
      return invalid_node(index, "unknown SSA opcode");
    }
    if (!node.lhs.valid() || !node.rhs.valid() || node.lhs.id() >= index ||
        node.rhs.id() >= index) {
      return invalid_node(index,
                          "binary operands must be earlier SSA definitions");
    }
    if (node.argument_begin != 0 || node.argument_count != 0) {
      return invalid_node(index, "binary node has call arguments");
    }
    const ValueType operand_type = has_float_operands(node.opcode)
                                       ? ValueType::kFloat64
                                       : ValueType::kWord;
    const ValueType result_type = is_float_comparison(node.opcode)
                                      ? ValueType::kWord
                                      : operand_type;
    if (node.type != result_type ||
        nodes[node.lhs.id()].type != operand_type ||
        nodes[node.rhs.id()].type != operand_type) {
      return invalid_node(index,
                          "binary operands or result have incompatible types");
    }
  }

  if (expected_call_argument != function.call_arguments().size()) {
    return {StatusCode::kInvalidIr,
            "function contains unreferenced runtime call arguments"};
  }
  if (expected_memory_access != function.memory_accesses().size()) {
    return {StatusCode::kInvalidIr,
            "function contains unreferenced memory descriptors"};
  }
  if (expected_vector_constant != function.vector_constants().size()) {
    return {StatusCode::kInvalidIr,
            "function contains unreferenced vector constants"};
  }
  if (expected_vector_shuffle != function.vector_shuffles().size()) {
    return {StatusCode::kInvalidIr,
            "function contains unreferenced vector shuffles"};
  }
  if (expected_vector_select != function.vector_select_arguments().size()) {
    return {StatusCode::kInvalidIr,
            "function contains unreferenced vector select operands"};
  }

  if (!function.return_value().valid() ||
      function.return_value().id() >= nodes.size()) {
    return {StatusCode::kInvalidIr, "function has no valid return value"};
  }
  if (!is_scalar_value_type(function.return_type())) {
    return {StatusCode::kInvalidIr,
            "function return type is not supported by the scalar ABI"};
  }
  return Status::ok_status();
}

}  // namespace unijit::ir
