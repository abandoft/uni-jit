#ifndef UNIJIT_RUNTIME_MEMORY_REGION_H
#define UNIJIT_RUNTIME_MEMORY_REGION_H

#include <cstddef>
#include <cstdint>

namespace unijit::runtime {

struct MemoryRegion final {
  void* data{nullptr};
  std::size_t size{0};
  bool writable{false};

  static constexpr std::size_t data_offset() noexcept {
    return offsetof(MemoryRegion, data);
  }
  static constexpr std::size_t size_offset() noexcept {
    return offsetof(MemoryRegion, size);
  }
  static constexpr std::size_t writable_offset() noexcept {
    return offsetof(MemoryRegion, writable);
  }
};

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_MEMORY_REGION_H
