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

  std::size_t expected_call_argument = 0;
  std::size_t expected_memory_access = 0;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const Node& node = nodes[index];
    if (!is_memory(node.opcode) &&
        node.memory_access != MemoryAccessDescriptor::kInvalidIndex) {
      return invalid_node(index,
                          "non-memory node has a memory descriptor");
    }
    if (index < function.parameter_count()) {
      if (node.opcode != Opcode::kParameter ||
          node.immediate != static_cast<Word>(index) ||
          node.type != function.parameter_type(index) ||
          node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "parameter nodes must lead the function");
      }
      continue;
    }

    if (node.opcode == Opcode::kParameter) {
      return invalid_node(index, "parameter node appears after another value");
    }
    if (node.opcode == Opcode::kConstant) {
      if (node.argument_begin != 0 || node.argument_count != 0) {
        return invalid_node(index, "constant node has call arguments");
      }
      continue;
    }
    if (node.opcode == Opcode::kCall) {
      if (unpack_runtime_helper(node.immediate) == nullptr) {
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
      }
      expected_call_argument += node.argument_count;
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

  if (!function.return_value().valid() ||
      function.return_value().id() >= nodes.size()) {
    return {StatusCode::kInvalidIr, "function has no valid return value"};
  }
  return Status::ok_status();
}

}  // namespace unijit::ir
