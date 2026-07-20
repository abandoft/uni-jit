#ifndef UNIJIT_SRC_JIT_EXECUTABLE_MEMORY_H
#define UNIJIT_SRC_JIT_EXECUTABLE_MEMORY_H

#include <cstddef>
#include <cstdint>

#include "unijit/status.h"

namespace unijit::jit::detail {

class ExecutableMemory final {
 public:
  ExecutableMemory() noexcept = default;
  ~ExecutableMemory();

  ExecutableMemory(ExecutableMemory&& other) noexcept;
  ExecutableMemory& operator=(ExecutableMemory&& other) noexcept;

  ExecutableMemory(const ExecutableMemory&) = delete;
  ExecutableMemory& operator=(const ExecutableMemory&) = delete;

  static Status publish(const std::uint8_t* code, std::size_t code_size,
                        ExecutableMemory* output);

  void* address() const noexcept { return address_; }
  std::size_t size() const noexcept { return code_size_; }
  std::size_t mapping_size() const noexcept { return mapping_size_; }

 private:
  void release() noexcept;

  void* mapping_address_{nullptr};
  void* address_{nullptr};
  std::size_t mapping_size_{0};
  std::size_t code_size_{0};
};

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_EXECUTABLE_MEMORY_H
