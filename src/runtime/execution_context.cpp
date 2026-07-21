#include "unijit/runtime/execution_context.h"

#include <cstdint>
#include <limits>

namespace unijit::runtime {

Status ExecutionContext::bind_memory_regions(const MemoryRegion* regions,
                                             std::size_t count) noexcept {
  if (count != 0 && regions == nullptr) {
    return {StatusCode::kInvalidArgument,
            "memory-region storage is null for a non-empty binding"};
  }
  for (std::size_t index = 0; index < count; ++index) {
    const MemoryRegion& region = regions[index];
    if (region.size != 0 && region.data == nullptr) {
      return {StatusCode::kInvalidArgument,
              "non-empty memory region has a null base", index};
    }
    const auto base = reinterpret_cast<std::uintptr_t>(region.data);
    if (region.size > std::numeric_limits<std::uintptr_t>::max() - base) {
      return {StatusCode::kInvalidArgument,
              "memory region wraps the process address space", index};
    }
  }
  memory_regions_ = regions;
  memory_region_count_ = count;
  return Status::ok_status();
}

Status ExecutionContext::bind_trusted_objects(const TrustedObject* objects,
                                              std::size_t count) noexcept {
  if (count != 0 && objects == nullptr) {
    return {StatusCode::kInvalidArgument,
            "trusted-object storage is null for a non-empty binding"};
  }
  for (std::size_t index = 0; index < count; ++index) {
    const TrustedObject& object = objects[index];
    if (object.layout_identity == 0) {
      return {StatusCode::kInvalidArgument,
              "trusted object has no layout identity", index};
    }
    if (object.size != 0 && object.data == nullptr) {
      return {StatusCode::kInvalidArgument,
              "non-empty trusted object has a null base", index};
    }
    const auto base = reinterpret_cast<std::uintptr_t>(object.data);
    if (object.size > std::numeric_limits<std::uintptr_t>::max() - base) {
      return {StatusCode::kInvalidArgument,
              "trusted object wraps the process address space", index};
    }
  }
  trusted_objects_ = objects;
  trusted_object_count_ = count;
  return Status::ok_status();
}

}  // namespace unijit::runtime
