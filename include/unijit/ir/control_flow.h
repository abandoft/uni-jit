#ifndef UNIJIT_IR_CONTROL_FLOW_H
#define UNIJIT_IR_CONTROL_FLOW_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/status.h"

namespace unijit::ir {

class Block final {
public:
  static constexpr std::uint32_t kInvalidId =
      std::numeric_limits<std::uint32_t>::max();

  constexpr Block() noexcept = default;
  explicit constexpr Block(std::uint32_t id) noexcept : id_(id) {}

  constexpr bool valid() const noexcept { return id_ != kInvalidId; }
  constexpr std::uint32_t id() const noexcept { return id_; }

  friend constexpr bool operator==(Block lhs, Block rhs) noexcept {
    return lhs.id_ == rhs.id_;
  }

  friend constexpr bool operator!=(Block lhs, Block rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  std::uint32_t id_{kInvalidId};
};

enum class ControlOpcode : std::uint8_t {
  kParameter,
  kBlockParameter,
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
  kLessThan,
  kLessEqual,
  kEqual,
  kNotEqual,
  kCall,
  kGuardWordNonzero,
  kGuardFloatNonzero,
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

struct ControlNode final {
  constexpr ControlNode() noexcept = default;
  constexpr ControlNode(
      ControlOpcode opcode_value, Value lhs_value = {}, Value rhs_value = {},
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

  ControlOpcode opcode{ControlOpcode::kConstant};
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

struct ControlEdge final {
  Block target;
  std::vector<Value> arguments;
};

enum class TerminatorOpcode : std::uint8_t {
  kNone,
  kReturn,
  kJump,
  kBranch,
};

struct ControlTerminator final {
  TerminatorOpcode opcode{TerminatorOpcode::kNone};
  Value value;
  ControlEdge true_edge;
  ControlEdge false_edge;
};

struct BasicBlock final {
  std::vector<Value> parameters;
  std::vector<Value> instructions;
  ControlTerminator terminator;
};

class ControlFlowFunction final {
public:
  ControlFlowFunction() = default;

  std::size_t parameter_count() const noexcept { return parameter_count_; }
  ValueType parameter_type(std::size_t index) const noexcept {
    return index < parameter_types_.size() ? parameter_types_[index]
                                           : ValueType::kWord;
  }
  ValueType value_type(Value value) const noexcept {
    return value.valid() && value.id() < nodes_.size()
               ? nodes_[value.id()].type
               : ValueType::kWord;
  }
  ValueType return_type() const noexcept {
    for (const BasicBlock &block : blocks_) {
      if (block.terminator.opcode == TerminatorOpcode::kReturn) {
        return value_type(block.terminator.value);
      }
    }
    return ValueType::kWord;
  }
  Block entry_block() const noexcept { return entry_block_; }
  const std::vector<ControlNode> &nodes() const noexcept { return nodes_; }
  const std::vector<Value> &call_arguments() const noexcept {
    return call_arguments_;
  }
  std::size_t memory_region_count() const noexcept {
    return memory_region_count_;
  }
  const std::vector<MemoryAccessDescriptor> &memory_accesses() const noexcept {
    return memory_accesses_;
  }
  const std::vector<AtomicAccessDescriptor> &atomic_accesses() const noexcept {
    return atomic_accesses_;
  }
  const std::vector<FrameSlotDescriptor> &frame_slots() const noexcept {
    return frame_slots_;
  }
  const std::vector<TrustedObjectDescriptor> &trusted_objects() const noexcept {
    return trusted_objects_;
  }
  const std::vector<Vector128> &vector_constants() const noexcept {
    return vector_constants_;
  }
  const std::vector<VectorShuffle> &vector_shuffles() const noexcept {
    return vector_shuffles_;
  }
  const std::vector<Value> &vector_select_arguments() const noexcept {
    return vector_select_arguments_;
  }
  const std::vector<BasicBlock> &blocks() const noexcept { return blocks_; }

private:
  friend class ControlFlowBuilder;

  std::size_t parameter_count_{0};
  std::vector<ValueType> parameter_types_;
  Block entry_block_;
  std::vector<ControlNode> nodes_;
  std::vector<Value> call_arguments_;
  std::size_t memory_region_count_{0};
  std::vector<MemoryAccessDescriptor> memory_accesses_;
  std::vector<AtomicAccessDescriptor> atomic_accesses_;
  std::vector<FrameSlotDescriptor> frame_slots_;
  std::vector<TrustedObjectDescriptor> trusted_objects_;
  std::vector<Vector128> vector_constants_;
  std::vector<VectorShuffle> vector_shuffles_;
  std::vector<Value> vector_select_arguments_;
  std::vector<BasicBlock> blocks_;
};

class ControlFlowBuilder final {
public:
  explicit ControlFlowBuilder(std::size_t parameter_count,
                              std::size_t memory_region_count = 0);
  explicit ControlFlowBuilder(std::vector<ValueType> parameter_types,
                              std::size_t memory_region_count = 0);

  Block entry_block() const noexcept { return function_.entry_block_; }
  Block insertion_block() const noexcept { return insertion_block_; }
  Value parameter(std::size_t index) const noexcept;
  Value block_parameter(Block block, std::size_t index) const noexcept;

  Block create_block(std::size_t parameter_count);
  Block create_block(std::vector<ValueType> parameter_types);
  Status set_insertion_block(Block block);

  Value constant(Word value);
  Value float64_constant(double value);
  Value float64_constant_bits(Word bits);
  Value add(Value lhs, Value rhs);
  Value subtract(Value lhs, Value rhs);
  Value multiply(Value lhs, Value rhs);
  Value bitwise_and(Value lhs, Value rhs);
  Value bitwise_or(Value lhs, Value rhs);
  Value bitwise_xor(Value lhs, Value rhs);
  // Uses the same signed bidirectional, overshift-to-zero contract as the
  // straight-line builder.
  Value shift_left(Value value, Value amount);
  Value floor_divide(Value lhs, Value rhs);
  Value floor_modulo(Value lhs, Value rhs);
  Value negate(Value value);
  Value bitwise_not(Value value);
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
  Value less_than(Value lhs, Value rhs);
  Value less_equal(Value lhs, Value rhs);
  Value equal(Value lhs, Value rhs);
  Value not_equal(Value lhs, Value rhs);
  Value call(RuntimeHelper helper, std::vector<Value> arguments,
             ValueType result_type = ValueType::kWord);
  Value guard_word_nonzero(Value value, std::size_t site);
  Value guard_float64_nonzero(Value value, std::size_t site);
  Value safepoint(std::size_t site);
  Value load_word(Value byte_offset, MemoryAccessDescriptor access,
                  std::size_t site);
  Value store_word(Value byte_offset, Value value,
                   MemoryAccessDescriptor access, std::size_t site);
  Value load_float(Value byte_offset, MemoryAccessDescriptor access,
                   std::size_t site);
  Value store_float(Value byte_offset, Value value,
                   MemoryAccessDescriptor access, std::size_t site);
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
  FrameSlot create_frame_slot(ValueType type, bool sensitive = false);
  Value load_frame(FrameSlot slot);
  Value store_frame(FrameSlot slot, Value value);
  TrustedObjectSlot create_trusted_object(std::uint64_t layout_identity,
                                          std::size_t byte_size);
  Value load_object(TrustedObjectSlot object, std::size_t byte_offset,
                    ValueType type);
  Value store_object(TrustedObjectSlot object, std::size_t byte_offset,
                     Value value);
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
  Status jump(Block target, std::vector<Value> arguments);
  Status branch(Value condition, Block true_target,
                std::vector<Value> true_arguments, Block false_target,
                std::vector<Value> false_arguments);

  ControlFlowFunction build() && noexcept;

private:
  Value append_node(ControlNode node);
  Value append_unary(ControlOpcode opcode, Value value,
                     ValueType type = ValueType::kWord);
  Value append_binary(ControlOpcode opcode, Value lhs, Value rhs,
                      ValueType type = ValueType::kWord);
  Value append_atomic(ControlOpcode opcode, Value byte_offset, Value value,
                      Value auxiliary, AtomicAccessDescriptor access,
                      std::size_t site);
  Status validate_edge(Block target, const std::vector<Value> &arguments) const;
  Status set_terminator(ControlTerminator terminator);

  ControlFlowFunction function_;
  Block insertion_block_;
};

Status verify(const ControlFlowFunction &function);

class ControlFlowInterpreter final {
public:
  static EvaluationResult
  evaluate(const ControlFlowFunction &function, const Word *args,
           std::size_t arg_count,
           std::size_t maximum_block_executions = 1000000,
           runtime::ExecutionContext *context = nullptr);

  static EvaluationResult
  evaluate(const ControlFlowFunction &function, const std::vector<Word> &args,
           std::size_t maximum_block_executions = 1000000,
           runtime::ExecutionContext *context = nullptr) {
    return evaluate(function, args.data(), args.size(),
                    maximum_block_executions, context);
  }
};

} // namespace unijit::ir

#endif // UNIJIT_IR_CONTROL_FLOW_H
