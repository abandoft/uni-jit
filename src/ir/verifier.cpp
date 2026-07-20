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
         opcode == Opcode::kMultiply;
}

}  // namespace

Status verify(const Function& function) {
  const auto& nodes = function.nodes();
  if (function.parameter_count() > nodes.size()) {
    return {StatusCode::kInvalidIr,
            "parameter count exceeds the number of SSA values"};
  }

  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const Node& node = nodes[index];
    if (index < function.parameter_count()) {
      if (node.opcode != Opcode::kParameter ||
          node.immediate != static_cast<Word>(index)) {
        return invalid_node(index, "parameter nodes must lead the function");
      }
      continue;
    }

    if (node.opcode == Opcode::kParameter) {
      return invalid_node(index, "parameter node appears after another value");
    }
    if (node.opcode == Opcode::kConstant) {
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
  }

  if (!function.return_value().valid() ||
      function.return_value().id() >= nodes.size()) {
    return {StatusCode::kInvalidIr, "function has no valid return value"};
  }
  return Status::ok_status();
}

}  // namespace unijit::ir
