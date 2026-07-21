#include "ir/memory_access.h"

#include <cstdint>
#include <cstring>

namespace unijit::ir::detail {
namespace {

struct ResolvedAccess final {
  Status status;
  std::uint8_t* address{nullptr};
  std::size_t width{0};
};

std::uint64_t word_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word bits_word(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool native_is_little_endian() noexcept {
  const std::uint16_t value = 1;
  std::uint8_t first = 0;
  std::memcpy(&first, &value, sizeof(first));
  return first == 1;
}

MemoryAccessResult access_failure(runtime::ExecutionContext* context,
                                  std::size_t site, Word byte_offset,
                                  const char* message) noexcept {
  if (context != nullptr) {
    context->record_exit(runtime::ExitReason::kRuntime, site, byte_offset);
  }
  return {{StatusCode::kRuntimeExit, message, site}, 0};
}

ResolvedAccess resolve(const MemoryAccessDescriptor& access,
                       Word byte_offset, std::size_t site,
                       runtime::ExecutionContext* context,
                       bool store) noexcept {
  if (context == nullptr || context->memory_regions() == nullptr ||
      access.region >= context->memory_region_count()) {
    return {access_failure(context, site, byte_offset,
                           "bounded memory region is not bound")
                .status,
            nullptr, 0};
  }
  const runtime::MemoryRegion& region =
      context->memory_regions()[access.region];
  if (store && !region.writable) {
    return {access_failure(context, site, byte_offset,
                           "bounded memory region is read-only")
                .status,
            nullptr, 0};
  }
  const std::size_t width = memory_width_bytes(access.width);
  const std::uint64_t offset = word_bits(byte_offset);
  if (width == 0 || width > region.size ||
      offset > static_cast<std::uint64_t>(region.size - width)) {
    return {access_failure(context, site, byte_offset,
                           "bounded memory access is out of range")
                .status,
            nullptr, 0};
  }
  auto* base = static_cast<std::uint8_t*>(region.data);
  auto* address = base + static_cast<std::size_t>(offset);
  const auto numeric_address = reinterpret_cast<std::uintptr_t>(address);
  if (access.alignment == 0 ||
      (numeric_address & (static_cast<std::uintptr_t>(access.alignment) - 1U)) !=
          0) {
    return {access_failure(context, site, byte_offset,
                           "bounded memory access is misaligned")
                .status,
            nullptr, 0};
  }
  return {Status::ok_status(), address, width};
}

std::uint8_t read_byte(const std::uint8_t* address, std::size_t index,
                       bool is_volatile) noexcept {
  if (is_volatile) {
    const volatile std::uint8_t* source = address;
    return source[index];
  }
  return address[index];
}

void write_byte(std::uint8_t* address, std::size_t index, std::uint8_t value,
                bool is_volatile) noexcept {
  if (is_volatile) {
    volatile std::uint8_t* destination = address;
    destination[index] = value;
    return;
  }
  address[index] = value;
}

bool little_endian(MemoryByteOrder order) noexcept {
  return order == MemoryByteOrder::kLittleEndian ||
         (order == MemoryByteOrder::kNative && native_is_little_endian());
}

}  // namespace

MemoryAccessResult load_bounded_word(
    const MemoryAccessDescriptor& access, Word byte_offset, std::size_t site,
    runtime::ExecutionContext* context) noexcept {
  const ResolvedAccess resolved =
      resolve(access, byte_offset, site, context, false);
  if (!resolved.status.ok()) {
    return {resolved.status, 0};
  }
  std::uint64_t bits = 0;
  const bool little = little_endian(access.byte_order);
  for (std::size_t index = 0; index < resolved.width; ++index) {
    const std::size_t shift_index = little ? index : resolved.width - index - 1;
    bits |= static_cast<std::uint64_t>(
                read_byte(resolved.address, index, access.is_volatile))
            << (shift_index * 8U);
  }
  if (access.sign_extend && resolved.width < sizeof(bits)) {
    const std::size_t bit_count = resolved.width * 8U;
    const std::uint64_t sign = UINT64_C(1) << (bit_count - 1U);
    if ((bits & sign) != 0) {
      bits |= ~((UINT64_C(1) << bit_count) - 1U);
    }
  }
  return {Status::ok_status(), bits_word(bits)};
}

MemoryAccessResult store_bounded_word(
    const MemoryAccessDescriptor& access, Word byte_offset, Word value,
    std::size_t site, runtime::ExecutionContext* context) noexcept {
  const ResolvedAccess resolved =
      resolve(access, byte_offset, site, context, true);
  if (!resolved.status.ok()) {
    return {resolved.status, 0};
  }
  const std::uint64_t bits = word_bits(value);
  const bool little = little_endian(access.byte_order);
  for (std::size_t index = 0; index < resolved.width; ++index) {
    const std::size_t shift_index = little ? index : resolved.width - index - 1;
    write_byte(resolved.address, index,
               static_cast<std::uint8_t>(bits >> (shift_index * 8U)),
               access.is_volatile);
  }
  return {Status::ok_status(), value};
}

MemoryAccessResult load_bounded_float(
    const MemoryAccessDescriptor& access, Word byte_offset, std::size_t site,
    runtime::ExecutionContext* context) noexcept {
  const MemoryAccessResult loaded =
      load_bounded_word(access, byte_offset, site, context);
  if (!loaded.ok() || access.width == MemoryWidth::k64) {
    return loaded;
  }
  std::uint32_t bits = static_cast<std::uint32_t>(word_bits(loaded.value));
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return {Status::ok_status(), pack_float64(static_cast<double>(value))};
}

MemoryAccessResult store_bounded_float(
    const MemoryAccessDescriptor& access, Word byte_offset, Word value,
    std::size_t site, runtime::ExecutionContext* context) noexcept {
  Word stored_bits = value;
  if (access.width == MemoryWidth::k32) {
    const float narrowed = static_cast<float>(unpack_float64(value));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &narrowed, sizeof(bits));
    stored_bits = bits_word(bits);
  }
  const MemoryAccessResult stored =
      store_bounded_word(access, byte_offset, stored_bits, site, context);
  return stored.ok() ? MemoryAccessResult{Status::ok_status(), value} : stored;
}

VectorMemoryAccessResult
load_bounded_vector(const MemoryAccessDescriptor &access, ValueType type,
                    Word byte_offset, std::size_t site,
                    runtime::ExecutionContext *context) noexcept {
  const ResolvedAccess resolved =
      resolve(access, byte_offset, site, context, false);
  if (!resolved.status.ok()) {
    return {resolved.status, {}};
  }
  Vector128 result;
  const std::size_t lane_bytes = vector_lane_bits(type) / 8U;
  const bool little = little_endian(access.byte_order);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    for (std::size_t index = 0; index < lane_bytes; ++index) {
      const std::size_t memory_index = lane * lane_bytes + index;
      const std::size_t logical_index =
          lane * lane_bytes + (little ? index : lane_bytes - index - 1U);
      result.bytes[logical_index] =
          read_byte(resolved.address, memory_index, access.is_volatile);
    }
  }
  return {Status::ok_status(), result};
}

VectorMemoryAccessResult
store_bounded_vector(const MemoryAccessDescriptor &access, ValueType type,
                     Word byte_offset, const Vector128 &value, std::size_t site,
                     runtime::ExecutionContext *context) noexcept {
  const ResolvedAccess resolved =
      resolve(access, byte_offset, site, context, true);
  if (!resolved.status.ok()) {
    return {resolved.status, {}};
  }
  const std::size_t lane_bytes = vector_lane_bits(type) / 8U;
  const bool little = little_endian(access.byte_order);
  for (std::size_t lane = 0; lane < vector_lane_count(type); ++lane) {
    for (std::size_t index = 0; index < lane_bytes; ++index) {
      const std::size_t memory_index = lane * lane_bytes + index;
      const std::size_t logical_index =
          lane * lane_bytes + (little ? index : lane_bytes - index - 1U);
      write_byte(resolved.address, memory_index, value.bytes[logical_index],
                 access.is_volatile);
    }
  }
  return {Status::ok_status(), value};
}

}  // namespace unijit::ir::detail
