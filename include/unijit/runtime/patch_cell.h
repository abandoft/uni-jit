#ifndef UNIJIT_RUNTIME_PATCH_CELL_H
#define UNIJIT_RUNTIME_PATCH_CELL_H

#include <atomic>
#include <cstddef>
#include <type_traits>

#include "unijit/ir/vector.h"

namespace unijit::runtime {

// Internal storage whose address is bound only for the duration of a managed
// compiled invocation. Published code performs an acquire load from value_;
// runtime mutation never changes executable pages.
class alignas(ir::Word) PatchCellStorage final {
 public:
  PatchCellStorage() noexcept = default;

  PatchCellStorage(const PatchCellStorage&) = delete;
  PatchCellStorage& operator=(const PatchCellStorage&) = delete;

  ir::Word load() const noexcept {
    return value_.load(std::memory_order_acquire);
  }

  void publish(ir::Word value) noexcept {
    value_.store(value, std::memory_order_release);
  }

  bool compare_exchange(ir::Word* expected, ir::Word desired) noexcept {
    return value_.compare_exchange_strong(*expected, desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  ir::Word fetch_add(ir::Word increment) noexcept {
    return value_.fetch_add(increment, std::memory_order_acq_rel);
  }

  static constexpr std::size_t value_offset() noexcept {
    return offsetof(PatchCellStorage, value_);
  }

 private:
  std::atomic<ir::Word> value_{0};
};

static_assert(std::atomic<ir::Word>::is_always_lock_free,
              "patch cells require lock-free native Word atomics");
static_assert(std::is_standard_layout<PatchCellStorage>::value,
              "patch-cell offsets are part of the managed native ABI");
static_assert(sizeof(PatchCellStorage) == sizeof(ir::Word),
              "patch-cell native loads require one-Word storage");

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_PATCH_CELL_H
