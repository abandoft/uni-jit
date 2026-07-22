#ifndef UNIJIT_IR_PACKAGE_H
#define UNIJIT_IR_PACKAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::ir {

inline constexpr std::uint16_t kPortableIrFormatVersion = 1;
inline constexpr std::size_t kPortableIrDigestSize = 32;

enum class PortableIrKind : std::uint8_t {
  kFunction = 1,
  kControlFlow = 2,
};

struct PortableIrLimits final {
  std::size_t maximum_package_bytes{64U * 1024U * 1024U};
  std::size_t maximum_parameters{256};
  std::size_t maximum_nodes{64U * 1024U};
  std::size_t maximum_blocks{1024};
  std::size_t maximum_call_arguments{256U * 1024U};
  std::size_t maximum_edge_arguments{256U * 1024U};
  std::size_t maximum_fast_calls{64};
  std::size_t maximum_fast_call_signature_values{16U * 1024U};
  std::size_t maximum_memory_regions{64};
  std::size_t maximum_memory_accesses{64U * 1024U};
  std::size_t maximum_atomic_accesses{64U * 1024U};
  std::size_t maximum_frame_slots{256};
  std::size_t maximum_trusted_objects{64};
  std::size_t maximum_patch_cells{256};
  std::size_t maximum_vector_constants{64U * 1024U};
  std::size_t maximum_vector_shuffles{64U * 1024U};
  std::size_t maximum_vector_select_arguments{64U * 1024U};
};

struct PortableIrMetadata final {
  PortableIrKind kind{PortableIrKind::kFunction};
  std::uint16_t format_version{0};
  std::size_t package_bytes{0};
  std::size_t payload_bytes{0};
  std::size_t parameter_count{0};
  std::size_t node_count{0};
  std::size_t block_count{0};
  std::size_t call_argument_count{0};
  std::size_t edge_argument_count{0};
  std::array<std::uint8_t, kPortableIrDigestSize> payload_sha256{};
};

struct PortableIrInspectResult final {
  Status status;
  PortableIrMetadata metadata;

  bool ok() const noexcept { return status.ok(); }
};

struct PortableIrEncodeResult final {
  Status status;
  PortableIrMetadata metadata;
  std::vector<std::uint8_t> bytes;

  bool ok() const noexcept { return status.ok() && !bytes.empty(); }
};

struct PortableFunctionDecodeResult final {
  Status status;
  PortableIrMetadata metadata;
  Function function;

  bool ok() const noexcept { return status.ok(); }
};

struct PortableControlFlowDecodeResult final {
  Status status;
  PortableIrMetadata metadata;
  ControlFlowFunction function;

  bool ok() const noexcept { return status.ok(); }
};

PortableIrEncodeResult encode_portable_ir(
    const Function& function,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept;

PortableIrEncodeResult encode_portable_ir(
    const ControlFlowFunction& function,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept;

PortableIrInspectResult inspect_portable_ir(
    const std::uint8_t* bytes, std::size_t byte_count,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept;

inline PortableIrInspectResult inspect_portable_ir(
    const std::vector<std::uint8_t>& bytes,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept {
  return inspect_portable_ir(bytes.data(), bytes.size(), limits);
}

PortableFunctionDecodeResult decode_portable_function(
    const std::uint8_t* bytes, std::size_t byte_count,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept;

inline PortableFunctionDecodeResult decode_portable_function(
    const std::vector<std::uint8_t>& bytes,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept {
  return decode_portable_function(bytes.data(), bytes.size(), limits);
}

PortableControlFlowDecodeResult decode_portable_control_flow(
    const std::uint8_t* bytes, std::size_t byte_count,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept;

inline PortableControlFlowDecodeResult decode_portable_control_flow(
    const std::vector<std::uint8_t>& bytes,
    const PortableIrLimits& limits = PortableIrLimits{}) noexcept {
  return decode_portable_control_flow(bytes.data(), bytes.size(), limits);
}

std::string portable_ir_digest_hex(
    const std::array<std::uint8_t, kPortableIrDigestSize>& digest);

}  // namespace unijit::ir

#endif  // UNIJIT_IR_PACKAGE_H
