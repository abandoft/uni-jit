#include "unijit/ir/vector.h"

#include <cmath>
#include <cstring>

namespace unijit::ir {
namespace {

std::uint64_t word_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint64_t lane_mask(std::size_t lane_bits) noexcept {
  if (lane_bits == 0 || lane_bits > 64) {
    return 0;
  }
  return lane_bits == 64 ? UINT64_MAX
                         : (UINT64_C(1) << lane_bits) - UINT64_C(1);
}

std::uint64_t load_lane(const Vector128& value, std::size_t lane,
                        std::size_t lane_bits) noexcept {
  const std::size_t lane_bytes = lane_bits / 8U;
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < lane_bytes; ++index) {
    result |= static_cast<std::uint64_t>(
                  value.bytes[lane * lane_bytes + index])
              << (index * 8U);
  }
  return result;
}

void store_lane(Vector128* value, std::size_t lane, std::size_t lane_bits,
                std::uint64_t bits) noexcept {
  const std::size_t lane_bytes = lane_bits / 8U;
  for (std::size_t index = 0; index < lane_bytes; ++index) {
    value->bytes[lane * lane_bytes + index] =
        static_cast<std::uint8_t>(bits >> (index * 8U));
  }
}

std::int64_t signed_lane(std::uint64_t bits,
                         std::size_t lane_bits) noexcept {
  if (lane_bits == 64) {
    Word result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }
  const std::uint64_t sign = UINT64_C(1) << (lane_bits - 1U);
  const std::uint64_t extended =
      (bits & sign) != 0 ? bits | ~lane_mask(lane_bits) : bits;
  std::int64_t result = 0;
  std::memcpy(&result, &extended, sizeof(result));
  return result;
}

float unpack_float32(std::uint64_t bits) noexcept {
  const std::uint32_t narrow = static_cast<std::uint32_t>(bits);
  float value = 0.0F;
  std::memcpy(&value, &narrow, sizeof(value));
  return value;
}

std::uint32_t pack_float32(float value) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

double unpack_float64_bits(std::uint64_t bits) noexcept {
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint64_t pack_float64_bits(double value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool compare_integer(VectorComparison comparison, std::uint64_t lhs,
                     std::uint64_t rhs, std::size_t lane_bits) noexcept {
  switch (comparison) {
    case VectorComparison::kEqual:
      return lhs == rhs;
    case VectorComparison::kSignedLessThan:
      return signed_lane(lhs, lane_bits) < signed_lane(rhs, lane_bits);
    case VectorComparison::kSignedLessEqual:
      return signed_lane(lhs, lane_bits) <= signed_lane(rhs, lane_bits);
    case VectorComparison::kUnsignedLessThan:
      return lhs < rhs;
    case VectorComparison::kUnsignedLessEqual:
      return lhs <= rhs;
    case VectorComparison::kOrderedFloatEqual:
    case VectorComparison::kOrderedFloatLessThan:
    case VectorComparison::kOrderedFloatLessEqual:
      return false;
  }
  return false;
}

bool compare_float(VectorComparison comparison, double lhs,
                   double rhs) noexcept {
  if (std::isnan(lhs) || std::isnan(rhs)) {
    return false;
  }
  switch (comparison) {
    case VectorComparison::kOrderedFloatEqual:
      return lhs == rhs;
    case VectorComparison::kOrderedFloatLessThan:
      return lhs < rhs;
    case VectorComparison::kOrderedFloatLessEqual:
      return lhs <= rhs;
    case VectorComparison::kEqual:
    case VectorComparison::kSignedLessThan:
    case VectorComparison::kSignedLessEqual:
    case VectorComparison::kUnsignedLessThan:
    case VectorComparison::kUnsignedLessEqual:
      return false;
  }
  return false;
}

bool valid_vector_binary(VectorBinaryOperation operation,
                         ValueType type) noexcept {
  if (operation == VectorBinaryOperation::kBitwiseAnd ||
      operation == VectorBinaryOperation::kBitwiseOr ||
      operation == VectorBinaryOperation::kBitwiseXor) {
    return is_vector_value_type(type);
  }
  if (operation == VectorBinaryOperation::kDivide) {
    return is_float_vector_type(type);
  }
  if (operation == VectorBinaryOperation::kAdd ||
      operation == VectorBinaryOperation::kSubtract ||
      operation == VectorBinaryOperation::kMultiply) {
    return is_integer_vector_type(type) || is_float_vector_type(type);
  }
  return false;
}

bool valid_vector_comparison(VectorComparison comparison,
                             ValueType type) noexcept {
  if (is_float_vector_type(type)) {
    return comparison == VectorComparison::kOrderedFloatEqual ||
           comparison == VectorComparison::kOrderedFloatLessThan ||
           comparison == VectorComparison::kOrderedFloatLessEqual;
  }
  if (!is_integer_vector_type(type)) {
    return false;
  }
  return comparison == VectorComparison::kEqual ||
         comparison == VectorComparison::kSignedLessThan ||
         comparison == VectorComparison::kSignedLessEqual ||
         comparison == VectorComparison::kUnsignedLessThan ||
         comparison == VectorComparison::kUnsignedLessEqual;
}

}  // namespace

bool vector_mask_is_canonical(const Vector128& value,
                              ValueType mask_type) noexcept {
  if (!is_mask_vector_type(mask_type)) {
    return false;
  }
  const std::size_t lane_bits = vector_lane_bits(mask_type);
  const std::uint64_t all = lane_mask(lane_bits);
  for (std::size_t lane = 0; lane < vector_lane_count(mask_type); ++lane) {
    const std::uint64_t bits = load_lane(value, lane, lane_bits);
    if (bits != 0 && bits != all) {
      return false;
    }
  }
  return true;
}

Vector128 vector_splat_bits(ValueType type, Word lane_bits) noexcept {
  Vector128 result;
  if (!is_integer_vector_type(type) && !is_float_vector_type(type)) {
    return result;
  }
  const std::size_t width = vector_lane_bits(type);
  const std::uint64_t bits = word_bits(lane_bits) & lane_mask(width);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    store_lane(&result, lane, width, bits);
  }
  return result;
}

Word vector_extract_lane_bits(const Vector128& value, ValueType type,
                              std::size_t lane,
                              bool sign_extend) noexcept {
  if (!is_vector_value_type(type) || lane >= vector_lane_count(type) ||
      (sign_extend && !is_integer_vector_type(type))) {
    return 0;
  }
  const std::size_t width = vector_lane_bits(type);
  const std::uint64_t bits = load_lane(value, lane, width);
  return sign_extend ? static_cast<Word>(signed_lane(bits, width))
                     : from_bits(bits);
}

Vector128 vector_insert_lane_bits(const Vector128& value, ValueType type,
                                  std::size_t lane, Word lane_bits) noexcept {
  if ((!is_integer_vector_type(type) && !is_float_vector_type(type)) ||
      lane >= vector_lane_count(type)) {
    return {};
  }
  Vector128 result = value;
  const std::size_t width = vector_lane_bits(type);
  store_lane(&result, lane, width, word_bits(lane_bits));
  return result;
}

Vector128 vector_unary(VectorUnaryOperation operation, const Vector128& value,
                       ValueType type) noexcept {
  Vector128 result;
  if (operation != VectorUnaryOperation::kBitwiseNot ||
      !is_vector_value_type(type)) {
    return result;
  }
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(~value.bytes[index]);
  }
  return result;
}

Vector128 vector_binary(VectorBinaryOperation operation,
                        const Vector128& lhs, const Vector128& rhs,
                        ValueType type) noexcept {
  Vector128 result;
  if (!valid_vector_binary(operation, type)) {
    return result;
  }
  if (operation == VectorBinaryOperation::kBitwiseAnd ||
      operation == VectorBinaryOperation::kBitwiseOr ||
      operation == VectorBinaryOperation::kBitwiseXor) {
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
      if (operation == VectorBinaryOperation::kBitwiseAnd) {
        result.bytes[index] = lhs.bytes[index] & rhs.bytes[index];
      } else if (operation == VectorBinaryOperation::kBitwiseOr) {
        result.bytes[index] = lhs.bytes[index] | rhs.bytes[index];
      } else {
        result.bytes[index] = lhs.bytes[index] ^ rhs.bytes[index];
      }
    }
    return result;
  }

  const std::size_t width = vector_lane_bits(type);
  const std::uint64_t mask = lane_mask(width);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    const std::uint64_t lhs_bits = load_lane(lhs, lane, width);
    const std::uint64_t rhs_bits = load_lane(rhs, lane, width);
    std::uint64_t bits = 0;
    if (is_float_vector_type(type)) {
      if (width == 32) {
        const float lhs_value = unpack_float32(lhs_bits);
        const float rhs_value = unpack_float32(rhs_bits);
        float value = 0.0F;
        if (operation == VectorBinaryOperation::kAdd) {
          value = lhs_value + rhs_value;
        } else if (operation == VectorBinaryOperation::kSubtract) {
          value = lhs_value - rhs_value;
        } else if (operation == VectorBinaryOperation::kMultiply) {
          value = lhs_value * rhs_value;
        } else if (operation == VectorBinaryOperation::kDivide) {
          value = lhs_value / rhs_value;
        }
        bits = pack_float32(value);
      } else {
        const double lhs_value = unpack_float64_bits(lhs_bits);
        const double rhs_value = unpack_float64_bits(rhs_bits);
        double value = 0.0;
        if (operation == VectorBinaryOperation::kAdd) {
          value = lhs_value + rhs_value;
        } else if (operation == VectorBinaryOperation::kSubtract) {
          value = lhs_value - rhs_value;
        } else if (operation == VectorBinaryOperation::kMultiply) {
          value = lhs_value * rhs_value;
        } else if (operation == VectorBinaryOperation::kDivide) {
          value = lhs_value / rhs_value;
        }
        bits = pack_float64_bits(value);
      }
    } else if (operation == VectorBinaryOperation::kAdd) {
      bits = (lhs_bits + rhs_bits) & mask;
    } else if (operation == VectorBinaryOperation::kSubtract) {
      bits = (lhs_bits - rhs_bits) & mask;
    } else if (operation == VectorBinaryOperation::kMultiply) {
      bits = (lhs_bits * rhs_bits) & mask;
    }
    store_lane(&result, lane, width, bits);
  }
  return result;
}

Vector128 vector_compare(VectorComparison comparison, const Vector128& lhs,
                         const Vector128& rhs, ValueType type) noexcept {
  Vector128 result;
  if (!valid_vector_comparison(comparison, type)) {
    return result;
  }
  const std::size_t width = vector_lane_bits(type);
  const std::uint64_t all = lane_mask(width);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    const std::uint64_t lhs_bits = load_lane(lhs, lane, width);
    const std::uint64_t rhs_bits = load_lane(rhs, lane, width);
    bool matches = false;
    if (is_float_vector_type(type)) {
      matches = width == 32
                    ? compare_float(comparison, unpack_float32(lhs_bits),
                                    unpack_float32(rhs_bits))
                    : compare_float(comparison, unpack_float64_bits(lhs_bits),
                                    unpack_float64_bits(rhs_bits));
    } else {
      matches = compare_integer(comparison, lhs_bits, rhs_bits, width);
    }
    store_lane(&result, lane, width, matches ? all : 0);
  }
  return result;
}

Vector128 vector_select(const Vector128& mask, const Vector128& true_value,
                        const Vector128& false_value,
                        ValueType mask_type) noexcept {
  Vector128 result;
  if (!vector_mask_is_canonical(mask, mask_type)) {
    return result;
  }
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    result.bytes[index] =
        (mask.bytes[index] & true_value.bytes[index]) |
        (static_cast<std::uint8_t>(~mask.bytes[index]) &
         false_value.bytes[index]);
  }
  return result;
}

Word vector_lane_sign_mask(const Vector128& value, ValueType type) noexcept {
  if (!is_vector_value_type(type)) {
    return 0;
  }
  std::uint64_t result = 0;
  const std::size_t width = vector_lane_bits(type);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    result |= ((load_lane(value, lane, width) >> (width - 1U)) & 1U) << lane;
  }
  return from_bits(result);
}

Vector128 vector_shuffle(const Vector128& value, ValueType type,
                         const VectorShuffle& shuffle) noexcept {
  Vector128 result;
  const std::size_t lane_count = vector_lane_count(type);
  if (!is_vector_value_type(type) || shuffle.lane_count != lane_count) {
    return result;
  }
  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    if (shuffle.lanes[lane] >= lane_count) {
      return {};
    }
  }
  const std::size_t width = vector_lane_bits(type);
  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    store_lane(&result, lane, width,
               load_lane(value, shuffle.lanes[lane], width));
  }
  return result;
}

Vector128 vector_widen(const Vector128& value, ValueType source_type,
                       ValueType result_type, VectorExtension extension,
                       VectorHalf half) noexcept {
  Vector128 result;
  if (!is_integer_vector_type(source_type) ||
      !is_integer_vector_type(result_type) ||
      vector_lane_bits(result_type) != vector_lane_bits(source_type) * 2U ||
      vector_lane_count(result_type) * 2U !=
          vector_lane_count(source_type) ||
      (extension != VectorExtension::kZero &&
       extension != VectorExtension::kSign) ||
      (half != VectorHalf::kLow && half != VectorHalf::kHigh)) {
    return result;
  }
  const std::size_t source_width = vector_lane_bits(source_type);
  const std::size_t result_width = vector_lane_bits(result_type);
  const std::size_t result_lanes = vector_lane_count(result_type);
  const std::size_t first =
      half == VectorHalf::kLow ? 0 : vector_lane_count(source_type) / 2U;
  for (std::size_t lane = 0; lane < result_lanes; ++lane) {
    const std::uint64_t source = load_lane(value, first + lane, source_width);
    const std::uint64_t widened =
        extension == VectorExtension::kSign
            ? static_cast<std::uint64_t>(signed_lane(source, source_width))
            : source;
    store_lane(&result, lane, result_width, widened);
  }
  return result;
}

}  // namespace unijit::ir
