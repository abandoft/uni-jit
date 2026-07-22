#include "unijit/ir/interpreter.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <system_error>
#include <vector>

#include "ir/atomic_access.h"
#include "ir/memory_access.h"
#include "ir/object_access.h"

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
    case Opcode::kLessThan:
      return lhs < rhs ? 1 : 0;
    case Opcode::kLessEqual:
      return lhs <= rhs ? 1 : 0;
    case Opcode::kEqual:
      return lhs == rhs ? 1 : 0;
    case Opcode::kNotEqual:
      return lhs != rhs ? 1 : 0;
    case Opcode::kNegate:
      return from_bits(UINT64_C(0) - lhs_bits);
    case Opcode::kBitwiseNot:
      return from_bits(~lhs_bits);
    case Opcode::kByteSwap:
      return 0;
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
    case Opcode::kFastCall:
    case Opcode::kSafepoint:
    case Opcode::kLoadWord:
    case Opcode::kStoreWord:
    case Opcode::kLoadFloat:
    case Opcode::kStoreFloat:
    case Opcode::kLoadVector:
    case Opcode::kStoreVector:
    case Opcode::kAtomicLoad:
    case Opcode::kAtomicStore:
    case Opcode::kAtomicExchange:
    case Opcode::kAtomicCompareExchange:
    case Opcode::kAtomicFetchAdd:
    case Opcode::kAtomicFetchAnd:
    case Opcode::kAtomicFetchOr:
    case Opcode::kAtomicFetchXor:
    case Opcode::kAtomicFence:
    case Opcode::kLoadFrame:
    case Opcode::kStoreFrame:
    case Opcode::kLoadObject:
    case Opcode::kStoreObject:
    case Opcode::kLoadPatchCell:
    case Opcode::kVectorConstant:
    case Opcode::kVectorSplat:
    case Opcode::kVectorExtractLane:
    case Opcode::kVectorInsertLane:
    case Opcode::kVectorUnary:
    case Opcode::kVectorBinary:
    case Opcode::kVectorCompare:
    case Opcode::kVectorSelect:
    case Opcode::kVectorLaneSignMask:
    case Opcode::kVectorShuffle:
    case Opcode::kVectorWiden:
      return 0;
  }
  return 0;
}

detail::AtomicOperation atomic_operation(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::kAtomicLoad:
    return detail::AtomicOperation::kLoad;
  case Opcode::kAtomicStore:
    return detail::AtomicOperation::kStore;
  case Opcode::kAtomicExchange:
    return detail::AtomicOperation::kExchange;
  case Opcode::kAtomicCompareExchange:
    return detail::AtomicOperation::kCompareExchange;
  case Opcode::kAtomicFetchAdd:
    return detail::AtomicOperation::kFetchAdd;
  case Opcode::kAtomicFetchAnd:
    return detail::AtomicOperation::kFetchAnd;
  case Opcode::kAtomicFetchOr:
    return detail::AtomicOperation::kFetchOr;
  case Opcode::kAtomicFetchXor:
    return detail::AtomicOperation::kFetchXor;
  default:
    return detail::AtomicOperation::kLoad;
  }
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
    std::vector<bool> trusted_object_writable(
        function.trusted_objects().size(), false);
    for (const Node& node : function.nodes()) {
      if (node.opcode == Opcode::kStoreObject &&
          node.trusted_object < trusted_object_writable.size()) {
        trusted_object_writable[node.trusted_object] = true;
      }
    }
    const Status object_status = detail::validate_trusted_object_bindings(
        function.trusted_objects(), trusted_object_writable, context);
    if (!object_status.ok()) {
      return {object_status, 0};
    }
    std::vector<Word> values(function.nodes().size());
    std::vector<Vector128> vector_values(function.nodes().size());
    std::vector<Word> frame_values(function.frame_slots().size(), 0);
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
        case Opcode::kFastCall: {
          const std::size_t target = static_cast<std::size_t>(node.immediate);
          if (context == nullptr ||
              target >= context->fast_call_oracle_count() ||
              context->fast_call_oracles()[target] == nullptr) {
            return {{StatusCode::kInvalidArgument,
                     "fast call has no bound interpreter oracle", target},
                    0};
          }
          helper_arguments.resize(node.argument_count);
          for (std::size_t argument_index = 0;
               argument_index < node.argument_count; ++argument_index) {
            const Value argument = function.call_arguments()[
                static_cast<std::size_t>(node.argument_begin) +
                argument_index];
            helper_arguments[argument_index] = values[argument.id()];
          }
          values[index] = context->fast_call_oracles()[target](
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
        case Opcode::kLoadWord: {
          const detail::MemoryAccessResult result = detail::load_bounded_word(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], static_cast<std::size_t>(node.immediate),
              context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kStoreWord: {
          const detail::MemoryAccessResult result = detail::store_bounded_word(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], values[node.rhs.id()],
              static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kLoadFloat: {
          const detail::MemoryAccessResult result = detail::load_bounded_float(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], static_cast<std::size_t>(node.immediate),
              context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kStoreFloat: {
          const detail::MemoryAccessResult result = detail::store_bounded_float(
              function.memory_accesses()[node.memory_access],
              values[node.lhs.id()], values[node.rhs.id()],
              static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kLoadVector: {
          const detail::VectorMemoryAccessResult result =
              detail::load_bounded_vector(
                  function.memory_accesses()[node.memory_access], node.type,
                  values[node.lhs.id()],
                  static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          vector_values[index] = result.value;
          break;
        }
        case Opcode::kStoreVector: {
          const detail::VectorMemoryAccessResult result =
              detail::store_bounded_vector(
                  function.memory_accesses()[node.memory_access], node.type,
                  values[node.lhs.id()], vector_values[node.rhs.id()],
                  static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          vector_values[index] = result.value;
          break;
        }
        case Opcode::kAtomicLoad:
        case Opcode::kAtomicStore:
        case Opcode::kAtomicExchange:
        case Opcode::kAtomicCompareExchange:
        case Opcode::kAtomicFetchAdd:
        case Opcode::kAtomicFetchAnd:
        case Opcode::kAtomicFetchOr:
        case Opcode::kAtomicFetchXor: {
          const detail::AtomicAccessResult result =
              detail::access_bounded_atomic(
                  function.atomic_accesses()[node.atomic_access],
                  atomic_operation(node.opcode), values[node.lhs.id()],
                  node.rhs.valid() ? values[node.rhs.id()] : 0,
                  node.auxiliary.valid() ? values[node.auxiliary.id()] : 0,
                  static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kAtomicFence:
          detail::execute_atomic_fence(
              static_cast<AtomicMemoryOrder>(node.immediate));
          values[index] = 0;
          break;
        case Opcode::kLoadFrame:
          values[index] = frame_values[node.frame_slot];
          break;
        case Opcode::kStoreFrame:
          values[index] = values[node.lhs.id()];
          frame_values[node.frame_slot] = values[index];
          break;
        case Opcode::kLoadObject: {
          const detail::ObjectAccessResult result =
              detail::load_trusted_object(
                  TrustedObjectSlot{node.trusted_object},
                  static_cast<std::size_t>(node.immediate), context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kStoreObject: {
          const detail::ObjectAccessResult result =
              detail::store_trusted_object(
                  TrustedObjectSlot{node.trusted_object},
                  static_cast<std::size_t>(node.immediate),
                  values[node.lhs.id()], context);
          if (!result.ok()) {
            return {result.status, 0};
          }
          values[index] = result.value;
          break;
        }
        case Opcode::kLoadPatchCell:
          values[index] = function.patch_cells()[
              static_cast<std::size_t>(node.immediate)].initial_value;
          break;
        case Opcode::kVectorConstant:
          vector_values[index] = function.vector_constants()[
              static_cast<std::size_t>(node.immediate)];
          break;
        case Opcode::kVectorSplat:
          vector_values[index] =
              vector_splat_bits(node.type, values[node.lhs.id()]);
          break;
        case Opcode::kVectorExtractLane: {
          const ValueType source_type = function.value_type(node.lhs);
          values[index] = vector_extract_lane_bits(
              vector_values[node.lhs.id()], source_type,
              static_cast<std::size_t>(node.immediate & 0xff),
              (node.immediate & 0x100) != 0);
          break;
        }
        case Opcode::kVectorInsertLane:
          vector_values[index] = vector_insert_lane_bits(
              vector_values[node.lhs.id()], node.type,
              static_cast<std::size_t>(node.immediate),
              values[node.rhs.id()]);
          break;
        case Opcode::kVectorUnary:
          vector_values[index] = vector_unary(
              static_cast<VectorUnaryOperation>(node.immediate),
              vector_values[node.lhs.id()], node.type);
          break;
        case Opcode::kVectorBinary:
          vector_values[index] = vector_binary(
              static_cast<VectorBinaryOperation>(node.immediate),
              vector_values[node.lhs.id()], vector_values[node.rhs.id()],
              node.type);
          break;
        case Opcode::kVectorCompare: {
          const ValueType source_type = function.value_type(node.lhs);
          vector_values[index] = vector_compare(
              static_cast<VectorComparison>(node.immediate),
              vector_values[node.lhs.id()], vector_values[node.rhs.id()],
              source_type);
          break;
        }
        case Opcode::kVectorSelect:
          vector_values[index] = vector_select(
              vector_values[node.lhs.id()], vector_values[node.rhs.id()],
              vector_values[function.vector_select_arguments()[
                                static_cast<std::size_t>(node.immediate)]
                                .id()],
              function.value_type(node.lhs));
          break;
        case Opcode::kVectorLaneSignMask:
          values[index] = vector_lane_sign_mask(
              vector_values[node.lhs.id()], function.value_type(node.lhs));
          break;
        case Opcode::kVectorShuffle:
          vector_values[index] = vector_shuffle(
              vector_values[node.lhs.id()], node.type,
              function.vector_shuffles()[
                  static_cast<std::size_t>(node.immediate)]);
          break;
        case Opcode::kVectorWiden:
          vector_values[index] = vector_widen(
              vector_values[node.lhs.id()], function.value_type(node.lhs),
              node.type,
              static_cast<VectorExtension>(node.immediate & 0xff),
              static_cast<VectorHalf>((node.immediate >> 8U) & 0xff));
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
        case Opcode::kLessThan:
        case Opcode::kLessEqual:
        case Opcode::kEqual:
        case Opcode::kNotEqual:
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
        case Opcode::kByteSwap:
          values[index] = byte_swap_word(
              values[node.lhs.id()], static_cast<MemoryWidth>(node.immediate));
          break;
      }
    }
    return {Status::ok_status(), values[function.return_value().id()]};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate interpreter value storage"},
            0};
  } catch (const std::system_error &) {
    return {{StatusCode::kResourceExhausted,
             "unable to synchronize interpreter atomic access"},
            0};
  }
}

}  // namespace unijit::ir
