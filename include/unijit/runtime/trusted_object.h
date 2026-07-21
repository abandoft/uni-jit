#ifndef UNIJIT_RUNTIME_TRUSTED_OBJECT_H
#define UNIJIT_RUNTIME_TRUSTED_OBJECT_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace unijit::runtime {

struct TrustedObject final {
  void* data{nullptr};
  std::size_t size{0};
  std::uint64_t layout_identity{0};
  bool writable{false};

  static constexpr std::size_t data_offset() noexcept {
    return offsetof(TrustedObject, data);
  }
};

static_assert(std::is_standard_layout<TrustedObject>::value,
              "trusted-object offsets are part of the native ABI");
static_assert(sizeof(void*) != sizeof(std::uint64_t) ||
                  sizeof(TrustedObject) == 32,
              "64-bit trusted-object bindings have a fixed native stride");

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_TRUSTED_OBJECT_H
