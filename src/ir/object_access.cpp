#include "ir/object_access.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace unijit::ir::detail {
namespace {

Status invalid_binding(const char* message, std::size_t index = 0) noexcept {
  return {StatusCode::kInvalidArgument, message, index};
}

std::uint8_t* resolve(TrustedObjectSlot slot, std::size_t byte_offset,
                      bool store, runtime::ExecutionContext* context,
                      Status* status) noexcept {
  if (context == nullptr || context->trusted_objects() == nullptr ||
      !slot.valid() || slot.id() >= context->trusted_object_count()) {
    *status = invalid_binding("trusted object is not bound", slot.id());
    return nullptr;
  }
  const runtime::TrustedObject& object =
      context->trusted_objects()[slot.id()];
  if (store && !object.writable) {
    *status = invalid_binding("trusted object is read-only", slot.id());
    return nullptr;
  }
  if (byte_offset > object.size || sizeof(Word) > object.size - byte_offset) {
    *status = invalid_binding("trusted object field is out of range", slot.id());
    return nullptr;
  }
  auto* address = static_cast<std::uint8_t*>(object.data) + byte_offset;
  if ((reinterpret_cast<std::uintptr_t>(address) & (alignof(Word) - 1U)) != 0) {
    *status = invalid_binding("trusted object field is misaligned", slot.id());
    return nullptr;
  }
  *status = Status::ok_status();
  return address;
}

}  // namespace

Status validate_trusted_object_bindings(
    const std::vector<TrustedObjectDescriptor>& descriptors,
    const std::vector<bool>& writable,
    runtime::ExecutionContext* context) noexcept {
  if (writable.size() != descriptors.size()) {
    return invalid_binding("trusted-object access metadata is inconsistent");
  }
  if (descriptors.empty()) {
    return Status::ok_status();
  }
  if (context == nullptr || context->trusted_objects() == nullptr ||
      context->trusted_object_count() < descriptors.size()) {
    return invalid_binding("required trusted objects are not bound");
  }
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const TrustedObjectDescriptor& descriptor = descriptors[index];
    const runtime::TrustedObject& object = context->trusted_objects()[index];
    if (object.layout_identity != descriptor.layout_identity) {
      return invalid_binding("trusted object layout identity does not match",
                             index);
    }
    if (object.data == nullptr || object.size < descriptor.byte_size) {
      return invalid_binding("trusted object is smaller than its layout",
                             index);
    }
    const auto base = reinterpret_cast<std::uintptr_t>(object.data);
    if ((base & (alignof(Word) - 1U)) != 0 ||
        object.size > std::numeric_limits<std::uintptr_t>::max() - base) {
      return invalid_binding("trusted object layout base is invalid", index);
    }
    if (writable[index] && !object.writable) {
      return invalid_binding("trusted object requires writable storage", index);
    }
  }
  return Status::ok_status();
}

ObjectAccessResult load_trusted_object(
    TrustedObjectSlot slot, std::size_t byte_offset,
    runtime::ExecutionContext* context) noexcept {
  Status status;
  const std::uint8_t* address =
      resolve(slot, byte_offset, false, context, &status);
  if (!status.ok()) {
    return {status, 0};
  }
  Word value = 0;
  std::memcpy(&value, address, sizeof(value));
  return {Status::ok_status(), value};
}

ObjectAccessResult store_trusted_object(
    TrustedObjectSlot slot, std::size_t byte_offset, Word value,
    runtime::ExecutionContext* context) noexcept {
  Status status;
  std::uint8_t* address = resolve(slot, byte_offset, true, context, &status);
  if (!status.ok()) {
    return {status, 0};
  }
  std::memcpy(address, &value, sizeof(value));
  return {Status::ok_status(), value};
}

}  // namespace unijit::ir::detail
