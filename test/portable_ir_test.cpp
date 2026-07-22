#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "ir/package_sha256.h"
#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/ir/package.h"
#include "unijit/jit/compiler.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Value;
using unijit::ir::ValueType;
using unijit::ir::Word;

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << "portable IR failure: " << message << '\n';
  }
}

void store_u32(std::vector<std::uint8_t> *bytes, std::size_t offset,
               std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index) {
    (*bytes)[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void resign(std::vector<std::uint8_t> *bytes) {
  const auto digest = unijit::ir::detail::package_sha256(bytes->data() + 64U,
                                                         bytes->size() - 64U);
  std::copy(digest.begin(), digest.end(), bytes->begin() + 32);
}

Word forbidden_helper(const Word *arguments, std::size_t count) {
  return count == 0 ? 0 : arguments[0];
}

Function make_scalar_function() {
  FunctionBuilder builder(2);
  const Value sum = builder.add(builder.parameter(0), builder.parameter(1));
  const Value scaled = builder.multiply(sum, builder.constant(2));
  expect(builder.set_return(scaled).ok(),
         "scalar fixture must accept its return value");
  return std::move(builder).build();
}

void test_sha256() {
  static constexpr std::array<std::uint8_t, 3> kAbc = {'a', 'b', 'c'};
  const auto digest =
      unijit::ir::detail::package_sha256(kAbc.data(), kAbc.size());
  expect(unijit::ir::portable_ir_digest_hex(digest) ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 must match the published abc test vector");
}

void test_scalar_round_trip() {
  const Function function = make_scalar_function();
  const auto first = unijit::ir::encode_portable_ir(function);
  expect(first.ok(), "verified scalar IR must encode");
  if (!first.ok()) {
    return;
  }
  expect(first.metadata.kind == unijit::ir::PortableIrKind::kFunction &&
             first.metadata.format_version == 1 &&
             first.metadata.parameter_count == 2 &&
             first.metadata.node_count == 5 &&
             first.metadata.block_count == 0 &&
             first.metadata.package_bytes == first.bytes.size() &&
             unijit::ir::portable_ir_digest_hex(first.metadata.payload_sha256)
                     .size() == 64,
         "scalar metadata must describe the canonical package");

  const auto inspected = unijit::ir::inspect_portable_ir(first.bytes);
  expect(inspected.ok() &&
             inspected.metadata.payload_sha256 ==
                 first.metadata.payload_sha256 &&
             inspected.metadata.node_count == first.metadata.node_count,
         "package inspection must reproduce authenticated metadata");

  auto decoded = unijit::ir::decode_portable_function(first.bytes);
  expect(decoded.ok() && unijit::ir::verify(decoded.function).ok(),
         "scalar package must decode into verified IR");
  if (!decoded.ok()) {
    return;
  }
  const auto interpreted =
      unijit::ir::Interpreter::evaluate(decoded.function, {8, 13});
  expect(interpreted.ok() && interpreted.value == 42,
         "decoded scalar IR must preserve interpreter semantics");
  auto compiled = unijit::jit::Compiler::compile(decoded.function);
  const std::array<Word, 2> arguments = {8, 13};
  const auto native =
      compiled.ok()
          ? compiled.function->invoke(arguments.data(), arguments.size())
          : unijit::ir::EvaluationResult{};
  expect(compiled.ok() && native.ok() && native.value == 42,
         "decoded scalar IR must preserve native semantics");

  const auto second = unijit::ir::encode_portable_ir(decoded.function);
  expect(second.ok() && second.bytes == first.bytes &&
             second.metadata.payload_sha256 == first.metadata.payload_sha256,
         "decode and re-encode must be byte-identical");
  expect(!unijit::ir::decode_portable_control_flow(first.bytes).ok(),
         "kind-specific decoding must reject a scalar package as CFG");
}

Function make_side_table_function() {
  using unijit::ir::AtomicAccessDescriptor;
  using unijit::ir::AtomicCompareExchangeStrength;
  using unijit::ir::AtomicMemoryOrder;
  using unijit::ir::MemoryAccessDescriptor;
  using unijit::ir::MemoryByteOrder;
  using unijit::ir::MemoryWidth;
  using unijit::ir::PatchCellKind;
  using unijit::ir::Vector128;

  FunctionBuilder builder({ValueType::kWord, ValueType::kFloat64}, 1);
  const Value offset = builder.constant(0);
  const MemoryAccessDescriptor memory{
      0, 7, MemoryWidth::k64, 8, MemoryByteOrder::kLittleEndian, false, false};
  const Value loaded = builder.load_word(offset, memory, 10);
  builder.store_word(offset, loaded, memory, 11);
  const MemoryAccessDescriptor floating{
      0, 8, MemoryWidth::k64, 8, MemoryByteOrder::kBigEndian, false, false};
  const Value loaded_float = builder.load_float(offset, floating, 12);
  builder.store_float(offset, loaded_float, floating, 13);

  AtomicAccessDescriptor atomic;
  atomic.memory = memory;
  atomic.memory.byte_order = MemoryByteOrder::kNative;
  atomic.order = AtomicMemoryOrder::kAcquire;
  atomic.failure_order = AtomicMemoryOrder::kRelaxed;
  atomic.strength = AtomicCompareExchangeStrength::kStrong;
  builder.atomic_load(offset, atomic, 14);
  builder.atomic_fence(AtomicMemoryOrder::kSequentiallyConsistent);

  const auto frame = builder.create_frame_slot(ValueType::kWord, true);
  builder.store_frame(frame, loaded);
  builder.load_frame(frame);
  const auto object = builder.create_trusted_object(
      UINT64_C(0x1020304050607080), sizeof(Word) * 2U);
  builder.store_object(object, 0, loaded);
  builder.load_object(object, sizeof(Word), ValueType::kFloat64);
  const auto cell = builder.create_patch_cell(19, PatchCellKind::kCounter);
  builder.load_patch_cell(cell);

  const auto fast = builder.create_fast_call(
      {ValueType::kWord, ValueType::kFloat64}, ValueType::kWord);
  builder.fast_call(fast, {loaded, builder.parameter(1)});

  Vector128 bits;
  for (std::size_t index = 0; index < bits.bytes.size(); ++index) {
    bits.bytes[index] = static_cast<std::uint8_t>(index + 1U);
  }
  const Value vector = builder.vector_constant(ValueType::kI32x4, bits);
  const Value shuffled = builder.vector_shuffle(vector, {3, 2, 1, 0});
  Vector128 mask_bits;
  mask_bits.bytes.fill(UINT8_MAX);
  const Value mask = builder.vector_constant(ValueType::kMask32x4, mask_bits);
  const Value selected = builder.vector_select(mask, shuffled, vector);
  builder.store_vector(offset, selected,
                       {0, 9, MemoryWidth::k128, 1,
                        MemoryByteOrder::kLittleEndian, false, false},
                       15);
  builder.load_vector(
      offset, ValueType::kI32x4,
      {0, 10, MemoryWidth::k128, 16, MemoryByteOrder::kBigEndian, false, false},
      16);
  expect(builder.set_return(loaded).ok(),
         "side-table fixture must accept its return value");
  return std::move(builder).build();
}

void test_side_table_round_trip() {
  const Function function = make_side_table_function();
  const auto verification = unijit::ir::verify(function);
  if (!verification.ok()) {
    std::cerr << "side-table verification detail: " << verification.message()
              << " at " << verification.location() << '\n';
  }
  expect(verification.ok(), "side-table fixture must verify before packaging");
  const auto encoded = unijit::ir::encode_portable_ir(function);
  expect(encoded.ok(), "all portable side tables must encode together");
  if (!encoded.ok()) {
    return;
  }
  auto decoded = unijit::ir::decode_portable_function(encoded.bytes);
  expect(decoded.ok(), "all portable side tables must decode together");
  if (!decoded.ok()) {
    return;
  }
  const Function &rebuilt = decoded.function;
  expect(rebuilt.memory_accesses().size() == 6 &&
             rebuilt.atomic_accesses().size() == 1 &&
             rebuilt.frame_slots().size() == 1 &&
             rebuilt.frame_slots()[0].sensitive &&
             rebuilt.trusted_objects().size() == 1 &&
             rebuilt.patch_cells().size() == 1 &&
             rebuilt.fast_calls().size() == 1 &&
             rebuilt.vector_constants().size() == 2 &&
             rebuilt.vector_shuffles().size() == 1 &&
             rebuilt.vector_select_arguments().size() == 1,
         "decoded side-table cardinalities must remain exact");
  const auto rebuilt_bytes = unijit::ir::encode_portable_ir(rebuilt);
  expect(rebuilt_bytes.ok() && rebuilt_bytes.bytes == encoded.bytes,
         "side-table reconstruction must retain canonical bytes");
}

unijit::ir::ControlFlowFunction make_cfg_function() {
  unijit::ir::ControlFlowBuilder builder(2);
  const auto take_lhs = builder.create_block(0);
  const auto take_rhs = builder.create_block(0);
  const auto merge = builder.create_block(1);
  const Value condition =
      builder.less_than(builder.parameter(0), builder.parameter(1));
  expect(builder.branch(condition, take_rhs, {}, take_lhs, {}).ok(),
         "CFG fixture entry must branch");
  expect(builder.set_insertion_block(take_lhs).ok() &&
             builder.jump(merge, {builder.parameter(0)}).ok(),
         "CFG fixture left edge must be valid");
  expect(builder.set_insertion_block(take_rhs).ok() &&
             builder.jump(merge, {builder.parameter(1)}).ok(),
         "CFG fixture right edge must be valid");
  expect(builder.set_insertion_block(merge).ok() &&
             builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "CFG fixture merge must return its argument");
  return std::move(builder).build();
}

void test_cfg_round_trip() {
  const auto function = make_cfg_function();
  const auto encoded = unijit::ir::encode_portable_ir(function);
  expect(encoded.ok() && encoded.metadata.block_count == 4 &&
             encoded.metadata.edge_argument_count == 2,
         "verified CFG must encode with exact aggregate counts");
  if (!encoded.ok()) {
    return;
  }
  auto decoded = unijit::ir::decode_portable_control_flow(encoded.bytes);
  expect(decoded.ok() && unijit::ir::verify(decoded.function).ok(),
         "CFG package must reconstruct a verified graph");
  if (!decoded.ok()) {
    return;
  }
  const auto interpreted =
      unijit::ir::ControlFlowInterpreter::evaluate(decoded.function, {17, 91});
  auto compiled = unijit::jit::Compiler::compile(decoded.function);
  const std::array<Word, 2> arguments = {117, -4};
  const auto native =
      compiled.ok()
          ? compiled.function->invoke(arguments.data(), arguments.size())
          : unijit::ir::EvaluationResult{};
  expect(interpreted.ok() && interpreted.value == 91 && compiled.ok() &&
             native.ok() && native.value == 117,
         "decoded CFG must preserve interpreter and native branch semantics");
  const auto rebuilt = unijit::ir::encode_portable_ir(decoded.function);
  expect(rebuilt.ok() && rebuilt.bytes == encoded.bytes,
         "CFG decode and re-encode must be byte-identical");
  expect(!unijit::ir::decode_portable_function(encoded.bytes).ok(),
         "kind-specific decoding must reject a CFG as straight-line IR");
}

void test_fail_closed_inputs() {
  const Function function = make_scalar_function();
  const auto encoded = unijit::ir::encode_portable_ir(function);
  if (!encoded.ok()) {
    expect(false, "corruption fixture must encode");
    return;
  }
  expect(!unijit::ir::decode_portable_function(nullptr, 0).ok(),
         "empty input must fail closed");
  expect(!unijit::ir::decode_portable_function(nullptr, 1).ok(),
         "null nonempty input must fail closed");
  for (std::size_t length = 0; length < encoded.bytes.size(); ++length) {
    if (unijit::ir::decode_portable_function(encoded.bytes.data(), length)
            .ok()) {
      expect(false, "every strict package prefix must be rejected");
      break;
    }
  }

  auto corrupted = encoded.bytes;
  corrupted[0] ^= 1U;
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "bad magic must be rejected");
  corrupted = encoded.bytes;
  corrupted[13] = 1;
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "unknown header flags must be rejected");
  corrupted = encoded.bytes;
  corrupted.back() ^= UINT8_C(0x80);
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "payload corruption must be rejected by SHA-256");

  corrupted = encoded.bytes;
  store_u32(&corrupted, 64U + 20U * 4U, 1U);
  resign(&corrupted);
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "nonzero manifest reserves must be rejected after valid hashing");

  corrupted = encoded.bytes;
  const std::size_t first_node = 64U + 96U + 2U;
  corrupted[first_node] = UINT8_MAX;
  resign(&corrupted);
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "unknown opcodes must be rejected after valid hashing");
  corrupted = encoded.bytes;
  corrupted[first_node] = static_cast<std::uint8_t>(unijit::ir::Opcode::kCall);
  resign(&corrupted);
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "runtime helper opcodes must never be decoded");
  corrupted = encoded.bytes;
  corrupted[first_node + 2U] = 1;
  resign(&corrupted);
  expect(!unijit::ir::decode_portable_function(corrupted).ok(),
         "nonzero node reserves must be rejected");

  unijit::ir::PortableIrLimits limits;
  limits.maximum_nodes = encoded.metadata.node_count - 1U;
  const auto limited =
      unijit::ir::decode_portable_function(encoded.bytes, limits);
  expect(!limited.ok() &&
             limited.status.code() == unijit::StatusCode::kResourceExhausted,
         "decode budgets must reject a package before table allocation");

  FunctionBuilder helper_builder(1);
  const Value call =
      helper_builder.call(forbidden_helper, {helper_builder.parameter(0)});
  expect(helper_builder.set_return(call).ok(),
         "runtime-helper rejection fixture must verify");
  const auto helper_package =
      unijit::ir::encode_portable_ir(std::move(helper_builder).build());
  expect(!helper_package.ok() &&
             helper_package.status.code() == unijit::StatusCode::kInvalidIr,
         "runtime helper addresses must be rejected during encoding");
}

} // namespace

int main() {
  test_sha256();
  test_scalar_round_trip();
  test_side_table_round_trip();
  test_cfg_round_trip();
  test_fail_closed_inputs();
  if (failures != 0) {
    std::cerr << failures << " portable IR assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "portable IR qualification passed\n";
  return EXIT_SUCCESS;
}
