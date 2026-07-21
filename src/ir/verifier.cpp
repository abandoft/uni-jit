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
         opcode == Opcode::kFloorModulo ||
         opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide ||
         opcode == Opcode::kFloatLessThan ||
         opcode == Opcode::kFloatLessEqual || opcode == Opcode::kFloatEqual ||
         opcode == Opcode::kFloatNotEqual;
}

bool is_unary(Opcode opcode) {
  return opcode == Opcode::kNegate || opcode == Opcode::kBitwiseNot ||
         opcode == Opcode::kFloatNegate;
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

}  // namespace

Status verify(const Function& function) {
  const auto& nodes = function.nodes();
  if (function.parameter_count() > nodes.size()) {
    return {StatusCode::kInvalidIr,
            "parameter count exceeds the number of SSA values"};
  }

  std::size_t expected_call_argument = 0;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const Node& node = nodes[index];
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
        if ((nodes[previous].opcode == Opcode::kGuardWordNonzero ||
             nodes[previous].opcode == Opcode::kGuardFloatNonzero ||
             nodes[previous].opcode == Opcode::kSafepoint) &&
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
        if ((nodes[previous].opcode == Opcode::kGuardWordNonzero ||
             nodes[previous].opcode == Opcode::kGuardFloatNonzero ||
             nodes[previous].opcode == Opcode::kSafepoint) &&
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
      if (!node.lhs.valid() || node.lhs.id() >= index || node.rhs.valid() ||
          node.type != operand_type ||
          nodes[node.lhs.id()].type != operand_type ||
          node.immediate != 0 || node.argument_begin != 0 ||
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

  if (!function.return_value().valid() ||
      function.return_value().id() >= nodes.size()) {
    return {StatusCode::kInvalidIr, "function has no valid return value"};
  }
  return Status::ok_status();
}

}  // namespace unijit::ir
