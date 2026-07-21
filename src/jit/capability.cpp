#include "unijit/jit/capability.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace unijit::jit {
namespace {

enum class NormalizedVectorOpcode : std::uint8_t {
  kNone = 0,
  kMemory,
  kConstant,
  kSplat,
  kExtractLane,
  kInsertLane,
  kUnary,
  kBinary,
  kCompare,
  kSelect,
  kLaneSignMask,
  kShuffle,
  kWiden,
};

int strategy_rank(LoweringStrategy strategy) noexcept {
  return static_cast<int>(strategy);
}

void merge_strategy(LoweringStrategy strategy,
                    LoweringStrategy *destination) noexcept {
  if (strategy_rank(strategy) > strategy_rank(*destination)) {
    *destination = strategy;
  }
}

NormalizedVectorOpcode normalize(ir::Opcode opcode) noexcept {
  switch (opcode) {
  case ir::Opcode::kLoadVector:
  case ir::Opcode::kStoreVector:
    return NormalizedVectorOpcode::kMemory;
  case ir::Opcode::kVectorConstant:
    return NormalizedVectorOpcode::kConstant;
  case ir::Opcode::kVectorSplat:
    return NormalizedVectorOpcode::kSplat;
  case ir::Opcode::kVectorExtractLane:
    return NormalizedVectorOpcode::kExtractLane;
  case ir::Opcode::kVectorInsertLane:
    return NormalizedVectorOpcode::kInsertLane;
  case ir::Opcode::kVectorUnary:
    return NormalizedVectorOpcode::kUnary;
  case ir::Opcode::kVectorBinary:
    return NormalizedVectorOpcode::kBinary;
  case ir::Opcode::kVectorCompare:
    return NormalizedVectorOpcode::kCompare;
  case ir::Opcode::kVectorSelect:
    return NormalizedVectorOpcode::kSelect;
  case ir::Opcode::kVectorLaneSignMask:
    return NormalizedVectorOpcode::kLaneSignMask;
  case ir::Opcode::kVectorShuffle:
    return NormalizedVectorOpcode::kShuffle;
  case ir::Opcode::kVectorWiden:
    return NormalizedVectorOpcode::kWiden;
  default:
    return NormalizedVectorOpcode::kNone;
  }
}

NormalizedVectorOpcode normalize(ir::ControlOpcode opcode) noexcept {
  switch (opcode) {
  case ir::ControlOpcode::kLoadVector:
  case ir::ControlOpcode::kStoreVector:
    return NormalizedVectorOpcode::kMemory;
  case ir::ControlOpcode::kVectorConstant:
    return NormalizedVectorOpcode::kConstant;
  case ir::ControlOpcode::kVectorSplat:
    return NormalizedVectorOpcode::kSplat;
  case ir::ControlOpcode::kVectorExtractLane:
    return NormalizedVectorOpcode::kExtractLane;
  case ir::ControlOpcode::kVectorInsertLane:
    return NormalizedVectorOpcode::kInsertLane;
  case ir::ControlOpcode::kVectorUnary:
    return NormalizedVectorOpcode::kUnary;
  case ir::ControlOpcode::kVectorBinary:
    return NormalizedVectorOpcode::kBinary;
  case ir::ControlOpcode::kVectorCompare:
    return NormalizedVectorOpcode::kCompare;
  case ir::ControlOpcode::kVectorSelect:
    return NormalizedVectorOpcode::kSelect;
  case ir::ControlOpcode::kVectorLaneSignMask:
    return NormalizedVectorOpcode::kLaneSignMask;
  case ir::ControlOpcode::kVectorShuffle:
    return NormalizedVectorOpcode::kShuffle;
  case ir::ControlOpcode::kVectorWiden:
    return NormalizedVectorOpcode::kWiden;
  default:
    return NormalizedVectorOpcode::kNone;
  }
}

bool is_call(ir::Opcode opcode) noexcept { return opcode == ir::Opcode::kCall; }

bool is_call(ir::ControlOpcode opcode) noexcept {
  return opcode == ir::ControlOpcode::kCall;
}

bool is_atomic_access(ir::Opcode opcode) noexcept {
  return opcode == ir::Opcode::kAtomicLoad ||
         opcode == ir::Opcode::kAtomicStore ||
         opcode == ir::Opcode::kAtomicExchange ||
         opcode == ir::Opcode::kAtomicCompareExchange ||
         opcode == ir::Opcode::kAtomicFetchAdd ||
         opcode == ir::Opcode::kAtomicFetchAnd ||
         opcode == ir::Opcode::kAtomicFetchOr ||
         opcode == ir::Opcode::kAtomicFetchXor;
}

bool is_atomic(ir::Opcode opcode) noexcept {
  return is_atomic_access(opcode) || opcode == ir::Opcode::kAtomicFence;
}

bool is_atomic_access(ir::ControlOpcode opcode) noexcept {
  return opcode == ir::ControlOpcode::kAtomicLoad ||
         opcode == ir::ControlOpcode::kAtomicStore ||
         opcode == ir::ControlOpcode::kAtomicExchange ||
         opcode == ir::ControlOpcode::kAtomicCompareExchange ||
         opcode == ir::ControlOpcode::kAtomicFetchAdd ||
         opcode == ir::ControlOpcode::kAtomicFetchAnd ||
         opcode == ir::ControlOpcode::kAtomicFetchOr ||
         opcode == ir::ControlOpcode::kAtomicFetchXor;
}

bool is_atomic(ir::ControlOpcode opcode) noexcept {
  return is_atomic_access(opcode) || opcode == ir::ControlOpcode::kAtomicFence;
}

bool requires_context(ir::Opcode opcode) noexcept {
  return opcode == ir::Opcode::kGuardWordNonzero ||
         opcode == ir::Opcode::kGuardFloatNonzero ||
         opcode == ir::Opcode::kSafepoint || opcode == ir::Opcode::kLoadWord ||
         opcode == ir::Opcode::kStoreWord || opcode == ir::Opcode::kLoadFloat ||
         opcode == ir::Opcode::kStoreFloat ||
         opcode == ir::Opcode::kLoadVector ||
         opcode == ir::Opcode::kStoreVector || is_atomic_access(opcode) ||
         opcode == ir::Opcode::kLoadObject ||
         opcode == ir::Opcode::kStoreObject;
}

bool requires_context(ir::ControlOpcode opcode) noexcept {
  return opcode == ir::ControlOpcode::kGuardWordNonzero ||
         opcode == ir::ControlOpcode::kGuardFloatNonzero ||
         opcode == ir::ControlOpcode::kSafepoint ||
         opcode == ir::ControlOpcode::kLoadWord ||
         opcode == ir::ControlOpcode::kStoreWord ||
         opcode == ir::ControlOpcode::kLoadFloat ||
         opcode == ir::ControlOpcode::kStoreFloat ||
         opcode == ir::ControlOpcode::kLoadVector ||
         opcode == ir::ControlOpcode::kStoreVector ||
         is_atomic_access(opcode) || opcode == ir::ControlOpcode::kLoadObject ||
         opcode == ir::ControlOpcode::kStoreObject;
}

bool is_word_multiply(ir::Opcode opcode, ir::ValueType type) noexcept {
  return opcode == ir::Opcode::kMultiply && type == ir::ValueType::kWord;
}

bool is_word_multiply(ir::ControlOpcode opcode, ir::ValueType type) noexcept {
  return opcode == ir::ControlOpcode::kMultiply && type == ir::ValueType::kWord;
}

bool uses_fp64(ir::Opcode opcode, ir::ValueType type) noexcept {
  if (type == ir::ValueType::kFloat64) {
    return true;
  }
  return opcode == ir::Opcode::kFloatAdd ||
         opcode == ir::Opcode::kFloatSubtract ||
         opcode == ir::Opcode::kFloatNegate ||
         opcode == ir::Opcode::kFloatMultiply ||
         opcode == ir::Opcode::kFloatDivide ||
         opcode == ir::Opcode::kFloatLessThan ||
         opcode == ir::Opcode::kFloatLessEqual ||
         opcode == ir::Opcode::kFloatEqual ||
         opcode == ir::Opcode::kFloatNotEqual ||
         opcode == ir::Opcode::kGuardFloatNonzero;
}

bool uses_fp64(ir::ControlOpcode opcode, ir::ValueType type) noexcept {
  if (type == ir::ValueType::kFloat64) {
    return true;
  }
  return opcode == ir::ControlOpcode::kFloatAdd ||
         opcode == ir::ControlOpcode::kFloatSubtract ||
         opcode == ir::ControlOpcode::kFloatNegate ||
         opcode == ir::ControlOpcode::kFloatMultiply ||
         opcode == ir::ControlOpcode::kFloatDivide ||
         opcode == ir::ControlOpcode::kFloatLessThan ||
         opcode == ir::ControlOpcode::kFloatLessEqual ||
         opcode == ir::ControlOpcode::kFloatEqual ||
         opcode == ir::ControlOpcode::kFloatNotEqual ||
         opcode == ir::ControlOpcode::kGuardFloatNonzero;
}

std::uint64_t scalar_features(TargetArchitecture architecture, bool fp64,
                              bool word_multiply) noexcept {
  std::uint64_t result = 0;
  if (fp64) {
    result |= target_feature_bit(TargetFeature::kFp64);
    if (architecture == TargetArchitecture::kRiscV64) {
      result |= target_feature_bit(TargetFeature::kRiscVFloat64);
    }
  }
  if (word_multiply && architecture == TargetArchitecture::kRiscV64) {
    result |= target_feature_bit(TargetFeature::kRiscVIntegerMultiply);
  }
  return result;
}

VectorOperationClass operation_class(NormalizedVectorOpcode opcode,
                                     ir::ValueType input_type,
                                     ir::Word immediate) noexcept {
  switch (opcode) {
  case NormalizedVectorOpcode::kMemory:
    return VectorOperationClass::kMemory;
  case NormalizedVectorOpcode::kConstant:
  case NormalizedVectorOpcode::kSplat:
    return VectorOperationClass::kConstruction;
  case NormalizedVectorOpcode::kExtractLane:
  case NormalizedVectorOpcode::kInsertLane:
    return VectorOperationClass::kLaneMovement;
  case NormalizedVectorOpcode::kUnary:
    return VectorOperationClass::kBitwiseLogic;
  case NormalizedVectorOpcode::kBinary: {
    const auto operation = static_cast<ir::VectorBinaryOperation>(immediate);
    if (operation == ir::VectorBinaryOperation::kBitwiseAnd ||
        operation == ir::VectorBinaryOperation::kBitwiseOr ||
        operation == ir::VectorBinaryOperation::kBitwiseXor) {
      return VectorOperationClass::kBitwiseLogic;
    }
    return ir::is_float_vector_type(input_type)
               ? VectorOperationClass::kFloatingArithmetic
               : VectorOperationClass::kIntegerArithmetic;
  }
  case NormalizedVectorOpcode::kCompare:
    return VectorOperationClass::kComparison;
  case NormalizedVectorOpcode::kSelect:
    return VectorOperationClass::kSelection;
  case NormalizedVectorOpcode::kLaneSignMask:
    return VectorOperationClass::kLaneSignMask;
  case NormalizedVectorOpcode::kShuffle:
    return VectorOperationClass::kShuffle;
  case NormalizedVectorOpcode::kWiden:
    return VectorOperationClass::kWidening;
  case NormalizedVectorOpcode::kNone:
    break;
  }
  return VectorOperationClass::kCount;
}

std::uint64_t vector_features(TargetArchitecture architecture,
                              ir::ValueType input_type,
                              NormalizedVectorOpcode opcode,
                              ir::Word immediate) noexcept {
  const bool floating = ir::is_float_vector_type(input_type);
  if (architecture == TargetArchitecture::kAArch64) {
    return target_feature_bit(TargetFeature::kNeon) |
           (floating ? target_feature_bit(TargetFeature::kFp64) : 0);
  }
  if (architecture == TargetArchitecture::kX86_64) {
    return target_feature_bit(TargetFeature::kSse2) |
           (floating ? target_feature_bit(TargetFeature::kFp64) : 0);
  }
  if (architecture == TargetArchitecture::kRiscV64) {
    std::uint64_t result = 0;
    if (floating) {
      result |= target_feature_bit(TargetFeature::kFp64) |
                target_feature_bit(TargetFeature::kRiscVFloat64);
    }
    if (opcode == NormalizedVectorOpcode::kBinary &&
        static_cast<ir::VectorBinaryOperation>(immediate) ==
            ir::VectorBinaryOperation::kMultiply &&
        !floating) {
      result |= target_feature_bit(TargetFeature::kRiscVIntegerMultiply);
    }
    return result;
  }
  return 0;
}

struct VectorDecision final {
  LoweringStrategy strategy{LoweringStrategy::kUnsupported};
  std::uint32_t resource_mask{0};
};

VectorDecision vector_decision(const TargetProfile &target,
                               NormalizedVectorOpcode opcode,
                               ir::ValueType input_type, ir::Word immediate,
                               const ir::MemoryAccessDescriptor *access) {
  const std::size_t lane_bits = ir::vector_lane_bits(input_type);
  if (target.architecture == TargetArchitecture::kRiscV64) {
    std::uint32_t resources =
        lowering_resource_bit(LoweringResource::kWordRegister) |
        lowering_resource_bit(LoweringResource::kAlignedVectorStack);
    if (ir::is_float_vector_type(input_type)) {
      resources |= lowering_resource_bit(LoweringResource::kFloatRegister);
    }
    return {LoweringStrategy::kScalarized, resources};
  }

  if (target.architecture == TargetArchitecture::kAArch64) {
    const bool big_endian_memory =
        opcode == NormalizedVectorOpcode::kMemory && access != nullptr &&
        access->byte_order == ir::MemoryByteOrder::kBigEndian && lane_bits > 8;
    const bool i64_multiply =
        opcode == NormalizedVectorOpcode::kBinary && lane_bits == 64 &&
        !ir::is_float_vector_type(input_type) &&
        static_cast<ir::VectorBinaryOperation>(immediate) ==
            ir::VectorBinaryOperation::kMultiply;
    const bool lane_sign_mask = opcode == NormalizedVectorOpcode::kLaneSignMask;
    if (big_endian_memory || i64_multiply || lane_sign_mask) {
      return {LoweringStrategy::kLegalized,
              lowering_resource_bit(LoweringResource::kWordRegister) |
                  lowering_resource_bit(LoweringResource::kVectorRegister)};
    }
    return {LoweringStrategy::kNative,
            lowering_resource_bit(LoweringResource::kVectorRegister)};
  }

  if (target.architecture == TargetArchitecture::kX86_64) {
    bool legalized = false;
    if (opcode == NormalizedVectorOpcode::kMemory) {
      legalized = access != nullptr &&
                  access->byte_order == ir::MemoryByteOrder::kBigEndian &&
                  lane_bits > 8;
    } else if (opcode == NormalizedVectorOpcode::kInsertLane) {
      legalized = true;
    } else if (opcode == NormalizedVectorOpcode::kBinary &&
               !ir::is_float_vector_type(input_type)) {
      const auto operation = static_cast<ir::VectorBinaryOperation>(immediate);
      legalized =
          operation == ir::VectorBinaryOperation::kMultiply && lane_bits != 16;
    } else if (opcode == NormalizedVectorOpcode::kCompare) {
      legalized = !ir::is_float_vector_type(input_type);
    } else if (opcode == NormalizedVectorOpcode::kLaneSignMask) {
      legalized = lane_bits == 16;
    } else if (opcode == NormalizedVectorOpcode::kShuffle) {
      legalized = lane_bits < 32;
    }
    if (legalized) {
      return {LoweringStrategy::kLegalized,
              lowering_resource_bit(LoweringResource::kWordRegister) |
                  lowering_resource_bit(LoweringResource::kVectorRegister) |
                  lowering_resource_bit(LoweringResource::kAlignedVectorStack)};
    }
    return {LoweringStrategy::kNative,
            lowering_resource_bit(LoweringResource::kVectorRegister)};
  }

  return {};
}

template <typename FunctionType, typename NodeType>
ir::ValueType vector_input_type(const FunctionType &function,
                                const NodeType &node,
                                NormalizedVectorOpcode opcode) noexcept {
  if (opcode == NormalizedVectorOpcode::kExtractLane ||
      opcode == NormalizedVectorOpcode::kCompare ||
      opcode == NormalizedVectorOpcode::kLaneSignMask ||
      opcode == NormalizedVectorOpcode::kWiden) {
    return function.value_type(node.lhs);
  }
  return node.type;
}

void note_operation(CapabilityReport *report, LoweringStrategy strategy,
                    std::uint64_t required_features) noexcept {
  const std::size_t strategy_index = static_cast<std::size_t>(strategy);
  if (strategy_index < report->operation_counts.size()) {
    ++report->operation_counts[strategy_index];
  }
  report->required_features |= required_features;
  merge_strategy(strategy, &report->overall_strategy);
}

template <typename FunctionType>
CapabilityReport analyze_verified(const FunctionType &function,
                                  const TargetProfile &target) {
  CapabilityReport report;
  report.status = Status::ok_status();
  report.target_profile = target;
  bool found_unsupported = false;
  std::size_t first_unsupported = 0;
  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const auto &node = function.nodes()[index];
    report.requires_execution_context =
        report.requires_execution_context || requires_context(node.opcode);
    if (is_atomic(node.opcode)) {
      if (!found_unsupported) {
        found_unsupported = true;
        first_unsupported = index;
      }
      note_operation(&report, LoweringStrategy::kUnsupported, 0);
      continue;
    }
    const NormalizedVectorOpcode vector_opcode = normalize(node.opcode);
    if (vector_opcode == NormalizedVectorOpcode::kNone) {
      LoweringStrategy strategy = is_call(node.opcode)
                                      ? LoweringStrategy::kHelper
                                      : LoweringStrategy::kNative;
      const std::uint64_t required = scalar_features(
          target.architecture, uses_fp64(node.opcode, node.type),
          is_word_multiply(node.opcode, node.type));
      if ((target.features & required) != required) {
        strategy = LoweringStrategy::kUnsupported;
        if (!found_unsupported) {
          found_unsupported = true;
          first_unsupported = index;
        }
      }
      note_operation(&report, strategy, required);
      continue;
    }

    report.vector_bits = 128;
    const ir::ValueType input_type =
        vector_input_type(function, node, vector_opcode);
    const std::uint64_t required = vector_features(
        target.architecture, input_type, vector_opcode, node.immediate);
    const ir::MemoryAccessDescriptor *access =
        vector_opcode == NormalizedVectorOpcode::kMemory
            ? &function.memory_accesses()[node.memory_access]
            : nullptr;
    VectorDecision decision = vector_decision(target, vector_opcode, input_type,
                                              node.immediate, access);
    if ((target.features & required) != required) {
      decision.strategy = LoweringStrategy::kUnsupported;
    }
    if (decision.strategy == LoweringStrategy::kUnsupported &&
        !found_unsupported) {
      found_unsupported = true;
      first_unsupported = index;
    }
    const VectorOperationClass category =
        operation_class(vector_opcode, input_type, node.immediate);
    const std::size_t category_index = static_cast<std::size_t>(category);
    if (category_index < report.vector_operations.size()) {
      VectorOperationCapability &capability =
          report.vector_operations[category_index];
      ++capability.operation_count;
      capability.required_features |= required;
      capability.resource_mask |= decision.resource_mask;
      merge_strategy(decision.strategy, &capability.strategy);
    }
    note_operation(&report, decision.strategy, required);
  }

  if (report.overall_strategy == LoweringStrategy::kUnsupported) {
    report.status = {StatusCode::kCodeGenerationFailed,
                     "target profile cannot lower the complete operation set",
                     first_unsupported};
  }
  return report;
}

} // namespace

CapabilityReport preflight_capabilities(const ir::Function &function,
                                        const TargetProfile &target_profile) {
  CapabilityReport report;
  report.target_profile = target_profile;
  report.status = validate_target_profile(target_profile);
  if (!report.status.ok()) {
    report.overall_strategy = LoweringStrategy::kUnsupported;
    return report;
  }
  report.status = ir::verify(function);
  if (!report.status.ok()) {
    report.overall_strategy = LoweringStrategy::kUnsupported;
    return report;
  }
  return analyze_verified(function, target_profile);
}

CapabilityReport preflight_capabilities(const ir::ControlFlowFunction &function,
                                        const TargetProfile &target_profile) {
  CapabilityReport report;
  report.target_profile = target_profile;
  report.status = validate_target_profile(target_profile);
  if (!report.status.ok()) {
    report.overall_strategy = LoweringStrategy::kUnsupported;
    return report;
  }
  report.status = ir::verify(function);
  if (!report.status.ok()) {
    report.overall_strategy = LoweringStrategy::kUnsupported;
    return report;
  }
  return analyze_verified(function, target_profile);
}

const char *lowering_strategy_name(LoweringStrategy strategy) noexcept {
  switch (strategy) {
  case LoweringStrategy::kNative:
    return "native";
  case LoweringStrategy::kLegalized:
    return "legalized";
  case LoweringStrategy::kScalarized:
    return "scalarized";
  case LoweringStrategy::kHelper:
    return "helper";
  case LoweringStrategy::kUnsupported:
    return "unsupported";
  case LoweringStrategy::kCount:
    break;
  }
  return "unknown";
}

const char *
vector_operation_class_name(VectorOperationClass operation_class) noexcept {
  switch (operation_class) {
  case VectorOperationClass::kMemory:
    return "memory";
  case VectorOperationClass::kConstruction:
    return "construction";
  case VectorOperationClass::kLaneMovement:
    return "lane-movement";
  case VectorOperationClass::kIntegerArithmetic:
    return "integer-arithmetic";
  case VectorOperationClass::kFloatingArithmetic:
    return "floating-arithmetic";
  case VectorOperationClass::kBitwiseLogic:
    return "bitwise-logic";
  case VectorOperationClass::kComparison:
    return "comparison";
  case VectorOperationClass::kSelection:
    return "selection";
  case VectorOperationClass::kLaneSignMask:
    return "lane-sign-mask";
  case VectorOperationClass::kShuffle:
    return "shuffle";
  case VectorOperationClass::kWidening:
    return "widening";
  case VectorOperationClass::kCount:
    break;
  }
  return "unknown";
}

} // namespace unijit::jit
