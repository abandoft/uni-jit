#include "unijit/ir/interpreter.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace unijit::ir {
namespace {

std::uint64_t to_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "word size must be 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

Word evaluate_binary(Opcode opcode, Word lhs, Word rhs) noexcept {
  const std::uint64_t lhs_bits = to_bits(lhs);
  const std::uint64_t rhs_bits = to_bits(rhs);
  switch (opcode) {
    case Opcode::kAdd:
      return from_bits(lhs_bits + rhs_bits);
    case Opcode::kSubtract:
      return from_bits(lhs_bits - rhs_bits);
    case Opcode::kMultiply:
      return from_bits(lhs_bits * rhs_bits);
    case Opcode::kFloatAdd:
      return pack_float64(unpack_float64(lhs) + unpack_float64(rhs));
    case Opcode::kFloatSubtract:
      return pack_float64(unpack_float64(lhs) - unpack_float64(rhs));
    case Opcode::kFloatMultiply:
      return pack_float64(unpack_float64(lhs) * unpack_float64(rhs));
    case Opcode::kParameter:
    case Opcode::kConstant:
    case Opcode::kCall:
      return 0;
  }
  return 0;
}

}  // namespace

EvaluationResult Interpreter::evaluate(const Function& function,
                                       const Word* args,
                                       std::size_t arg_count) {
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

  try {
    std::vector<Word> values(function.nodes().size());
    std::vector<Word> helper_arguments;
    for (std::size_t index = 0; index < function.nodes().size(); ++index) {
      const Node& node = function.nodes()[index];
      switch (node.opcode) {
        case Opcode::kParameter:
          values[index] = args[static_cast<std::size_t>(node.immediate)];
          break;
        case Opcode::kConstant:
          values[index] = node.immediate;
          break;
        case Opcode::kCall: {
          helper_arguments.resize(node.argument_count);
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            const Value argument = function.call_arguments()[
                static_cast<std::size_t>(node.argument_begin) +
                argument_index];
            helper_arguments[argument_index] = values[argument.id()];
          }
          values[index] = unpack_runtime_helper(node.immediate)(
              helper_arguments.data(), helper_arguments.size());
          break;
        }
        case Opcode::kAdd:
        case Opcode::kSubtract:
        case Opcode::kMultiply:
        case Opcode::kFloatAdd:
        case Opcode::kFloatSubtract:
        case Opcode::kFloatMultiply:
          values[index] = evaluate_binary(node.opcode, values[node.lhs.id()],
                                          values[node.rhs.id()]);
          break;
      }
    }
    return {Status::ok_status(), values[function.return_value().id()]};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate interpreter value storage"},
            0};
  }
}

}  // namespace unijit::ir
