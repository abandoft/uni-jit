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

Word shift_left(Word value, Word amount) noexcept {
  const std::uint64_t amount_bits = to_bits(amount);
  if (amount < 0) {
    const std::uint64_t magnitude = UINT64_C(0) - amount_bits;
    return magnitude >= 64U ? 0
                            : from_bits(to_bits(value) >> magnitude);
  }
  return amount_bits >= 64U ? 0
                            : from_bits(to_bits(value) << amount_bits);
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
    case Opcode::kBitwiseAnd:
      return from_bits(lhs_bits & rhs_bits);
    case Opcode::kBitwiseOr:
      return from_bits(lhs_bits | rhs_bits);
    case Opcode::kBitwiseXor:
      return from_bits(lhs_bits ^ rhs_bits);
    case Opcode::kShiftLeft:
      return shift_left(lhs, rhs);
    case Opcode::kFloorDivide:
      return floor_divide_word(lhs, rhs);
    case Opcode::kFloorModulo:
      return floor_modulo_word(lhs, rhs);
    case Opcode::kNegate:
      return from_bits(UINT64_C(0) - lhs_bits);
    case Opcode::kBitwiseNot:
      return from_bits(~lhs_bits);
    case Opcode::kFloatAdd:
      return pack_float64(unpack_float64(lhs) + unpack_float64(rhs));
    case Opcode::kFloatSubtract:
      return pack_float64(unpack_float64(lhs) - unpack_float64(rhs));
    case Opcode::kFloatNegate:
      return from_bits(lhs_bits ^ (UINT64_C(1) << 63U));
    case Opcode::kFloatMultiply:
      return pack_float64(unpack_float64(lhs) * unpack_float64(rhs));
    case Opcode::kFloatDivide:
      return pack_float64(unpack_float64(lhs) / unpack_float64(rhs));
    case Opcode::kFloatLessThan:
      return unpack_float64(lhs) < unpack_float64(rhs) ? 1 : 0;
    case Opcode::kFloatLessEqual:
      return unpack_float64(lhs) <= unpack_float64(rhs) ? 1 : 0;
    case Opcode::kFloatEqual:
      return unpack_float64(lhs) == unpack_float64(rhs) ? 1 : 0;
    case Opcode::kFloatNotEqual:
      return unpack_float64(lhs) != unpack_float64(rhs) ? 1 : 0;
    case Opcode::kGuardWordNonzero:
    case Opcode::kGuardFloatNonzero:
    case Opcode::kParameter:
    case Opcode::kConstant:
    case Opcode::kCall:
    case Opcode::kSafepoint:
      return 0;
  }
  return 0;
}

}  // namespace

EvaluationResult Interpreter::evaluate(const Function& function,
                                       const Word* args,
                                       std::size_t arg_count,
                                       runtime::ExecutionContext* context) {
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
  if (context != nullptr) {
    context->clear_exit();
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
        case Opcode::kGuardWordNonzero:
        case Opcode::kGuardFloatNonzero:
          values[index] = 0;
          if ((node.opcode == Opcode::kGuardWordNonzero &&
               values[node.lhs.id()] == 0) ||
              (node.opcode == Opcode::kGuardFloatNonzero &&
               unpack_float64(values[node.lhs.id()]) == 0.0)) {
            const auto site = static_cast<std::size_t>(node.immediate);
            if (context != nullptr) {
              context->record_exit(runtime::ExitReason::kRuntime, site,
                                   values[node.lhs.id()]);
            }
            return {{StatusCode::kRuntimeExit,
                     "nonzero guard requested a runtime exit", site},
                    0};
          }
          break;
        case Opcode::kSafepoint:
          values[index] = 0;
          if (context != nullptr) {
            context->record_safepoint_poll();
          }
          if (context != nullptr && context->exit_poll_requested()) {
            const auto site = static_cast<std::size_t>(node.immediate);
            context->record_exit(runtime::ExitReason::kSafepoint, site);
            return {{StatusCode::kExecutionInterrupted,
                     "execution interrupted at a safepoint", site},
                    0};
          }
          break;
        case Opcode::kAdd:
        case Opcode::kSubtract:
        case Opcode::kMultiply:
        case Opcode::kBitwiseAnd:
        case Opcode::kBitwiseOr:
        case Opcode::kBitwiseXor:
        case Opcode::kShiftLeft:
        case Opcode::kFloorDivide:
        case Opcode::kFloorModulo:
        case Opcode::kNegate:
        case Opcode::kBitwiseNot:
        case Opcode::kFloatAdd:
        case Opcode::kFloatSubtract:
        case Opcode::kFloatNegate:
        case Opcode::kFloatMultiply:
        case Opcode::kFloatDivide:
        case Opcode::kFloatLessThan:
        case Opcode::kFloatLessEqual:
        case Opcode::kFloatEqual:
        case Opcode::kFloatNotEqual:
          values[index] = evaluate_binary(
              node.opcode, values[node.lhs.id()],
              node.rhs.valid() ? values[node.rhs.id()] : 0);
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
