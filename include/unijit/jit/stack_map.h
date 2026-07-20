#ifndef UNIJIT_JIT_STACK_MAP_H
#define UNIJIT_JIT_STACK_MAP_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::jit {

enum class StackMapKind : std::uint8_t {
  kSafepoint = 0,
  kGuard,
};

struct StackMapValue final {
  ir::Value value;
  ir::ValueType type{ir::ValueType::kWord};
  std::size_t frame_offset{0};
};

struct StackMapRecord final {
  std::size_t site{0};
  std::size_t native_offset{0};
  std::size_t frame_size{0};
  StackMapKind kind{StackMapKind::kSafepoint};
  std::vector<StackMapValue> live_values;

  const StackMapValue* find(ir::Value value) const noexcept {
    for (const StackMapValue& entry : live_values) {
      if (entry.value == value) {
        return &entry;
      }
    }
    return nullptr;
  }
};

class StackMapTable final {
 public:
  StackMapTable() = default;
  explicit StackMapTable(std::vector<StackMapRecord> records) noexcept
      : records_(std::move(records)) {}

  const StackMapRecord* find(std::size_t site) const noexcept {
    for (const StackMapRecord& record : records_) {
      if (record.site == site) {
        return &record;
      }
    }
    return nullptr;
  }

  bool empty() const noexcept { return records_.empty(); }
  std::size_t size() const noexcept { return records_.size(); }
  const std::vector<StackMapRecord>& records() const noexcept {
    return records_;
  }

 private:
  std::vector<StackMapRecord> records_;
};

struct CapturedStackMapValue final {
  ir::Value value;
  ir::ValueType type{ir::ValueType::kWord};
  ir::Word value_bits{0};
};

struct CapturedStackMap final {
  std::size_t site{0};
  StackMapKind kind{StackMapKind::kSafepoint};
  std::vector<CapturedStackMapValue> values;

  const CapturedStackMapValue* find(ir::Value value) const noexcept {
    for (const CapturedStackMapValue& entry : values) {
      if (entry.value == value) {
        return &entry;
      }
    }
    return nullptr;
  }
};

struct StackMapCaptureResult final {
  Status status;
  CapturedStackMap capture;

  bool ok() const noexcept { return status.ok(); }
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_STACK_MAP_H
