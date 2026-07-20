#ifndef UNIJIT_RUNTIME_ON_STACK_REPLACEMENT_H
#define UNIJIT_RUNTIME_ON_STACK_REPLACEMENT_H

#include <array>
#include <cstddef>
#include <vector>

#include "unijit/ir/interpreter.h"

namespace unijit::runtime {

struct OsrValue final {
  std::size_t slot{0};
  ir::ValueType type{ir::ValueType::kWord};
  ir::Word value{0};
};

class OsrFrame final {
 public:
  static constexpr std::size_t kMaximumValues = 64;

  OsrFrame(std::size_t entry_site, std::size_t resume_offset) noexcept
      : entry_site_(entry_site), resume_offset_(resume_offset) {}

  Status add(std::size_t slot, ir::ValueType type, ir::Word value);

  std::size_t entry_site() const noexcept { return entry_site_; }
  std::size_t resume_offset() const noexcept { return resume_offset_; }
  std::size_t size() const noexcept { return values_.size(); }
  const OsrValue* find(std::size_t slot) const noexcept;
  const std::vector<OsrValue>& values() const noexcept { return values_; }

 private:
  std::size_t entry_site_{0};
  std::size_t resume_offset_{0};
  std::vector<OsrValue> values_;
};

struct OsrBinding final {
  std::size_t slot{0};
  ir::ValueType type{ir::ValueType::kWord};
};

struct OsrArguments final {
  static constexpr std::size_t kMaximumArguments = OsrFrame::kMaximumValues;

  Status status;
  std::size_t entry_site{0};
  std::size_t resume_offset{0};
  std::size_t count{0};
  std::array<ir::Word, kMaximumArguments> values{};

  bool ok() const noexcept { return status.ok(); }
  const ir::Word* data() const noexcept {
    return count == 0 ? nullptr : values.data();
  }
};

class OsrEntryPlan final {
 public:
  static constexpr std::size_t kMaximumArguments =
      OsrArguments::kMaximumArguments;

  OsrEntryPlan(std::size_t entry_site, std::size_t resume_offset) noexcept
      : entry_site_(entry_site), resume_offset_(resume_offset) {}

  Status add_argument(std::size_t slot, ir::ValueType type);
  OsrArguments marshal(
      const OsrFrame& frame,
      const std::vector<ir::ValueType>& parameter_types) const;

  std::size_t entry_site() const noexcept { return entry_site_; }
  std::size_t resume_offset() const noexcept { return resume_offset_; }
  std::size_t size() const noexcept { return bindings_.size(); }
  const std::vector<OsrBinding>& bindings() const noexcept {
    return bindings_;
  }

 private:
  std::size_t entry_site_{0};
  std::size_t resume_offset_{0};
  std::vector<OsrBinding> bindings_;
};

struct OsrEntryResult final {
  OsrArguments arguments;
  ir::EvaluationResult result;

  bool entered() const noexcept { return arguments.ok(); }
  bool ok() const noexcept { return entered() && result.ok(); }
};

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_ON_STACK_REPLACEMENT_H
