#ifndef UNIJIT_IR_PACKAGE_SHA256_H
#define UNIJIT_IR_PACKAGE_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace unijit::ir::detail {

std::array<std::uint8_t, 32> package_sha256(const std::uint8_t *bytes,
                                            std::size_t byte_count) noexcept;

} // namespace unijit::ir::detail

#endif // UNIJIT_IR_PACKAGE_SHA256_H
