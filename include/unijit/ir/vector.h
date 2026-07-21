#ifndef UNIJIT_IR_VECTOR_H
#define UNIJIT_IR_VECTOR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace unijit::ir {

using Word = std::int64_t;

enum class ValueType : std::uint8_t {
  kWord,
  kFloat64,
  kI8x16,
  kI16x8,
  kI32x4,
  kI64x2,
  kF32x4,
  kF64x2,
  kMask8x16,
  kMask16x8,
  kMask32x4,
  kMask64x2,
};

struct Vector128 final {
  static constexpr std::size_t kByteSize = 16;

  std::array<std::uint8_t, kByteSize> bytes{};

  friend constexpr bool operator==(const Vector128& lhs,
                                   const Vector128& rhs) noexcept {
    return lhs.bytes == rhs.bytes;
  }

  friend constexpr bool operator!=(const Vector128& lhs,
                                   const Vector128& rhs) noexcept {
    return !(lhs == rhs);
  }
};

static_assert(sizeof(Vector128) == Vector128::kByteSize,
              "Vector128 must contain exactly 128 logical bits");
static_assert(std::is_trivially_copyable<Vector128>::value,
              "Vector128 side-table values must be trivially copyable");

struct VectorShuffle final {
  static constexpr std::uint32_t kInvalidIndex =
      std::numeric_limits<std::uint32_t>::max();

  std::array<std::uint8_t, 16> lanes{};
  std::uint8_t lane_count{0};
};

enum class VectorUnaryOperation : std::uint8_t {
  kBitwiseNot = 0,
};

enum class VectorBinaryOperation : std::uint8_t {
  kAdd = 0,
  kSubtract,
  kMultiply,
  kBitwiseAnd,
  kBitwiseOr,
  kBitwiseXor,
  kDivide,
};

enum class VectorComparison : std::uint8_t {
  kEqual = 0,
  kSignedLessThan,
  kSignedLessEqual,
  kUnsignedLessThan,
  kUnsignedLessEqual,
  kOrderedFloatEqual,
  kOrderedFloatLessThan,
  kOrderedFloatLessEqual,
};

enum class VectorExtension : std::uint8_t {
  kZero = 0,
  kSign,
};

enum class VectorHalf : std::uint8_t {
  kLow = 0,
  kHigh,
};

constexpr bool is_scalar_value_type(ValueType type) noexcept {
  return type == ValueType::kWord || type == ValueType::kFloat64;
}

constexpr bool is_integer_vector_type(ValueType type) noexcept {
  return type == ValueType::kI8x16 || type == ValueType::kI16x8 ||
         type == ValueType::kI32x4 || type == ValueType::kI64x2;
}

constexpr bool is_float_vector_type(ValueType type) noexcept {
  return type == ValueType::kF32x4 || type == ValueType::kF64x2;
}

constexpr bool is_mask_vector_type(ValueType type) noexcept {
  return type == ValueType::kMask8x16 || type == ValueType::kMask16x8 ||
         type == ValueType::kMask32x4 || type == ValueType::kMask64x2;
}

constexpr bool is_vector_value_type(ValueType type) noexcept {
  return is_integer_vector_type(type) || is_float_vector_type(type) ||
         is_mask_vector_type(type);
}

constexpr bool is_valid_value_type(ValueType type) noexcept {
  return is_scalar_value_type(type) || is_vector_value_type(type);
}

constexpr std::size_t vector_lane_bits(ValueType type) noexcept {
  switch (type) {
    case ValueType::kI8x16:
    case ValueType::kMask8x16:
      return 8;
    case ValueType::kI16x8:
    case ValueType::kMask16x8:
      return 16;
    case ValueType::kI32x4:
    case ValueType::kF32x4:
    case ValueType::kMask32x4:
      return 32;
    case ValueType::kI64x2:
    case ValueType::kF64x2:
    case ValueType::kMask64x2:
      return 64;
    case ValueType::kWord:
    case ValueType::kFloat64:
      return 0;
  }
  return 0;
}

constexpr std::size_t vector_lane_count(ValueType type) noexcept {
  const std::size_t lane_bits = vector_lane_bits(type);
  return lane_bits == 0 ? 0 : 128U / lane_bits;
}

constexpr ValueType vector_mask_type(ValueType type) noexcept {
  switch (vector_lane_bits(type)) {
    case 8:
      return ValueType::kMask8x16;
    case 16:
      return ValueType::kMask16x8;
    case 32:
      return ValueType::kMask32x4;
    case 64:
      return ValueType::kMask64x2;
    default:
      return ValueType::kWord;
  }
}

constexpr bool vector_shapes_match(ValueType lhs, ValueType rhs) noexcept {
  return is_vector_value_type(lhs) && is_vector_value_type(rhs) &&
         vector_lane_bits(lhs) == vector_lane_bits(rhs);
}

bool vector_mask_is_canonical(const Vector128& value,
                              ValueType mask_type) noexcept;
Vector128 vector_splat_bits(ValueType type, Word lane_bits) noexcept;
Word vector_extract_lane_bits(const Vector128& value, ValueType type,
                              std::size_t lane,
                              bool sign_extend) noexcept;
Vector128 vector_insert_lane_bits(const Vector128& value, ValueType type,
                                  std::size_t lane, Word lane_bits) noexcept;
Vector128 vector_unary(VectorUnaryOperation operation, const Vector128& value,
                       ValueType type) noexcept;
Vector128 vector_binary(VectorBinaryOperation operation,
                        const Vector128& lhs, const Vector128& rhs,
                        ValueType type) noexcept;
Vector128 vector_compare(VectorComparison comparison, const Vector128& lhs,
                         const Vector128& rhs, ValueType type) noexcept;
Vector128 vector_select(const Vector128& mask, const Vector128& true_value,
                        const Vector128& false_value,
                        ValueType mask_type) noexcept;
Word vector_lane_sign_mask(const Vector128& value, ValueType type) noexcept;
Vector128 vector_shuffle(const Vector128& value, ValueType type,
                         const VectorShuffle& shuffle) noexcept;
Vector128 vector_widen(const Vector128& value, ValueType source_type,
                       ValueType result_type, VectorExtension extension,
                       VectorHalf half) noexcept;

}  // namespace unijit::ir

#endif  // UNIJIT_IR_VECTOR_H
