#ifndef UNIJIT_STATUS_H
#define UNIJIT_STATUS_H

#include <cstddef>
#include <string>
#include <utility>

namespace unijit {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kInvalidIr,
  kUnsupportedArchitecture,
  kResourceExhausted,
  kCodeGenerationFailed,
  kMemoryProtectionFailed,
};

class Status final {
 public:
  Status() noexcept = default;

  Status(StatusCode code, std::string message, std::size_t location = 0)
      : code_(code), message_(std::move(message)), location_(location) {}

  static Status ok_status() noexcept { return {}; }

  bool ok() const noexcept { return code_ == StatusCode::kOk; }
  explicit operator bool() const noexcept { return ok(); }

  StatusCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  std::size_t location() const noexcept { return location_; }

 private:
  StatusCode code_{StatusCode::kOk};
  std::string message_;
  std::size_t location_{0};
};

}  // namespace unijit

#endif  // UNIJIT_STATUS_H
