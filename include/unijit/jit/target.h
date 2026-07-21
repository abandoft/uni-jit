#ifndef UNIJIT_JIT_TARGET_H
#define UNIJIT_JIT_TARGET_H

#include <cstdint>

#include "unijit/status.h"

namespace unijit::jit {

enum class TargetArchitecture : std::uint8_t {
  kUnknown = 0,
  kAArch64,
  kX86_64,
  kRiscV64,
};

enum class TargetAbi : std::uint8_t {
  kUnknown = 0,
  kAapcs64,
  kSystemV,
  kWindowsX64,
  kRiscVElf,
};

enum class TargetEndianness : std::uint8_t {
  kUnknown = 0,
  kLittle,
  kBig,
};

enum class VectorWidthPolicy : std::uint8_t {
  kPortable128 = 0,
  kNative,
};

enum class TargetFeature : std::uint64_t {
  kFp64 = UINT64_C(1) << 0U,
  kNeon = UINT64_C(1) << 1U,
  kSse2 = UINT64_C(1) << 2U,
  kAvx = UINT64_C(1) << 3U,
  kAvx2 = UINT64_C(1) << 4U,
  kFma = UINT64_C(1) << 5U,
  kRiscVIntegerMultiply = UINT64_C(1) << 6U,
  kRiscVFloat64 = UINT64_C(1) << 7U,
  kRiscVVector = UINT64_C(1) << 8U,
  kAarch64Lse = UINT64_C(1) << 9U,
};

constexpr std::uint64_t target_feature_bit(TargetFeature feature) noexcept {
  return static_cast<std::uint64_t>(feature);
}

struct TargetProfile final {
  TargetArchitecture architecture{TargetArchitecture::kUnknown};
  TargetAbi abi{TargetAbi::kUnknown};
  TargetEndianness endianness{TargetEndianness::kUnknown};
  std::uint64_t features{0};
  VectorWidthPolicy vector_width_policy{VectorWidthPolicy::kPortable128};
  std::uint16_t maximum_vector_bits{128};
};

constexpr bool has_target_feature(const TargetProfile& profile,
                                  TargetFeature feature) noexcept {
  return (profile.features & target_feature_bit(feature)) != 0;
}

TargetProfile baseline_target_profile() noexcept;
TargetProfile host_target_profile() noexcept;

Status validate_target_profile(const TargetProfile& profile);

bool target_profile_contains(const TargetProfile& available,
                             const TargetProfile& required) noexcept;
bool target_profiles_equal(const TargetProfile& lhs,
                           const TargetProfile& rhs) noexcept;

// Stable across processes and suitable for cache/artifact identity. This is
// not a cryptographic hash and must not be used as a trust decision.
std::uint64_t target_profile_key(const TargetProfile& profile) noexcept;

const char* target_architecture_name(TargetArchitecture architecture) noexcept;
const char* target_abi_name(TargetAbi abi) noexcept;

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_TARGET_H
