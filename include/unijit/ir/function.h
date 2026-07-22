#ifndef UNIJIT_IR_FUNCTION_H
#define UNIJIT_IR_FUNCTION_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/ir/atomic.h"
#include "unijit/ir/fast_call.h"
#include "unijit/ir/memory.h"
#include "unijit/ir/object.h"
#include "unijit/ir/patch_cell.h"
#include "unijit/ir/vector.h"
#include "unijit/status.h"

namespace unijit::ir {

using RuntimeHelper = Word (*)(const Word* arguments, std::size_t count);

Word pack_float64(double value) noexcept;
double unpack_float64(Word bits) noexcept;
Word pack_runtime_helper(RuntimeHelper helper) noexcept;
RuntimeHelper unpack_runtime_helper(Word bits) noexcept;
// Lua/Python-style signed floor arithmetic. Division by zero is totalized to
// zero so malformed or speculative IR cannot raise a host hardware trap;
// language frontends that require an error insert a Word nonzero guard.
Word floor_divide_word(Word lhs, Word rhs) noexcept;
Word floor_modulo_word(Word lhs, Word rhs) noexcept;
Word byte_swap_word(Word value, MemoryWidth width) noexcept;

class Value final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr Value() noexcept = default;
  explicit constexpr Value(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(Value lhs, Value rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(Value lhs, Value rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

enum class Opcode : std::uint8_t {
  kParameter,
  kConstant,
  kAdd,
  kSubtract,
  kMultiply,
  kBitwiseAnd,
  kBitwiseOr,
  kBitwiseXor,
  kShiftLeft,
  kFloorDivide,
  kFloorModulo,
  kLessThan,
  kLessEqual,
  kEqual,
  kNotEqual,
  kNegate,
  kBitwiseNot,
  kByteSwap,
  kFloatAdd,
  kFloatSubtract,
  kFloatNegate,
  kFloatMultiply,
  kFloatDivide,
  kFloatLessThan,
  kFloatLessEqual,
  kFloatEqual,
  kFloatNotEqual,
  kGuardWordNonzero,
  kGuardFloatNonzero,
  kCall,
  kFastCall,
  kSafepoint,
  kLoadWord,
  kStoreWord,
  kLoadFloat,
  kStoreFloat,
  kLoadVector,
  kStoreVector,
  kAtomicLoad,
  kAtomicStore,
  kAtomicExchange,
  kAtomicCompareExchange,
  kAtomicFetchAdd,
  kAtomicFetchAnd,
  kAtomicFetchOr,
  kAtomicFetchXor,
  kAtomicFence,
  kLoadFrame,
  kStoreFrame,
  kLoadObject,
  kStoreObject,
  kLoadPatchCell,
  kVectorConstant,
  kVectorSplat,
  kVectorExtractLane,
  kVectorInsertLane,
  kVectorUnary,
  kVectorBinary,
  kVectorCompare,
  kVectorSelect,
  kVectorLaneSignMask,
  kVectorShuffle,
  kVectorWiden,
};

class FrameSlot final {
 public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr FrameSlot() noexcept = default;
  explicit constexpr FrameSlot(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(FrameSlot lhs, FrameSlot rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(FrameSlot lhs, FrameSlot rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  std::uint32_t id_{kInvalidId};
};

struct FrameSlotDescriptor final {
  ValueType type{ValueType::kWord};
  bool sensitive{false};
};

struct Node final {
  constexpr Node() noexcept = default;
  constexpr Node(
      Opcode opcode_value, Value lhs_value = {}, Value rhs_value = {},
      Word immediate_value = 0, ValueType type_value = ValueType::kWord,
      std::uint32_t argument_begin_value = 0,
      std::uint32_t argument_count_value = 0,
      std::uint32_t memory_access_value = MemoryAccessDescriptor::kInvalidIndex,
      std::uint32_t frame_slot_value = FrameSlot::kInvalidId,
      std::uint32_t trusted_object_value = TrustedObjectSlot::kInvalidId,
      Value auxiliary_value = {},
      std::uint32_t atomic_access_value =
          AtomicAccessDescriptor::kInvalidIndex) noexcept
      : opcode(opcode_value), lhs(lhs_value), rhs(rhs_value),
        immediate(immediate_value), type(type_value),
        argument_begin(argument_begin_value),
        argument_count(argument_count_value),
        memory_access(memory_access_value), frame_slot(frame_slot_value),
        trusted_object(trusted_object_value), auxiliary(auxiliary_value),
        atomic_access(atomic_access_value) {}

  Opcode opcode{Opcode::kConstant};
  Value lhs;
  Value rhs;
  Word immediate{0};
  ValueType type{ValueType::kWord};
  std::uint32_t argument_begin{0};
  std::uint32_t argument_count{0};
  std::uint32_t memory_access{MemoryAccessDescriptor::kInvalidIndex};
  std::uint32_t frame_slot{FrameSlot::kInvalidId};
  std::uint32_t trusted_object{TrustedObjectSlot::kInvalidId};
  Value auxiliary;
  std::uint32_t atomic_access{AtomicAccessDescriptor::kInvalidIndex};
};

struct AtomicCompareExchangeResult final {
  Value observed;
  Value success;

  bool valid() const noexcept { return observed.valid() && success.valid(); }
};

class Function final {
 public:
  Function() = default;

  std::size_t parameter_count() const noexcept { return parameter_count_; }
  ValueType parameter_type(std::size_t index) const noexcept {
    return index < parameter_types_.size() ? parameter_types_[index]
                                           : ValueType::kWord;
  }
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  const std::vector<Value>& call_arguments() const noexcept {
    return call_arguments_;
  }
  const std::vector<FastCallDescriptor>& fast_calls() const noexcept {
    return fast_calls_;
  }
  std::size_t memory_region_count() const noexcept {
    return memory_region_count_;
  }
  const std::vector<MemoryAccessDescriptor>& memory_accesses() const noexcept {
    return memory_accesses_;
  }
  const std::vector<AtomicAccessDescriptor> &atomic_accesses() const noexcept {
    return atomic_accesses_;
  }
  const std::vector<FrameSlotDescriptor>& frame_slots() const noexcept {
    return frame_slots_;
  }
  const std::vector<TrustedObjectDescriptor>& trusted_objects() const noexcept {
    return trusted_objects_;
  }
  const std::vector<PatchCellDescriptor>& patch_cells() const noexcept {
    return patch_cells_;
  }
  const std::vector<Vector128>& vector_constants() const noexcept {
    return vector_constants_;
  }
  const std::vector<VectorShuffle>& vector_shuffles() const noexcept {
    return vector_shuffles_;
  }
  const std::vector<Value>& vector_select_arguments() const noexcept {
    return vector_select_arguments_;
  }
  Value return_value() const noexcept { return return_value_; }
  ValueType return_type() const noexcept {
    return return_value_.valid() && return_value_.id() < nodes_.size()
               ? nodes_[return_value_.id()].type
               : ValueType::kWord;
  }
  ValueType value_type(Value value) const noexcept {
    return value.valid() && value.id() < nodes_.size()
               ? nodes_[value.id()].type
               : ValueType::kWord;
  }

 private:
  friend class FunctionBuilder;

  std::size_t parameter_count_{0};
  std::vector<ValueType> parameter_types_;
  std::vector<Node> nodes_;
  std::vector<Value> call_arguments_;
  std::vector<FastCallDescriptor> fast_calls_;
  std::size_t memory_region_count_{0};
  std::vector<MemoryAccessDescriptor> memory_accesses_;
  std::vector<AtomicAccessDescriptor> atomic_accesses_;
  std::vector<FrameSlotDescriptor> frame_slots_;
  std::vector<TrustedObjectDescriptor> trusted_objects_;
  std::vector<PatchCellDescriptor> patch_cells_;
  std::vector<Vector128> vector_constants_;
  std::vector<VectorShuffle> vector_shuffles_;
  std::vector<Value> vector_select_arguments_;
  Value return_value_;
};

class FunctionBuilder final {
 public:
  explicit FunctionBuilder(std::size_t parameter_count,
                           std::size_t memory_region_count = 0);
  explicit FunctionBuilder(std::vector<ValueType> parameter_types,
                           std::size_t memory_region_count = 0);

  std::size_t parameter_count() const noexcept {
    return function_.parameter_count_;
  }

  Value parameter(std::size_t index) const noexcept;
  Value constant(Word value);
  Value float64_constant(double value);
  Value float64_constant_bits(Word bits);
  Value add(Value lhs, Value rhs);
  Value subtract(Value lhs, Value rhs);
  Value multiply(Value lhs, Value rhs);
  Value bitwise_and(Value lhs, Value rhs);
  Value bitwise_or(Value lhs, Value rhs);
  Value bitwise_xor(Value lhs, Value rhs);
  // Shifts left for nonnegative amounts and logically right for negative
  // amounts. Magnitudes of 64 or more produce zero.
  Value shift_left(Value value, Value amount);
  Value floor_divide(Value lhs, Value rhs);
  Value floor_modulo(Value lhs, Value rhs);
  Value less_than(Value lhs, Value rhs);
  Value less_equal(Value lhs, Value rhs);
  Value equal(Value lhs, Value rhs);
  Value not_equal(Value lhs, Value rhs);
  Value negate(Value value);
  Value bitwise_not(Value value);
  // Reverses the low 16, 32, or 64 bits and zero-extends narrower results.
  Value byte_swap(Value value, MemoryWidth width);
  Value float64_add(Value lhs, Value rhs);
  Value float64_subtract(Value lhs, Value rhs);
  Value float64_negate(Value value);
  Value float64_multiply(Value lhs, Value rhs);
  Value float64_divide(Value lhs, Value rhs);
  Value float64_less_than(Value lhs, Value rhs);
  Value float64_less_equal(Value lhs, Value rhs);
  Value float64_equal(Value lhs, Value rhs);
  Value float64_not_equal(Value lhs, Value rhs);
  Value guard_word_nonzero(Value value, std::size_t site);
  Value guard_float64_nonzero(Value value, std::size_t site);
  Value call(RuntimeHelper helper, std::vector<Value> arguments,
             ValueType result_type = ValueType::kWord);
  FastCallSlot create_fast_call(std::vector<ValueType> parameter_types,
                                ValueType return_type = ValueType::kWord);
  Value fast_call(FastCallSlot target, std::vector<Value> arguments);
  Value safepoint(std::size_t site);
  Value load_word(Value byte_offset, MemoryAccessDescriptor access,
                  std::size_t site);
  Value store_word(Value byte_offset, Value value,
                   MemoryAccessDescriptor access, std::size_t site);
  // Float32 storage converts to/from the IR Float64 value type. Float64
  // storage preserves all 64 payload bits. Only k32 and k64 are valid.
  Value load_float(Value byte_offset, MemoryAccessDescriptor access,
                   std::size_t site);
  Value store_float(Value byte_offset, Value value,
                    MemoryAccessDescriptor access, std::size_t site);
  // Vector memory transfers exactly 128 bits. Lane zero remains at the lowest
  // address; byte order applies independently within each typed lane. Mask
  // vectors cannot be loaded from untrusted bytes.
  Value load_vector(Value byte_offset, ValueType type,
                    MemoryAccessDescriptor access, std::size_t site);
  Value store_vector(Value byte_offset, Value value,
                     MemoryAccessDescriptor access, std::size_t site);
  Value atomic_load(Value byte_offset, AtomicAccessDescriptor access,
                    std::size_t site);
  Value atomic_store(Value byte_offset, Value value,
                     AtomicAccessDescriptor access, std::size_t site);
  Value atomic_exchange(Value byte_offset, Value value,
                        AtomicAccessDescriptor access, std::size_t site);
  Value atomic_compare_exchange_observed(Value byte_offset, Value expected,
                                         Value desired,
                                         AtomicAccessDescriptor access,
                                         std::size_t site);
  AtomicCompareExchangeResult
  atomic_compare_exchange(Value byte_offset, Value expected, Value desired,
                          AtomicAccessDescriptor access, std::size_t site);
  Value atomic_fetch_add(Value byte_offset, Value value,
                         AtomicAccessDescriptor access, std::size_t site);
  Value atomic_fetch_and(Value byte_offset, Value value,
                         AtomicAccessDescriptor access, std::size_t site);
  Value atomic_fetch_or(Value byte_offset, Value value,
                        AtomicAccessDescriptor access, std::size_t site);
  Value atomic_fetch_xor(Value byte_offset, Value value,
                         AtomicAccessDescriptor access, std::size_t site);
  Value atomic_fence(AtomicMemoryOrder order);
  // Frame slots are fixed-size, zero-initialized for each invocation, and
  // remain live for the complete generated frame. Sensitive slots are cleared
  // on every native return path.
  FrameSlot create_frame_slot(ValueType type, bool sensitive = false);
  Value load_frame(FrameSlot slot);
  Value store_frame(FrameSlot slot, Value value);
  // Trusted objects are invocation-bound, non-addressable primitive layouts.
  // The delivered field floor is one naturally aligned Word or Float64 value.
  TrustedObjectSlot create_trusted_object(std::uint64_t layout_identity,
                                          std::size_t byte_size);
  Value load_object(TrustedObjectSlot object, std::size_t byte_offset,
                    ValueType type);
  Value store_object(TrustedObjectSlot object, std::size_t byte_offset,
                     Value value);
  // Patch cells are compiled-function-owned non-executable data. Generated
  // code can only acquire-load them; runtime publication is exposed by the
  // compiled-function and code-handle APIs.
  PatchCellSlot create_patch_cell(
      Word initial_value = 0,
      PatchCellKind kind = PatchCellKind::kValue);
  Value load_patch_cell(PatchCellSlot cell);
  // Vector lane zero is the lowest-addressed logical lane. Floating lane
  // insertion/extraction uses raw IEEE bits in Word so payloads survive.
  Value vector_constant(ValueType type, Vector128 bits);
  Value vector_zero(ValueType type);
  Value vector_splat(ValueType type, Value lane_bits);
  Value vector_extract_lane(Value vector, std::size_t lane,
                            bool sign_extend = false);
  Value vector_insert_lane(Value vector, std::size_t lane, Value lane_bits);
  Value vector_unary(VectorUnaryOperation operation, Value vector);
  Value vector_binary(VectorBinaryOperation operation, Value lhs, Value rhs);
  Value vector_compare(VectorComparison comparison, Value lhs, Value rhs);
  Value vector_select(Value mask, Value true_value, Value false_value);
  Value vector_lane_sign_mask(Value vector);
  Value vector_shuffle(Value vector, std::vector<std::uint8_t> lanes);
  Value vector_widen(Value vector, ValueType result_type,
                     VectorExtension extension, VectorHalf half);
  Status set_return(Value value);

  Function build() && noexcept;

 private:
  Value append_binary(Opcode opcode, Value lhs, Value rhs);
  Value append_atomic(Opcode opcode, Value byte_offset, Value value,
                      Value auxiliary, AtomicAccessDescriptor access,
                      std::size_t site);

  Function function_;
};

Status verify(const Function& function);

}  // namespace unijit::ir

#endif  // UNIJIT_IR_FUNCTION_H
