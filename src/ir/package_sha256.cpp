#include "ir/package_sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace unijit::ir::detail {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)};

constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     unsigned amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

std::uint32_t load_big_endian(const std::uint8_t* bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

void compress(const std::uint8_t* block,
              std::array<std::uint32_t, 8>* state) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = load_big_endian(block + index * 4U);
  }
  for (std::size_t index = 16; index < schedule.size(); ++index) {
    const std::uint32_t previous_15 = schedule[index - 15U];
    const std::uint32_t previous_2 = schedule[index - 2U];
    const std::uint32_t sigma0 = rotate_right(previous_15, 7U) ^
                                 rotate_right(previous_15, 18U) ^
                                 (previous_15 >> 3U);
    const std::uint32_t sigma1 = rotate_right(previous_2, 17U) ^
                                 rotate_right(previous_2, 19U) ^
                                 (previous_2 >> 10U);
    schedule[index] = schedule[index - 16U] + sigma0 +
                      schedule[index - 7U] + sigma1;
  }

  std::uint32_t a = (*state)[0];
  std::uint32_t b = (*state)[1];
  std::uint32_t c = (*state)[2];
  std::uint32_t d = (*state)[3];
  std::uint32_t e = (*state)[4];
  std::uint32_t f = (*state)[5];
  std::uint32_t g = (*state)[6];
  std::uint32_t h = (*state)[7];

  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const std::uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                               rotate_right(e, 25U);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temporary1 =
        h + sum1 + choice + kRoundConstants[index] + schedule[index];
    const std::uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                               rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  (*state)[0] += a;
  (*state)[1] += b;
  (*state)[2] += c;
  (*state)[3] += d;
  (*state)[4] += e;
  (*state)[5] += f;
  (*state)[6] += g;
  (*state)[7] += h;
}

}  // namespace

std::array<std::uint8_t, 32> package_sha256(const std::uint8_t* bytes,
                                            std::size_t byte_count) noexcept {
  std::array<std::uint32_t, 8> state = {
      UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372),
      UINT32_C(0xa54ff53a), UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
      UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)};

  const std::size_t complete_blocks = byte_count / 64U;
  for (std::size_t index = 0; index < complete_blocks; ++index) {
    compress(bytes + index * 64U, &state);
  }

  std::array<std::uint8_t, 128> tail{};
  const std::size_t remaining = byte_count % 64U;
  if (remaining != 0) {
    std::copy_n(bytes + complete_blocks * 64U, remaining, tail.data());
  }
  tail[remaining] = UINT8_C(0x80);
  const std::size_t padded_bytes = remaining < 56U ? 64U : 128U;
  const std::uint64_t bit_count = static_cast<std::uint64_t>(byte_count) * 8U;
  for (std::size_t index = 0; index < 8U; ++index) {
    tail[padded_bytes - 1U - index] =
        static_cast<std::uint8_t>(bit_count >> (index * 8U));
  }
  compress(tail.data(), &state);
  if (padded_bytes == 128U) {
    compress(tail.data() + 64U, &state);
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t word = 0; word < state.size(); ++word) {
    for (std::size_t byte = 0; byte < 4U; ++byte) {
      digest[word * 4U + byte] = static_cast<std::uint8_t>(
          state[word] >> ((3U - byte) * 8U));
    }
  }
  return digest;
}

}  // namespace unijit::ir::detail
