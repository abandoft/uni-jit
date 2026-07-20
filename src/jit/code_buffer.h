#ifndef UNIJIT_SRC_JIT_CODE_BUFFER_H
#define UNIJIT_SRC_JIT_CODE_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace unijit::jit::detail {

class CodeBuffer final {
 public:
  void emit_u8(std::uint8_t byte) { bytes_.push_back(byte); }

  void emit_u32(std::uint32_t instruction) {
    bytes_.push_back(static_cast<std::uint8_t>(instruction));
    bytes_.push_back(static_cast<std::uint8_t>(instruction >> 8U));
    bytes_.push_back(static_cast<std::uint8_t>(instruction >> 16U));
    bytes_.push_back(static_cast<std::uint8_t>(instruction >> 24U));
  }

  void emit_u64(std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
      emit_u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  std::vector<std::uint8_t> take_bytes() noexcept { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_CODE_BUFFER_H
