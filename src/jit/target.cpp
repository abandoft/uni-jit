#include "unijit/jit/target.h"

#include <cstddef>
#include <cstdint>

#if defined(UNIJIT_TARGET_AARCH64) && defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(UNIJIT_TARGET_AARCH64) && defined(__linux__)
#include <sys/auxv.h>
#endif

#if defined(UNIJIT_TARGET_X86_64) && defined(_MSC_VER)
#include <intrin.h>
#elif defined(UNIJIT_TARGET_X86_64)
#include <cpuid.h>
#endif

#if defined(UNIJIT_TARGET_RISCV64) && defined(__linux__)
#include <sys/auxv.h>
#endif

namespace unijit::jit {
namespace {

constexpr std::uint64_t kAArch64Features =
    target_feature_bit(TargetFeature::kFp64) |
    target_feature_bit(TargetFeature::kNeon);
constexpr std::uint64_t kX86Features =
    target_feature_bit(TargetFeature::kFp64) |
    target_feature_bit(TargetFeature::kSse2);
constexpr std::uint64_t kRiscVFeatures =
    target_feature_bit(TargetFeature::kFp64) |
    target_feature_bit(TargetFeature::kRiscVIntegerMultiply) |
    target_feature_bit(TargetFeature::kRiscVFloat64);

constexpr std::uint64_t kAArch64Allowed =
    kAArch64Features | target_feature_bit(TargetFeature::kAarch64Lse);
constexpr std::uint64_t kX86Allowed =
    kX86Features | target_feature_bit(TargetFeature::kAvx) |
    target_feature_bit(TargetFeature::kAvx2) |
    target_feature_bit(TargetFeature::kFma);
constexpr std::uint64_t kRiscVAllowed =
    kRiscVFeatures | target_feature_bit(TargetFeature::kRiscVVector) |
    target_feature_bit(TargetFeature::kRiscVAtomic);

void discover_aarch64_features(TargetProfile* profile) noexcept {
#if defined(UNIJIT_TARGET_AARCH64) && defined(__APPLE__)
  int supported = 0;
  std::size_t size = sizeof(supported);
  if (sysctlbyname("hw.optional.arm.FEAT_LSE", &supported, &size, nullptr, 0) ==
          0 &&
      supported != 0) {
    profile->features |= target_feature_bit(TargetFeature::kAarch64Lse);
  }
#elif defined(UNIJIT_TARGET_AARCH64) && defined(__linux__) && defined(AT_HWCAP)
  constexpr unsigned long kAtomics = 1UL << 8U;
  if ((getauxval(AT_HWCAP) & kAtomics) != 0) {
    profile->features |= target_feature_bit(TargetFeature::kAarch64Lse);
  }
#else
  (void)profile;
#endif
}

#if defined(UNIJIT_TARGET_X86_64)
std::uint64_t xgetbv0() noexcept {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
  return (static_cast<std::uint64_t>(high) << 32U) | low;
#endif
}
#endif

void discover_x86_features(TargetProfile* profile) noexcept {
#if defined(UNIJIT_TARGET_X86_64)
  std::uint32_t leaf1_ecx = 0;
  std::uint32_t leaf1_edx = 0;
  std::uint32_t maximum_leaf = 0;
#if defined(_MSC_VER)
  int registers[4] = {};
  __cpuidex(registers, 0, 0);
  maximum_leaf = static_cast<std::uint32_t>(registers[0]);
  if (maximum_leaf >= 1) {
    __cpuidex(registers, 1, 0);
    leaf1_ecx = static_cast<std::uint32_t>(registers[2]);
    leaf1_edx = static_cast<std::uint32_t>(registers[3]);
  }
#else
  maximum_leaf = __get_cpuid_max(0, nullptr);
  std::uint32_t leaf1_eax = 0;
  std::uint32_t leaf1_ebx = 0;
  if (maximum_leaf >= 1) {
    __cpuid(1, leaf1_eax, leaf1_ebx, leaf1_ecx, leaf1_edx);
  }
#endif
  if ((leaf1_edx & (UINT32_C(1) << 26U)) != 0) {
    profile->features |= target_feature_bit(TargetFeature::kSse2);
  }
  const bool cpu_avx = (leaf1_ecx & (UINT32_C(1) << 28U)) != 0;
  const bool osxsave = (leaf1_ecx & (UINT32_C(1) << 27U)) != 0;
  const bool avx_state = osxsave && (xgetbv0() & UINT64_C(0x6)) == 0x6;
  if (cpu_avx && avx_state) {
    profile->features |= target_feature_bit(TargetFeature::kAvx);
    profile->vector_width_policy = VectorWidthPolicy::kNative;
    profile->maximum_vector_bits = 256;
    if ((leaf1_ecx & (UINT32_C(1) << 12U)) != 0) {
      profile->features |= target_feature_bit(TargetFeature::kFma);
    }
  }
  if (maximum_leaf >= 7 && cpu_avx && avx_state) {
    std::uint32_t leaf7_ebx = 0;
#if defined(_MSC_VER)
    __cpuidex(registers, 7, 0);
    leaf7_ebx = static_cast<std::uint32_t>(registers[1]);
#else
    std::uint32_t leaf7_eax = 0;
    std::uint32_t leaf7_ecx = 0;
    std::uint32_t leaf7_edx = 0;
    __cpuid_count(7, 0, leaf7_eax, leaf7_ebx, leaf7_ecx, leaf7_edx);
#endif
    if ((leaf7_ebx & (UINT32_C(1) << 5U)) != 0) {
      profile->features |= target_feature_bit(TargetFeature::kAvx2);
    }
  }
#else
  (void)profile;
#endif
}

void discover_riscv_features(TargetProfile* profile) noexcept {
#if defined(UNIJIT_TARGET_RISCV64) && defined(__linux__) && defined(AT_HWCAP)
  // Linux RISC-V maps single-letter ISA extensions A..Z to HWCAP bits 0..25.
  const unsigned long capabilities = getauxval(AT_HWCAP);
  constexpr unsigned long kAtomic = 1UL << ('A' - 'A');
  constexpr unsigned long kVector = 1UL << ('V' - 'A');
  if ((capabilities & kAtomic) != 0) {
    profile->features |= target_feature_bit(TargetFeature::kRiscVAtomic);
  }
  if ((capabilities & kVector) != 0) {
    profile->features |= target_feature_bit(TargetFeature::kRiscVVector);
  }
#else
  (void)profile;
#endif
}

bool abi_matches_architecture(TargetArchitecture architecture,
                              TargetAbi abi) noexcept {
  switch (architecture) {
    case TargetArchitecture::kAArch64:
      return abi == TargetAbi::kAapcs64;
    case TargetArchitecture::kX86_64:
      return abi == TargetAbi::kSystemV || abi == TargetAbi::kWindowsX64;
    case TargetArchitecture::kRiscV64:
      return abi == TargetAbi::kRiscVElf;
    case TargetArchitecture::kUnknown:
      return false;
  }
  return false;
}

std::uint64_t allowed_features(TargetArchitecture architecture) noexcept {
  switch (architecture) {
    case TargetArchitecture::kAArch64:
      return kAArch64Allowed;
    case TargetArchitecture::kX86_64:
      return kX86Allowed;
    case TargetArchitecture::kRiscV64:
      return kRiscVAllowed;
    case TargetArchitecture::kUnknown:
      return 0;
  }
  return 0;
}

std::uint64_t required_features(TargetArchitecture architecture) noexcept {
  switch (architecture) {
    case TargetArchitecture::kAArch64:
      return kAArch64Features;
    case TargetArchitecture::kX86_64:
      return kX86Features;
    case TargetArchitecture::kRiscV64:
      return kRiscVFeatures;
    case TargetArchitecture::kUnknown:
      return 0;
  }
  return 0;
}

void hash_byte(std::uint64_t* hash, std::uint8_t value) noexcept {
  *hash ^= value;
  *hash *= UINT64_C(1099511628211);
}

}  // namespace

TargetProfile baseline_target_profile() noexcept {
  TargetProfile profile;
  profile.endianness = TargetEndianness::kLittle;
  profile.vector_width_policy = VectorWidthPolicy::kPortable128;
  profile.maximum_vector_bits = 128;
#if defined(UNIJIT_TARGET_AARCH64)
  profile.architecture = TargetArchitecture::kAArch64;
  profile.abi = TargetAbi::kAapcs64;
  profile.features = kAArch64Features;
#elif defined(UNIJIT_TARGET_X86_64)
  profile.architecture = TargetArchitecture::kX86_64;
#if defined(_WIN32)
  profile.abi = TargetAbi::kWindowsX64;
#else
  profile.abi = TargetAbi::kSystemV;
#endif
  profile.features = kX86Features;
#elif defined(UNIJIT_TARGET_RISCV64)
  profile.architecture = TargetArchitecture::kRiscV64;
  profile.abi = TargetAbi::kRiscVElf;
  profile.features = kRiscVFeatures;
#else
  profile.endianness = TargetEndianness::kUnknown;
  profile.maximum_vector_bits = 0;
#endif
  return profile;
}

TargetProfile host_target_profile() noexcept {
  TargetProfile profile = baseline_target_profile();
  if (profile.architecture == TargetArchitecture::kAArch64) {
    discover_aarch64_features(&profile);
  } else if (profile.architecture == TargetArchitecture::kX86_64) {
    discover_x86_features(&profile);
  } else if (profile.architecture == TargetArchitecture::kRiscV64) {
    discover_riscv_features(&profile);
  }
  return profile;
}

Status validate_target_profile(const TargetProfile& profile) {
  if (!abi_matches_architecture(profile.architecture, profile.abi)) {
    return {StatusCode::kInvalidArgument,
            "target architecture and ABI are inconsistent"};
  }
  if (profile.endianness != TargetEndianness::kLittle) {
    return {StatusCode::kInvalidArgument,
            "current native backends require little-endian targets"};
  }
  const std::uint64_t allowed = allowed_features(profile.architecture);
  const std::uint64_t required = required_features(profile.architecture);
  if ((profile.features & ~allowed) != 0) {
    return {StatusCode::kInvalidArgument,
            "target profile contains features from another architecture"};
  }
  if ((profile.features & required) != required) {
    return {StatusCode::kInvalidArgument,
            "target profile omits a required backend baseline feature"};
  }
  if (has_target_feature(profile, TargetFeature::kAvx2) &&
      !has_target_feature(profile, TargetFeature::kAvx)) {
    return {StatusCode::kInvalidArgument,
            "AVX2 requires the AVX target feature"};
  }
  if (has_target_feature(profile, TargetFeature::kFma) &&
      !has_target_feature(profile, TargetFeature::kAvx)) {
    return {StatusCode::kInvalidArgument,
            "x86 FMA requires the AVX target feature"};
  }
  if (profile.maximum_vector_bits < 128 ||
      (profile.maximum_vector_bits % 128) != 0) {
    return {StatusCode::kInvalidArgument,
            "target vector width must be a positive multiple of 128 bits"};
  }
  if (profile.vector_width_policy == VectorWidthPolicy::kPortable128 &&
      profile.maximum_vector_bits != 128) {
    return {StatusCode::kInvalidArgument,
            "portable vector policy requires an exact 128-bit width"};
  }
  if (profile.vector_width_policy != VectorWidthPolicy::kPortable128 &&
      profile.vector_width_policy != VectorWidthPolicy::kNative) {
    return {StatusCode::kInvalidArgument,
            "target vector width policy is unknown"};
  }
  return Status::ok_status();
}

bool target_profile_contains(const TargetProfile& available,
                             const TargetProfile& required) noexcept {
  return available.architecture == required.architecture &&
         available.abi == required.abi &&
         available.endianness == required.endianness &&
         (available.features & required.features) == required.features &&
         available.maximum_vector_bits >= required.maximum_vector_bits;
}

bool target_profiles_equal(const TargetProfile& lhs,
                           const TargetProfile& rhs) noexcept {
  return lhs.architecture == rhs.architecture && lhs.abi == rhs.abi &&
         lhs.endianness == rhs.endianness && lhs.features == rhs.features &&
         lhs.vector_width_policy == rhs.vector_width_policy &&
         lhs.maximum_vector_bits == rhs.maximum_vector_bits;
}

std::uint64_t target_profile_key(const TargetProfile& profile) noexcept {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  hash_byte(&hash, static_cast<std::uint8_t>(profile.architecture));
  hash_byte(&hash, static_cast<std::uint8_t>(profile.abi));
  hash_byte(&hash, static_cast<std::uint8_t>(profile.endianness));
  for (unsigned shift = 0; shift < 64; shift += 8) {
    hash_byte(&hash,
              static_cast<std::uint8_t>(profile.features >> shift));
  }
  hash_byte(&hash, static_cast<std::uint8_t>(profile.vector_width_policy));
  hash_byte(&hash, static_cast<std::uint8_t>(profile.maximum_vector_bits));
  hash_byte(&hash,
            static_cast<std::uint8_t>(profile.maximum_vector_bits >> 8U));
  return hash;
}

const char* target_architecture_name(
    TargetArchitecture architecture) noexcept {
  switch (architecture) {
    case TargetArchitecture::kAArch64:
      return "aarch64";
    case TargetArchitecture::kX86_64:
      return "x86-64";
    case TargetArchitecture::kRiscV64:
      return "riscv64";
    case TargetArchitecture::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* target_abi_name(TargetAbi abi) noexcept {
  switch (abi) {
    case TargetAbi::kAapcs64:
      return "aapcs64";
    case TargetAbi::kSystemV:
      return "system-v";
    case TargetAbi::kWindowsX64:
      return "windows-x64";
    case TargetAbi::kRiscVElf:
      return "riscv-elf";
    case TargetAbi::kUnknown:
      return "unknown";
  }
  return "unknown";
}

}  // namespace unijit::jit
