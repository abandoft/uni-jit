#include "unijit/ir/package.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ir/package_sha256.h"

namespace unijit::ir {

namespace detail {

struct PortableIrAccess final {
  static void
  publish_function(Function *function, std::vector<ValueType> parameter_types,
                   std::vector<Node> nodes, std::vector<Value> call_arguments,
                   std::vector<FastCallDescriptor> fast_calls,
                   std::size_t memory_region_count,
                   std::vector<MemoryAccessDescriptor> memory_accesses,
                   std::vector<AtomicAccessDescriptor> atomic_accesses,
                   std::vector<FrameSlotDescriptor> frame_slots,
                   std::vector<TrustedObjectDescriptor> trusted_objects,
                   std::vector<PatchCellDescriptor> patch_cells,
                   std::vector<Vector128> vector_constants,
                   std::vector<VectorShuffle> vector_shuffles,
                   std::vector<Value> vector_select_arguments,
                   Value return_value) {
    function->parameter_count_ = parameter_types.size();
    function->parameter_types_ = std::move(parameter_types);
    function->nodes_ = std::move(nodes);
    function->call_arguments_ = std::move(call_arguments);
    function->fast_calls_ = std::move(fast_calls);
    function->memory_region_count_ = memory_region_count;
    function->memory_accesses_ = std::move(memory_accesses);
    function->atomic_accesses_ = std::move(atomic_accesses);
    function->frame_slots_ = std::move(frame_slots);
    function->trusted_objects_ = std::move(trusted_objects);
    function->patch_cells_ = std::move(patch_cells);
    function->vector_constants_ = std::move(vector_constants);
    function->vector_shuffles_ = std::move(vector_shuffles);
    function->vector_select_arguments_ = std::move(vector_select_arguments);
    function->return_value_ = return_value;
  }

  static void
  publish_control_flow(ControlFlowFunction *function,
                       std::vector<ValueType> parameter_types,
                       Block entry_block, std::vector<ControlNode> nodes,
                       std::vector<Value> call_arguments,
                       std::vector<FastCallDescriptor> fast_calls,
                       std::size_t memory_region_count,
                       std::vector<MemoryAccessDescriptor> memory_accesses,
                       std::vector<AtomicAccessDescriptor> atomic_accesses,
                       std::vector<FrameSlotDescriptor> frame_slots,
                       std::vector<TrustedObjectDescriptor> trusted_objects,
                       std::vector<PatchCellDescriptor> patch_cells,
                       std::vector<Vector128> vector_constants,
                       std::vector<VectorShuffle> vector_shuffles,
                       std::vector<Value> vector_select_arguments,
                       std::vector<BasicBlock> blocks) {
    function->parameter_count_ = parameter_types.size();
    function->parameter_types_ = std::move(parameter_types);
    function->entry_block_ = entry_block;
    function->nodes_ = std::move(nodes);
    function->call_arguments_ = std::move(call_arguments);
    function->fast_calls_ = std::move(fast_calls);
    function->memory_region_count_ = memory_region_count;
    function->memory_accesses_ = std::move(memory_accesses);
    function->atomic_accesses_ = std::move(atomic_accesses);
    function->frame_slots_ = std::move(frame_slots);
    function->trusted_objects_ = std::move(trusted_objects);
    function->patch_cells_ = std::move(patch_cells);
    function->vector_constants_ = std::move(vector_constants);
    function->vector_shuffles_ = std::move(vector_shuffles);
    function->vector_select_arguments_ = std::move(vector_select_arguments);
    function->blocks_ = std::move(blocks);
  }
};

} // namespace detail

namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {
    'U', 'N', 'I', 'J', 'I', 'R', UINT8_C(0), UINT8_C(1)};
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kManifestWords = 24;
constexpr std::size_t kManifestSize = kManifestWords * sizeof(std::uint32_t);
constexpr std::size_t kNodeRecordSize = 48;
constexpr std::size_t kFastCallRecordSize = 8;
constexpr std::size_t kMemoryRecordSize = 12;
constexpr std::size_t kAtomicRecordSize = 16;
constexpr std::size_t kFrameRecordSize = 4;
constexpr std::size_t kTrustedObjectRecordSize = 16;
constexpr std::size_t kPatchCellRecordSize = 16;
constexpr std::size_t kVectorConstantRecordSize = 16;
constexpr std::size_t kVectorShuffleRecordSize = 24;
constexpr std::size_t kBlockRecordSize = 32;
constexpr std::uint32_t kSchemaRevision = 1;
constexpr std::uint32_t kInvalidIndex =
    std::numeric_limits<std::uint32_t>::max();

enum ManifestIndex : std::size_t {
  kSchema = 0,
  kParameters,
  kNodes,
  kCallArguments,
  kFastCalls,
  kFastCallSignatureValues,
  kMemoryRegions,
  kMemoryAccesses,
  kAtomicAccesses,
  kFrameSlots,
  kTrustedObjects,
  kPatchCells,
  kVectorConstants,
  kVectorShuffles,
  kVectorSelectArguments,
  kBlocks,
  kBlockInstructionReferences,
  kEdgeArguments,
  kReturnValue,
  kEntryBlock,
  kFirstReserved,
};

static_assert(static_cast<std::uint8_t>(Opcode::kParameter) == 0,
              "portable straight-line opcode base changed");
static_assert(static_cast<std::uint8_t>(Opcode::kVectorWiden) == 62,
              "portable straight-line opcode table changed");
static_assert(static_cast<std::uint8_t>(ControlOpcode::kParameter) == 0,
              "portable CFG opcode base changed");
static_assert(static_cast<std::uint8_t>(ControlOpcode::kVectorWiden) == 63,
              "portable CFG opcode table changed");
static_assert(static_cast<std::uint8_t>(ValueType::kWord) == 0,
              "portable value-type base changed");
static_assert(static_cast<std::uint8_t>(ValueType::kMask64x2) == 11,
              "portable value-type table changed");
static_assert(static_cast<std::uint8_t>(TerminatorOpcode::kBranch) == 3,
              "portable terminator table changed");

Status invalid_package(const char *message, std::size_t location = 0) {
  return {StatusCode::kInvalidIr, message, location};
}

Status exhausted_package(const char *message) {
  return {StatusCode::kResourceExhausted, message};
}

class Writer final {
public:
  explicit Writer(std::size_t reserve_bytes = 0) {
    bytes_.reserve(reserve_bytes);
  }

  void u8(std::uint8_t value) { bytes_.push_back(value); }

  void u16(std::uint16_t value) {
    for (unsigned shift = 0; shift < 16U; shift += 8U) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void raw(const std::uint8_t *bytes, std::size_t byte_count) {
    bytes_.insert(bytes_.end(), bytes, bytes + byte_count);
  }

  const std::vector<std::uint8_t> &bytes() const noexcept { return bytes_; }
  std::vector<std::uint8_t> take() && noexcept { return std::move(bytes_); }

private:
  std::vector<std::uint8_t> bytes_;
};

class Reader final {
public:
  Reader(const std::uint8_t *bytes, std::size_t byte_count) noexcept
      : bytes_(bytes), byte_count_(byte_count) {}

  bool u8(std::uint8_t *value) noexcept {
    if (remaining() < 1U) {
      return false;
    }
    *value = bytes_[offset_++];
    return true;
  }

  bool u16(std::uint16_t *value) noexcept {
    std::uint64_t decoded = 0;
    if (!integer(2U, &decoded)) {
      return false;
    }
    *value = static_cast<std::uint16_t>(decoded);
    return true;
  }

  bool u32(std::uint32_t *value) noexcept {
    std::uint64_t decoded = 0;
    if (!integer(4U, &decoded)) {
      return false;
    }
    *value = static_cast<std::uint32_t>(decoded);
    return true;
  }

  bool u64(std::uint64_t *value) noexcept { return integer(8U, value); }

  bool raw(std::uint8_t *destination, std::size_t byte_count) noexcept {
    if (byte_count > remaining()) {
      return false;
    }
    if (byte_count != 0) {
      std::memcpy(destination, bytes_ + offset_, byte_count);
    }
    offset_ += byte_count;
    return true;
  }

  std::size_t offset() const noexcept { return offset_; }
  std::size_t remaining() const noexcept { return byte_count_ - offset_; }

private:
  bool integer(std::size_t byte_count, std::uint64_t *value) noexcept {
    if (byte_count > remaining()) {
      return false;
    }
    std::uint64_t decoded = 0;
    for (std::size_t index = 0; index < byte_count; ++index) {
      decoded |= static_cast<std::uint64_t>(bytes_[offset_ + index])
                 << (index * 8U);
    }
    offset_ += byte_count;
    *value = decoded;
    return true;
  }

  const std::uint8_t *bytes_{nullptr};
  std::size_t byte_count_{0};
  std::size_t offset_{0};
};

std::uint64_t word_bits(Word value) noexcept {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "Word must be 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

Word word_from_bits(std::uint64_t bits) noexcept {
  Word value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool checked_add(std::size_t *value, std::size_t increment) noexcept {
  if (increment > std::numeric_limits<std::size_t>::max() - *value) {
    return false;
  }
  *value += increment;
  return true;
}

bool checked_add_product(std::size_t *value, std::size_t count,
                         std::size_t element_size) noexcept {
  if (element_size != 0 &&
      count > std::numeric_limits<std::size_t>::max() / element_size) {
    return false;
  }
  return checked_add(value, count * element_size);
}

bool size_to_u32(std::size_t value, std::uint32_t *output) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *output = static_cast<std::uint32_t>(value);
  return true;
}

bool valid_kind(std::uint8_t value) noexcept {
  return value == static_cast<std::uint8_t>(PortableIrKind::kFunction) ||
         value == static_cast<std::uint8_t>(PortableIrKind::kControlFlow);
}

bool decode_value_type(std::uint8_t encoded, ValueType *type) noexcept {
  if (encoded > static_cast<std::uint8_t>(ValueType::kMask64x2)) {
    return false;
  }
  *type = static_cast<ValueType>(encoded);
  return is_valid_value_type(*type);
}

bool decode_memory_width(std::uint8_t encoded, MemoryWidth *width) noexcept {
  if (encoded != static_cast<std::uint8_t>(MemoryWidth::k8) &&
      encoded != static_cast<std::uint8_t>(MemoryWidth::k16) &&
      encoded != static_cast<std::uint8_t>(MemoryWidth::k32) &&
      encoded != static_cast<std::uint8_t>(MemoryWidth::k64) &&
      encoded != static_cast<std::uint8_t>(MemoryWidth::k128)) {
    return false;
  }
  *width = static_cast<MemoryWidth>(encoded);
  return true;
}

bool decode_memory_byte_order(std::uint8_t encoded,
                              MemoryByteOrder *order) noexcept {
  if (encoded > static_cast<std::uint8_t>(MemoryByteOrder::kBigEndian)) {
    return false;
  }
  *order = static_cast<MemoryByteOrder>(encoded);
  return true;
}

struct Envelope final {
  PortableIrMetadata metadata;
  std::array<std::uint32_t, kManifestWords> manifest{};
  const std::uint8_t *body{nullptr};
  std::size_t body_size{0};
};

Status validate_manifest_limits(
    const std::array<std::uint32_t, kManifestWords> &manifest,
    PortableIrKind kind, const PortableIrLimits &limits) {
  const auto exceeds = [&](ManifestIndex index, std::size_t maximum) {
    return static_cast<std::size_t>(manifest[index]) > maximum;
  };
  if (exceeds(kParameters, limits.maximum_parameters)) {
    return exhausted_package("portable IR parameter limit exceeded");
  }
  if (exceeds(kNodes, limits.maximum_nodes)) {
    return exhausted_package("portable IR node limit exceeded");
  }
  if (exceeds(kBlocks, limits.maximum_blocks)) {
    return exhausted_package("portable IR block limit exceeded");
  }
  if (exceeds(kCallArguments, limits.maximum_call_arguments)) {
    return exhausted_package("portable IR call-argument limit exceeded");
  }
  if (exceeds(kEdgeArguments, limits.maximum_edge_arguments)) {
    return exhausted_package("portable IR edge-argument limit exceeded");
  }
  if (exceeds(kFastCalls, limits.maximum_fast_calls) ||
      exceeds(kFastCallSignatureValues,
              limits.maximum_fast_call_signature_values)) {
    return exhausted_package("portable IR fast-call limit exceeded");
  }
  if (exceeds(kMemoryRegions, limits.maximum_memory_regions) ||
      exceeds(kMemoryAccesses, limits.maximum_memory_accesses)) {
    return exhausted_package("portable IR memory limit exceeded");
  }
  if (exceeds(kAtomicAccesses, limits.maximum_atomic_accesses)) {
    return exhausted_package("portable IR atomic limit exceeded");
  }
  if (exceeds(kFrameSlots, limits.maximum_frame_slots)) {
    return exhausted_package("portable IR frame-slot limit exceeded");
  }
  if (exceeds(kTrustedObjects, limits.maximum_trusted_objects)) {
    return exhausted_package("portable IR trusted-object limit exceeded");
  }
  if (exceeds(kPatchCells, limits.maximum_patch_cells)) {
    return exhausted_package("portable IR patch-cell limit exceeded");
  }
  if (exceeds(kVectorConstants, limits.maximum_vector_constants) ||
      exceeds(kVectorShuffles, limits.maximum_vector_shuffles) ||
      exceeds(kVectorSelectArguments, limits.maximum_vector_select_arguments)) {
    return exhausted_package("portable IR vector side-table limit exceeded");
  }
  if (kind == PortableIrKind::kFunction) {
    if (manifest[kBlocks] != 0 || manifest[kBlockInstructionReferences] != 0 ||
        manifest[kEdgeArguments] != 0 ||
        manifest[kEntryBlock] != kInvalidIndex) {
      return invalid_package("straight-line package has CFG state");
    }
  } else {
    if (manifest[kReturnValue] != kInvalidIndex || manifest[kEntryBlock] != 0 ||
        manifest[kBlocks] == 0 ||
        manifest[kBlockInstructionReferences] != manifest[kNodes]) {
      return invalid_package("CFG package manifest is inconsistent");
    }
  }
  return Status::ok_status();
}

Status
expected_payload_size(const std::array<std::uint32_t, kManifestWords> &manifest,
                      std::size_t *output) {
  std::size_t size = kManifestSize;
  const auto add = [&](ManifestIndex index, std::size_t record_size) {
    return checked_add_product(&size, manifest[index], record_size);
  };
  if (!add(kParameters, 1U) || !add(kNodes, kNodeRecordSize) ||
      !add(kCallArguments, sizeof(std::uint32_t)) ||
      !add(kFastCalls, kFastCallRecordSize) ||
      !add(kFastCallSignatureValues, 1U) ||
      !add(kMemoryAccesses, kMemoryRecordSize) ||
      !add(kAtomicAccesses, kAtomicRecordSize) ||
      !add(kFrameSlots, kFrameRecordSize) ||
      !add(kTrustedObjects, kTrustedObjectRecordSize) ||
      !add(kPatchCells, kPatchCellRecordSize) ||
      !add(kVectorConstants, kVectorConstantRecordSize) ||
      !add(kVectorShuffles, kVectorShuffleRecordSize) ||
      !add(kVectorSelectArguments, sizeof(std::uint32_t)) ||
      !add(kBlocks, kBlockRecordSize) ||
      !add(kBlockInstructionReferences, sizeof(std::uint32_t)) ||
      !add(kEdgeArguments, sizeof(std::uint32_t))) {
    return invalid_package("portable IR size arithmetic overflow");
  }
  *output = size;
  return Status::ok_status();
}

Status parse_envelope(const std::uint8_t *bytes, std::size_t byte_count,
                      const PortableIrLimits &limits, Envelope *envelope) {
  if (bytes == nullptr && byte_count != 0) {
    return {StatusCode::kInvalidArgument,
            "portable IR bytes are null for a nonempty input"};
  }
  if (byte_count > limits.maximum_package_bytes) {
    return exhausted_package("portable IR package byte limit exceeded");
  }
  if (byte_count < kHeaderSize + kManifestSize) {
    return invalid_package("portable IR package is truncated");
  }

  Reader header(bytes, kHeaderSize);
  std::array<std::uint8_t, 8> magic{};
  std::uint16_t format_version = 0;
  std::uint16_t minimum_reader_version = 0;
  std::uint8_t encoded_kind = 0;
  std::uint8_t flags = 0;
  std::uint16_t header_size = 0;
  std::uint64_t encoded_total_size = 0;
  std::uint64_t encoded_payload_size = 0;
  std::array<std::uint8_t, kPortableIrDigestSize> encoded_digest{};
  if (!header.raw(magic.data(), magic.size()) || !header.u16(&format_version) ||
      !header.u16(&minimum_reader_version) || !header.u8(&encoded_kind) ||
      !header.u8(&flags) || !header.u16(&header_size) ||
      !header.u64(&encoded_total_size) || !header.u64(&encoded_payload_size) ||
      !header.raw(encoded_digest.data(), encoded_digest.size())) {
    return invalid_package("portable IR header is truncated");
  }
  if (magic != kMagic) {
    return invalid_package("portable IR magic is invalid");
  }
  if (format_version != kPortableIrFormatVersion ||
      minimum_reader_version > kPortableIrFormatVersion ||
      minimum_reader_version == 0) {
    return {StatusCode::kUnavailable,
            "portable IR format version is unsupported"};
  }
  if (!valid_kind(encoded_kind) || flags != 0 || header_size != kHeaderSize) {
    return invalid_package("portable IR header fields are invalid");
  }
  if (encoded_total_size != byte_count ||
      encoded_payload_size != byte_count - kHeaderSize) {
    return invalid_package("portable IR encoded length is inconsistent");
  }

  const std::uint8_t *payload = bytes + kHeaderSize;
  const std::size_t payload_size = byte_count - kHeaderSize;
  const auto actual_digest = detail::package_sha256(payload, payload_size);
  if (actual_digest != encoded_digest) {
    return invalid_package("portable IR payload digest does not match");
  }

  Reader manifest_reader(payload, kManifestSize);
  for (std::uint32_t &word : envelope->manifest) {
    if (!manifest_reader.u32(&word)) {
      return invalid_package("portable IR manifest is truncated");
    }
  }
  if (envelope->manifest[kSchema] != kSchemaRevision) {
    return {StatusCode::kUnavailable,
            "portable IR payload schema is unsupported"};
  }
  for (std::size_t index = kFirstReserved; index < kManifestWords; ++index) {
    if (envelope->manifest[index] != 0) {
      return invalid_package("portable IR manifest reserved word is nonzero",
                             index);
    }
  }

  const auto kind = static_cast<PortableIrKind>(encoded_kind);
  Status status = validate_manifest_limits(envelope->manifest, kind, limits);
  if (!status.ok()) {
    return status;
  }
  std::size_t expected_size = 0;
  status = expected_payload_size(envelope->manifest, &expected_size);
  if (!status.ok()) {
    return status;
  }
  if (expected_size != payload_size) {
    return invalid_package("portable IR payload length is not canonical");
  }

  envelope->metadata.kind = kind;
  envelope->metadata.format_version = format_version;
  envelope->metadata.package_bytes = byte_count;
  envelope->metadata.payload_bytes = payload_size;
  envelope->metadata.parameter_count = envelope->manifest[kParameters];
  envelope->metadata.node_count = envelope->manifest[kNodes];
  envelope->metadata.block_count = envelope->manifest[kBlocks];
  envelope->metadata.call_argument_count = envelope->manifest[kCallArguments];
  envelope->metadata.edge_argument_count = envelope->manifest[kEdgeArguments];
  envelope->metadata.payload_sha256 = encoded_digest;
  envelope->body = payload + kManifestSize;
  envelope->body_size = payload_size - kManifestSize;
  return Status::ok_status();
}

template <typename FunctionType>
Status
collect_common_manifest(const FunctionType &function,
                        std::array<std::uint32_t, kManifestWords> *manifest,
                        const PortableIrLimits &limits) {
  manifest->fill(0);
  (*manifest)[kSchema] = kSchemaRevision;
  const std::array<std::pair<std::size_t, ManifestIndex>, 13> counts = {{
      {function.parameter_count(), kParameters},
      {function.nodes().size(), kNodes},
      {function.call_arguments().size(), kCallArguments},
      {function.fast_calls().size(), kFastCalls},
      {function.memory_region_count(), kMemoryRegions},
      {function.memory_accesses().size(), kMemoryAccesses},
      {function.atomic_accesses().size(), kAtomicAccesses},
      {function.frame_slots().size(), kFrameSlots},
      {function.trusted_objects().size(), kTrustedObjects},
      {function.patch_cells().size(), kPatchCells},
      {function.vector_constants().size(), kVectorConstants},
      {function.vector_shuffles().size(), kVectorShuffles},
      {function.vector_select_arguments().size(), kVectorSelectArguments},
  }};
  for (const auto &count : counts) {
    if (!size_to_u32(count.first, &(*manifest)[count.second])) {
      return exhausted_package(
          "portable IR table exceeds the wire index range");
    }
  }
  std::size_t signature_values = 0;
  for (const FastCallDescriptor &descriptor : function.fast_calls()) {
    if (!checked_add(&signature_values, descriptor.parameter_types.size())) {
      return exhausted_package("portable IR fast-call signature overflows");
    }
  }
  if (!size_to_u32(signature_values, &(*manifest)[kFastCallSignatureValues])) {
    return exhausted_package("portable IR fast-call signatures are too large");
  }

  (void)limits;
  return Status::ok_status();
}

template <typename NodeType>
void write_node(const NodeType &node, Writer *writer) {
  writer->u8(static_cast<std::uint8_t>(node.opcode));
  writer->u8(static_cast<std::uint8_t>(node.type));
  writer->u16(0);
  writer->u32(node.lhs.id());
  writer->u32(node.rhs.id());
  writer->u32(node.auxiliary.id());
  writer->u32(node.argument_begin);
  writer->u32(node.argument_count);
  writer->u32(node.memory_access);
  writer->u32(node.frame_slot);
  writer->u32(node.trusted_object);
  writer->u32(node.atomic_access);
  writer->u64(word_bits(node.immediate));
}

void write_memory(const MemoryAccessDescriptor &access, Writer *writer) {
  writer->u32(access.region);
  writer->u32(access.alias_class);
  writer->u8(static_cast<std::uint8_t>(access.width));
  writer->u8(access.alignment);
  writer->u8(static_cast<std::uint8_t>(access.byte_order));
  writer->u8(static_cast<std::uint8_t>((access.sign_extend ? 1U : 0U) |
                                       (access.is_volatile ? 2U : 0U)));
}

template <typename FunctionType>
void write_common(const FunctionType &function,
                  const std::array<std::uint32_t, kManifestWords> &manifest,
                  Writer *writer) {
  for (std::uint32_t word : manifest) {
    writer->u32(word);
  }
  for (std::size_t index = 0; index < function.parameter_count(); ++index) {
    writer->u8(static_cast<std::uint8_t>(function.parameter_type(index)));
  }
  for (const auto &node : function.nodes()) {
    write_node(node, writer);
  }
  for (Value argument : function.call_arguments()) {
    writer->u32(argument.id());
  }
  for (const FastCallDescriptor &descriptor : function.fast_calls()) {
    writer->u8(static_cast<std::uint8_t>(descriptor.return_type));
    writer->u8(0);
    writer->u16(0);
    writer->u32(static_cast<std::uint32_t>(descriptor.parameter_types.size()));
  }
  for (const FastCallDescriptor &descriptor : function.fast_calls()) {
    for (ValueType type : descriptor.parameter_types) {
      writer->u8(static_cast<std::uint8_t>(type));
    }
  }
  for (const MemoryAccessDescriptor &access : function.memory_accesses()) {
    write_memory(access, writer);
  }
  for (const AtomicAccessDescriptor &access : function.atomic_accesses()) {
    write_memory(access.memory, writer);
    writer->u8(static_cast<std::uint8_t>(access.order));
    writer->u8(static_cast<std::uint8_t>(access.failure_order));
    writer->u8(static_cast<std::uint8_t>(access.strength));
    writer->u8(0);
  }
  for (const FrameSlotDescriptor &slot : function.frame_slots()) {
    writer->u8(static_cast<std::uint8_t>(slot.type));
    writer->u8(slot.sensitive ? 1U : 0U);
    writer->u16(0);
  }
  for (const TrustedObjectDescriptor &object : function.trusted_objects()) {
    writer->u64(object.layout_identity);
    writer->u64(static_cast<std::uint64_t>(object.byte_size));
  }
  for (const PatchCellDescriptor &cell : function.patch_cells()) {
    writer->u64(word_bits(cell.initial_value));
    writer->u8(static_cast<std::uint8_t>(cell.kind));
    for (std::size_t index = 0; index < 7U; ++index) {
      writer->u8(0);
    }
  }
  for (const Vector128 &constant : function.vector_constants()) {
    writer->raw(constant.bytes.data(), constant.bytes.size());
  }
  for (const VectorShuffle &shuffle : function.vector_shuffles()) {
    writer->raw(shuffle.lanes.data(), shuffle.lanes.size());
    writer->u8(shuffle.lane_count);
    for (std::size_t index = 0; index < 7U; ++index) {
      writer->u8(0);
    }
  }
  for (Value value : function.vector_select_arguments()) {
    writer->u32(value.id());
  }
}

template <typename FunctionType>
bool has_runtime_call(const FunctionType &function) noexcept {
  for (const auto &node : function.nodes()) {
    if constexpr (std::is_same_v<FunctionType, Function>) {
      if (node.opcode == Opcode::kCall) {
        return true;
      }
    } else if (node.opcode == ControlOpcode::kCall) {
      return true;
    }
  }
  return false;
}

PortableIrEncodeResult
finish_encoding(PortableIrKind kind, Writer payload,
                const std::array<std::uint32_t, kManifestWords> &manifest,
                const PortableIrLimits &limits) {
  PortableIrEncodeResult result;
  if (payload.bytes().size() >
      std::numeric_limits<std::size_t>::max() - kHeaderSize) {
    result.status = exhausted_package("portable IR package size overflows");
    return result;
  }
  const std::size_t total_size = kHeaderSize + payload.bytes().size();
  if (total_size > limits.maximum_package_bytes) {
    result.status =
        exhausted_package("portable IR package byte limit exceeded");
    return result;
  }
  const auto digest =
      detail::package_sha256(payload.bytes().data(), payload.bytes().size());
  Writer package(total_size);
  package.raw(kMagic.data(), kMagic.size());
  package.u16(kPortableIrFormatVersion);
  package.u16(kPortableIrFormatVersion);
  package.u8(static_cast<std::uint8_t>(kind));
  package.u8(0);
  package.u16(static_cast<std::uint16_t>(kHeaderSize));
  package.u64(static_cast<std::uint64_t>(total_size));
  package.u64(static_cast<std::uint64_t>(payload.bytes().size()));
  package.raw(digest.data(), digest.size());
  package.raw(payload.bytes().data(), payload.bytes().size());

  result.metadata.kind = kind;
  result.metadata.format_version = kPortableIrFormatVersion;
  result.metadata.package_bytes = total_size;
  result.metadata.payload_bytes = payload.bytes().size();
  result.metadata.parameter_count = manifest[kParameters];
  result.metadata.node_count = manifest[kNodes];
  result.metadata.block_count = manifest[kBlocks];
  result.metadata.call_argument_count = manifest[kCallArguments];
  result.metadata.edge_argument_count = manifest[kEdgeArguments];
  result.metadata.payload_sha256 = digest;
  result.bytes = std::move(package).take();
  result.status = Status::ok_status();
  return result;
}

struct CommonDecoded final {
  std::vector<ValueType> parameter_types;
  std::vector<Value> call_arguments;
  std::vector<FastCallDescriptor> fast_calls;
  std::vector<MemoryAccessDescriptor> memory_accesses;
  std::vector<AtomicAccessDescriptor> atomic_accesses;
  std::vector<FrameSlotDescriptor> frame_slots;
  std::vector<TrustedObjectDescriptor> trusted_objects;
  std::vector<PatchCellDescriptor> patch_cells;
  std::vector<Vector128> vector_constants;
  std::vector<VectorShuffle> vector_shuffles;
  std::vector<Value> vector_select_arguments;
};

template <typename NodeType, typename OpcodeType>
Status read_nodes(Reader *reader, std::size_t count,
                  std::uint8_t maximum_opcode, std::uint8_t runtime_call_opcode,
                  std::vector<NodeType> *nodes) {
  nodes->reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::uint8_t opcode = 0;
    std::uint8_t encoded_type = 0;
    std::uint16_t reserved = 0;
    std::uint32_t lhs = 0;
    std::uint32_t rhs = 0;
    std::uint32_t auxiliary = 0;
    std::uint32_t argument_begin = 0;
    std::uint32_t argument_count = 0;
    std::uint32_t memory_access = 0;
    std::uint32_t frame_slot = 0;
    std::uint32_t trusted_object = 0;
    std::uint32_t atomic_access = 0;
    std::uint64_t immediate = 0;
    if (!reader->u8(&opcode) || !reader->u8(&encoded_type) ||
        !reader->u16(&reserved) || !reader->u32(&lhs) || !reader->u32(&rhs) ||
        !reader->u32(&auxiliary) || !reader->u32(&argument_begin) ||
        !reader->u32(&argument_count) || !reader->u32(&memory_access) ||
        !reader->u32(&frame_slot) || !reader->u32(&trusted_object) ||
        !reader->u32(&atomic_access) || !reader->u64(&immediate)) {
      return invalid_package("portable IR node table is truncated", index);
    }
    ValueType type = ValueType::kWord;
    if (reserved != 0 || opcode > maximum_opcode ||
        opcode == runtime_call_opcode ||
        !decode_value_type(encoded_type, &type)) {
      return invalid_package("portable IR node encoding is invalid", index);
    }
    nodes->emplace_back(static_cast<OpcodeType>(opcode), Value{lhs}, Value{rhs},
                        word_from_bits(immediate), type, argument_begin,
                        argument_count, memory_access, frame_slot,
                        trusted_object, Value{auxiliary}, atomic_access);
  }
  return Status::ok_status();
}

Status read_memory(Reader *reader, MemoryAccessDescriptor *access) {
  std::uint8_t width = 0;
  std::uint8_t alignment = 0;
  std::uint8_t byte_order = 0;
  std::uint8_t flags = 0;
  if (!reader->u32(&access->region) || !reader->u32(&access->alias_class) ||
      !reader->u8(&width) || !reader->u8(&alignment) ||
      !reader->u8(&byte_order) || !reader->u8(&flags)) {
    return invalid_package("portable IR memory descriptor is truncated");
  }
  if (!decode_memory_width(width, &access->width) ||
      !decode_memory_byte_order(byte_order, &access->byte_order) ||
      (flags & ~UINT8_C(3)) != 0) {
    return invalid_package("portable IR memory descriptor is invalid");
  }
  access->alignment = alignment;
  access->sign_extend = (flags & 1U) != 0;
  access->is_volatile = (flags & 2U) != 0;
  return Status::ok_status();
}

Status read_common(Reader *reader, const Envelope &envelope,
                   CommonDecoded *decoded) {
  const auto &manifest = envelope.manifest;
  decoded->parameter_types.reserve(manifest[kParameters]);
  for (std::size_t index = 0; index < manifest[kParameters]; ++index) {
    std::uint8_t encoded = 0;
    ValueType type = ValueType::kWord;
    if (!reader->u8(&encoded) || !decode_value_type(encoded, &type) ||
        !is_scalar_value_type(type)) {
      return invalid_package("portable IR parameter type is invalid", index);
    }
    decoded->parameter_types.push_back(type);
  }
  return Status::ok_status();
}

Status read_common_after_nodes(Reader *reader, const Envelope &envelope,
                               CommonDecoded *decoded) {
  const auto &manifest = envelope.manifest;
  decoded->call_arguments.reserve(manifest[kCallArguments]);
  for (std::size_t index = 0; index < manifest[kCallArguments]; ++index) {
    std::uint32_t value = 0;
    if (!reader->u32(&value)) {
      return invalid_package("portable IR call arguments are truncated");
    }
    decoded->call_arguments.emplace_back(value);
  }

  struct FastCallHeader final {
    ValueType return_type{ValueType::kWord};
    std::uint32_t parameter_count{0};
  };
  std::vector<FastCallHeader> fast_call_headers;
  fast_call_headers.reserve(manifest[kFastCalls]);
  std::size_t signature_count = 0;
  for (std::size_t index = 0; index < manifest[kFastCalls]; ++index) {
    std::uint8_t encoded_return = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    std::uint32_t parameter_count = 0;
    ValueType return_type = ValueType::kWord;
    if (!reader->u8(&encoded_return) || !reader->u8(&reserved8) ||
        !reader->u16(&reserved16) || !reader->u32(&parameter_count) ||
        reserved8 != 0 || reserved16 != 0 ||
        !decode_value_type(encoded_return, &return_type) ||
        !is_scalar_value_type(return_type) ||
        !checked_add(&signature_count, parameter_count) ||
        signature_count > manifest[kFastCallSignatureValues]) {
      return invalid_package("portable IR fast-call descriptor is invalid",
                             index);
    }
    fast_call_headers.push_back({return_type, parameter_count});
  }
  if (signature_count != manifest[kFastCallSignatureValues]) {
    return invalid_package("portable IR fast-call signature count differs");
  }
  decoded->fast_calls.reserve(fast_call_headers.size());
  for (const FastCallHeader &header : fast_call_headers) {
    std::vector<ValueType> parameter_types;
    parameter_types.reserve(header.parameter_count);
    for (std::size_t index = 0; index < header.parameter_count; ++index) {
      std::uint8_t encoded = 0;
      ValueType type = ValueType::kWord;
      if (!reader->u8(&encoded) || !decode_value_type(encoded, &type) ||
          !is_scalar_value_type(type)) {
        return invalid_package("portable IR fast-call parameter is invalid");
      }
      parameter_types.push_back(type);
    }
    decoded->fast_calls.emplace_back(std::move(parameter_types),
                                     header.return_type);
  }

  decoded->memory_accesses.reserve(manifest[kMemoryAccesses]);
  for (std::size_t index = 0; index < manifest[kMemoryAccesses]; ++index) {
    MemoryAccessDescriptor access;
    Status status = read_memory(reader, &access);
    if (!status.ok()) {
      return status;
    }
    decoded->memory_accesses.push_back(access);
  }

  decoded->atomic_accesses.reserve(manifest[kAtomicAccesses]);
  for (std::size_t index = 0; index < manifest[kAtomicAccesses]; ++index) {
    AtomicAccessDescriptor access;
    Status status = read_memory(reader, &access.memory);
    std::uint8_t order = 0;
    std::uint8_t failure_order = 0;
    std::uint8_t strength = 0;
    std::uint8_t reserved = 0;
    if (!status.ok() || !reader->u8(&order) || !reader->u8(&failure_order) ||
        !reader->u8(&strength) || !reader->u8(&reserved) || reserved != 0 ||
        order > static_cast<std::uint8_t>(
                    AtomicMemoryOrder::kSequentiallyConsistent) ||
        failure_order > static_cast<std::uint8_t>(
                            AtomicMemoryOrder::kSequentiallyConsistent) ||
        strength >
            static_cast<std::uint8_t>(AtomicCompareExchangeStrength::kWeak)) {
      return invalid_package("portable IR atomic descriptor is invalid", index);
    }
    access.order = static_cast<AtomicMemoryOrder>(order);
    access.failure_order = static_cast<AtomicMemoryOrder>(failure_order);
    access.strength = static_cast<AtomicCompareExchangeStrength>(strength);
    decoded->atomic_accesses.push_back(access);
  }

  decoded->frame_slots.reserve(manifest[kFrameSlots]);
  for (std::size_t index = 0; index < manifest[kFrameSlots]; ++index) {
    std::uint8_t encoded_type = 0;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    ValueType type = ValueType::kWord;
    if (!reader->u8(&encoded_type) || !reader->u8(&flags) ||
        !reader->u16(&reserved) || !decode_value_type(encoded_type, &type) ||
        !is_scalar_value_type(type) || (flags & ~UINT8_C(1)) != 0 ||
        reserved != 0) {
      return invalid_package("portable IR frame slot is invalid", index);
    }
    decoded->frame_slots.push_back({type, (flags & 1U) != 0});
  }

  decoded->trusted_objects.reserve(manifest[kTrustedObjects]);
  for (std::size_t index = 0; index < manifest[kTrustedObjects]; ++index) {
    std::uint64_t identity = 0;
    std::uint64_t byte_size = 0;
    if (!reader->u64(&identity) || !reader->u64(&byte_size) ||
        byte_size > std::numeric_limits<std::size_t>::max()) {
      return invalid_package("portable IR trusted object is invalid", index);
    }
    decoded->trusted_objects.push_back(
        {identity, static_cast<std::size_t>(byte_size)});
  }

  decoded->patch_cells.reserve(manifest[kPatchCells]);
  for (std::size_t index = 0; index < manifest[kPatchCells]; ++index) {
    std::uint64_t initial_value = 0;
    std::uint8_t kind = 0;
    std::array<std::uint8_t, 7> reserved{};
    if (!reader->u64(&initial_value) || !reader->u8(&kind) ||
        !reader->raw(reserved.data(), reserved.size()) ||
        std::any_of(reserved.begin(), reserved.end(),
                    [](std::uint8_t value) { return value != 0; }) ||
        kind > static_cast<std::uint8_t>(PatchCellKind::kCounter)) {
      return invalid_package("portable IR patch cell is invalid", index);
    }
    decoded->patch_cells.push_back(
        {word_from_bits(initial_value), static_cast<PatchCellKind>(kind)});
  }

  decoded->vector_constants.resize(manifest[kVectorConstants]);
  for (std::size_t index = 0; index < manifest[kVectorConstants]; ++index) {
    Vector128 &constant = decoded->vector_constants[index];
    if (!reader->raw(constant.bytes.data(), constant.bytes.size())) {
      return invalid_package("portable IR vector constants are truncated");
    }
  }

  decoded->vector_shuffles.resize(manifest[kVectorShuffles]);
  for (std::size_t index = 0; index < manifest[kVectorShuffles]; ++index) {
    VectorShuffle &shuffle = decoded->vector_shuffles[index];
    std::array<std::uint8_t, 7> reserved{};
    if (!reader->raw(shuffle.lanes.data(), shuffle.lanes.size()) ||
        !reader->u8(&shuffle.lane_count) ||
        !reader->raw(reserved.data(), reserved.size()) ||
        shuffle.lane_count > shuffle.lanes.size() ||
        std::any_of(reserved.begin(), reserved.end(),
                    [](std::uint8_t value) { return value != 0; })) {
      return invalid_package("portable IR vector shuffle is invalid", index);
    }
  }

  decoded->vector_select_arguments.reserve(manifest[kVectorSelectArguments]);
  for (std::size_t index = 0; index < manifest[kVectorSelectArguments];
       ++index) {
    std::uint32_t value = 0;
    if (!reader->u32(&value)) {
      return invalid_package("portable IR vector selects are truncated");
    }
    decoded->vector_select_arguments.emplace_back(value);
  }
  return Status::ok_status();
}

Status require_consumed(const Reader &reader) {
  return reader.remaining() == 0
             ? Status::ok_status()
             : invalid_package("portable IR package has trailing bytes");
}

} // namespace

PortableIrEncodeResult
encode_portable_ir(const Function &function,
                   const PortableIrLimits &limits) noexcept {
  try {
    Status status = verify(function);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    if (has_runtime_call(function)) {
      return {{StatusCode::kInvalidIr,
               "runtime helper addresses cannot enter portable IR"},
              {},
              {}};
    }
    std::array<std::uint32_t, kManifestWords> manifest{};
    status = collect_common_manifest(function, &manifest, limits);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    manifest[kReturnValue] = function.return_value().id();
    manifest[kEntryBlock] = kInvalidIndex;
    status =
        validate_manifest_limits(manifest, PortableIrKind::kFunction, limits);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    std::size_t payload_size = 0;
    status = expected_payload_size(manifest, &payload_size);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    Writer payload(payload_size);
    write_common(function, manifest, &payload);
    if (payload.bytes().size() != payload_size) {
      return {invalid_package("portable IR encoder size mismatch"), {}, {}};
    }
    return finish_encoding(PortableIrKind::kFunction, std::move(payload),
                           manifest, limits);
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted, ""}, {}, {}};
  } catch (...) {
    return {{StatusCode::kInvalidIr, ""}, {}, {}};
  }
}

PortableIrEncodeResult
encode_portable_ir(const ControlFlowFunction &function,
                   const PortableIrLimits &limits) noexcept {
  try {
    Status status = verify(function);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    if (has_runtime_call(function)) {
      return {{StatusCode::kInvalidIr,
               "runtime helper addresses cannot enter portable IR"},
              {},
              {}};
    }
    std::array<std::uint32_t, kManifestWords> manifest{};
    status = collect_common_manifest(function, &manifest, limits);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    manifest[kReturnValue] = kInvalidIndex;
    manifest[kEntryBlock] = function.entry_block().id();
    if (!size_to_u32(function.blocks().size(), &manifest[kBlocks])) {
      return {exhausted_package("portable IR CFG has too many blocks"), {}, {}};
    }
    std::size_t instruction_references = 0;
    std::size_t edge_arguments = 0;
    for (const BasicBlock &block : function.blocks()) {
      if (!checked_add(&instruction_references, block.instructions.size()) ||
          !checked_add(&edge_arguments,
                       block.terminator.true_edge.arguments.size()) ||
          !checked_add(&edge_arguments,
                       block.terminator.false_edge.arguments.size())) {
        return {exhausted_package("portable IR CFG counts overflow"), {}, {}};
      }
    }
    if (!size_to_u32(instruction_references,
                     &manifest[kBlockInstructionReferences]) ||
        !size_to_u32(edge_arguments, &manifest[kEdgeArguments])) {
      return {
          exhausted_package("portable IR CFG tables are too large"), {}, {}};
    }
    status = validate_manifest_limits(manifest, PortableIrKind::kControlFlow,
                                      limits);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    std::size_t payload_size = 0;
    status = expected_payload_size(manifest, &payload_size);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    Writer payload(payload_size);
    write_common(function, manifest, &payload);
    for (const BasicBlock &block : function.blocks()) {
      payload.u32(static_cast<std::uint32_t>(block.parameters.size()));
      payload.u32(static_cast<std::uint32_t>(block.instructions.size()));
      payload.u8(static_cast<std::uint8_t>(block.terminator.opcode));
      payload.u8(0);
      payload.u16(0);
      payload.u32(block.terminator.value.id());
      payload.u32(block.terminator.true_edge.target.id());
      payload.u32(static_cast<std::uint32_t>(
          block.terminator.true_edge.arguments.size()));
      payload.u32(block.terminator.false_edge.target.id());
      payload.u32(static_cast<std::uint32_t>(
          block.terminator.false_edge.arguments.size()));
      for (Value instruction : block.instructions) {
        payload.u32(instruction.id());
      }
      for (Value argument : block.terminator.true_edge.arguments) {
        payload.u32(argument.id());
      }
      for (Value argument : block.terminator.false_edge.arguments) {
        payload.u32(argument.id());
      }
    }
    if (payload.bytes().size() != payload_size) {
      return {invalid_package("portable IR CFG encoder size mismatch"), {}, {}};
    }
    return finish_encoding(PortableIrKind::kControlFlow, std::move(payload),
                           manifest, limits);
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted, ""}, {}, {}};
  } catch (...) {
    return {{StatusCode::kInvalidIr, ""}, {}, {}};
  }
}

PortableIrInspectResult
inspect_portable_ir(const std::uint8_t *bytes, std::size_t byte_count,
                    const PortableIrLimits &limits) noexcept {
  try {
    Envelope envelope;
    PortableIrInspectResult result;
    result.status = parse_envelope(bytes, byte_count, limits, &envelope);
    if (result.status.ok()) {
      result.metadata = envelope.metadata;
    }
    return result;
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted, ""}, {}};
  } catch (...) {
    return {{StatusCode::kInvalidIr, ""}, {}};
  }
}

PortableFunctionDecodeResult
decode_portable_function(const std::uint8_t *bytes, std::size_t byte_count,
                         const PortableIrLimits &limits) noexcept {
  try {
    Envelope envelope;
    Status status = parse_envelope(bytes, byte_count, limits, &envelope);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    if (envelope.metadata.kind != PortableIrKind::kFunction) {
      return {
          invalid_package("portable IR package is not straight-line"), {}, {}};
    }
    Reader reader(envelope.body, envelope.body_size);
    CommonDecoded common;
    status = read_common(&reader, envelope, &common);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    std::vector<Node> nodes;
    status = read_nodes<Node, Opcode>(
        &reader, envelope.manifest[kNodes],
        static_cast<std::uint8_t>(Opcode::kVectorWiden),
        static_cast<std::uint8_t>(Opcode::kCall), &nodes);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    status = read_common_after_nodes(&reader, envelope, &common);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    status = require_consumed(reader);
    if (!status.ok()) {
      return {status, {}, {}};
    }

    Function function;
    detail::PortableIrAccess::publish_function(
        &function, std::move(common.parameter_types), std::move(nodes),
        std::move(common.call_arguments), std::move(common.fast_calls),
        envelope.manifest[kMemoryRegions], std::move(common.memory_accesses),
        std::move(common.atomic_accesses), std::move(common.frame_slots),
        std::move(common.trusted_objects), std::move(common.patch_cells),
        std::move(common.vector_constants), std::move(common.vector_shuffles),
        std::move(common.vector_select_arguments),
        Value{envelope.manifest[kReturnValue]});
    status = verify(function);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    PortableFunctionDecodeResult result;
    result.status = Status::ok_status();
    result.metadata = envelope.metadata;
    result.function = std::move(function);
    return result;
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted, ""}, {}, {}};
  } catch (...) {
    return {{StatusCode::kInvalidIr, ""}, {}, {}};
  }
}

PortableControlFlowDecodeResult
decode_portable_control_flow(const std::uint8_t *bytes, std::size_t byte_count,
                             const PortableIrLimits &limits) noexcept {
  try {
    Envelope envelope;
    Status status = parse_envelope(bytes, byte_count, limits, &envelope);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    if (envelope.metadata.kind != PortableIrKind::kControlFlow) {
      return {invalid_package("portable IR package is not a CFG"), {}, {}};
    }
    Reader reader(envelope.body, envelope.body_size);
    CommonDecoded common;
    status = read_common(&reader, envelope, &common);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    std::vector<ControlNode> nodes;
    status = read_nodes<ControlNode, ControlOpcode>(
        &reader, envelope.manifest[kNodes],
        static_cast<std::uint8_t>(ControlOpcode::kVectorWiden),
        static_cast<std::uint8_t>(ControlOpcode::kCall), &nodes);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    status = read_common_after_nodes(&reader, envelope, &common);
    if (!status.ok()) {
      return {status, {}, {}};
    }

    std::vector<BasicBlock> blocks;
    blocks.reserve(envelope.manifest[kBlocks]);
    std::size_t instruction_references = 0;
    std::size_t edge_arguments = 0;
    for (std::size_t block_index = 0; block_index < envelope.manifest[kBlocks];
         ++block_index) {
      std::uint32_t parameter_count = 0;
      std::uint32_t instruction_count = 0;
      std::uint8_t terminator = 0;
      std::uint8_t reserved8 = 0;
      std::uint16_t reserved16 = 0;
      std::uint32_t value = 0;
      std::uint32_t true_target = 0;
      std::uint32_t true_argument_count = 0;
      std::uint32_t false_target = 0;
      std::uint32_t false_argument_count = 0;
      if (!reader.u32(&parameter_count) || !reader.u32(&instruction_count) ||
          !reader.u8(&terminator) || !reader.u8(&reserved8) ||
          !reader.u16(&reserved16) || !reader.u32(&value) ||
          !reader.u32(&true_target) || !reader.u32(&true_argument_count) ||
          !reader.u32(&false_target) || !reader.u32(&false_argument_count) ||
          reserved8 != 0 || reserved16 != 0 ||
          terminator > static_cast<std::uint8_t>(TerminatorOpcode::kBranch) ||
          terminator == static_cast<std::uint8_t>(TerminatorOpcode::kNone) ||
          parameter_count > instruction_count ||
          !checked_add(&instruction_references, instruction_count) ||
          instruction_references >
              envelope.manifest[kBlockInstructionReferences] ||
          !checked_add(&edge_arguments, true_argument_count) ||
          !checked_add(&edge_arguments, false_argument_count) ||
          edge_arguments > envelope.manifest[kEdgeArguments]) {
        return {
            invalid_package("portable IR CFG block is invalid", block_index),
            {},
            {}};
      }

      const auto terminator_opcode = static_cast<TerminatorOpcode>(terminator);
      if ((terminator_opcode == TerminatorOpcode::kReturn &&
           (true_target != kInvalidIndex || false_target != kInvalidIndex ||
            true_argument_count != 0 || false_argument_count != 0)) ||
          (terminator_opcode == TerminatorOpcode::kJump &&
           (value != kInvalidIndex || true_target == kInvalidIndex ||
            false_target != kInvalidIndex || false_argument_count != 0)) ||
          (terminator_opcode == TerminatorOpcode::kBranch &&
           (value == kInvalidIndex || true_target == kInvalidIndex ||
            false_target == kInvalidIndex))) {
        return {invalid_package("portable IR CFG terminator is noncanonical",
                                block_index),
                {},
                {}};
      }

      BasicBlock block;
      block.instructions.reserve(instruction_count);
      for (std::size_t index = 0; index < instruction_count; ++index) {
        std::uint32_t instruction = 0;
        if (!reader.u32(&instruction)) {
          return {invalid_package("portable IR CFG instructions are truncated"),
                  {},
                  {}};
        }
        block.instructions.emplace_back(instruction);
      }
      block.parameters.assign(block.instructions.begin(),
                              block.instructions.begin() +
                                  static_cast<std::ptrdiff_t>(parameter_count));
      block.terminator.opcode = terminator_opcode;
      block.terminator.value = Value{value};
      block.terminator.true_edge.target = Block{true_target};
      block.terminator.true_edge.arguments.reserve(true_argument_count);
      for (std::size_t index = 0; index < true_argument_count; ++index) {
        std::uint32_t argument = 0;
        if (!reader.u32(&argument)) {
          return {invalid_package("portable IR CFG edge is truncated"), {}, {}};
        }
        block.terminator.true_edge.arguments.emplace_back(argument);
      }
      block.terminator.false_edge.target = Block{false_target};
      block.terminator.false_edge.arguments.reserve(false_argument_count);
      for (std::size_t index = 0; index < false_argument_count; ++index) {
        std::uint32_t argument = 0;
        if (!reader.u32(&argument)) {
          return {invalid_package("portable IR CFG edge is truncated"), {}, {}};
        }
        block.terminator.false_edge.arguments.emplace_back(argument);
      }
      blocks.push_back(std::move(block));
    }
    if (instruction_references !=
            envelope.manifest[kBlockInstructionReferences] ||
        edge_arguments != envelope.manifest[kEdgeArguments]) {
      return {
          invalid_package("portable IR CFG aggregate counts differ"), {}, {}};
    }
    status = require_consumed(reader);
    if (!status.ok()) {
      return {status, {}, {}};
    }

    ControlFlowFunction function;
    detail::PortableIrAccess::publish_control_flow(
        &function, std::move(common.parameter_types),
        Block{envelope.manifest[kEntryBlock]}, std::move(nodes),
        std::move(common.call_arguments), std::move(common.fast_calls),
        envelope.manifest[kMemoryRegions], std::move(common.memory_accesses),
        std::move(common.atomic_accesses), std::move(common.frame_slots),
        std::move(common.trusted_objects), std::move(common.patch_cells),
        std::move(common.vector_constants), std::move(common.vector_shuffles),
        std::move(common.vector_select_arguments), std::move(blocks));
    status = verify(function);
    if (!status.ok()) {
      return {status, {}, {}};
    }
    PortableControlFlowDecodeResult result;
    result.status = Status::ok_status();
    result.metadata = envelope.metadata;
    result.function = std::move(function);
    return result;
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted, ""}, {}, {}};
  } catch (...) {
    return {{StatusCode::kInvalidIr, ""}, {}, {}};
  }
}

std::string portable_ir_digest_hex(
    const std::array<std::uint8_t, kPortableIrDigestSize> &digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.resize(digest.size() * 2U);
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2U] = kHex[digest[index] >> 4U];
    result[index * 2U + 1U] = kHex[digest[index] & UINT8_C(0x0f)];
  }
  return result;
}

} // namespace unijit::ir
