#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/ir/optimizer.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"
#include "jit/register_allocator.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::Compiler;

int failures = 0;
std::size_t runtime_call_count = 0;

struct TestMaterializedField final {
  bool is_object{false};
  unijit::ir::ValueType type{unijit::ir::ValueType::kWord};
  Word primitive{0};
  unijit::runtime::ObjectHandle object{0};
};

struct TestMaterializedObject final {
  std::size_t id{0};
  std::uint64_t kind{0};
  std::vector<TestMaterializedField> fields;
};

struct TestMaterializer final {
  bool active{false};
  bool fail_begin{false};
  bool fail_commit{false};
  unijit::runtime::DeoptimizationReason reason{
      unijit::runtime::DeoptimizationReason::kGuardFailed};
  std::size_t site{0};
  std::size_t resume_offset{0};
  std::size_t frame_value_count{0};
  std::size_t begin_count{0};
  std::size_t allocate_count{0};
  std::size_t commit_count{0};
  std::size_t rollback_count{0};
  std::size_t store_count{0};
  std::size_t frame_store_count{0};
  std::size_t fail_allocate{std::numeric_limits<std::size_t>::max()};
  std::size_t fail_store{std::numeric_limits<std::size_t>::max()};
  std::size_t fail_frame_store{std::numeric_limits<std::size_t>::max()};
  std::vector<TestMaterializedObject> staged;
  std::vector<TestMaterializedObject> committed;
  std::vector<unijit::runtime::MaterializedValue> staged_frame;
  std::vector<unijit::runtime::MaterializedValue> committed_frame;
};

unijit::Status begin_materialization(void* opaque,
                                     unijit::runtime::DeoptimizationReason reason,
                                     std::size_t site,
                                     std::size_t resume_offset,
                                     std::size_t object_count,
                                     std::size_t frame_value_count) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  ++state->begin_count;
  state->active = true;
  state->reason = reason;
  state->site = site;
  state->resume_offset = resume_offset;
  state->frame_value_count = frame_value_count;
  state->allocate_count = 0;
  state->store_count = 0;
  state->frame_store_count = 0;
  state->staged.clear();
  state->staged_frame.clear();
  if (state->fail_begin) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected object transaction failure"};
  }
  try {
    state->staged.reserve(object_count);
    state->staged_frame.reserve(frame_value_count);
    return unijit::Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {unijit::StatusCode::kResourceExhausted,
            "test materializer could not reserve objects"};
  }
}

unijit::Status allocate_materialized_object(
    void* opaque, std::size_t id, std::uint64_t kind,
    std::size_t field_count,
    unijit::runtime::ObjectHandle* handle) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  if (!state->active || handle == nullptr) {
    return {unijit::StatusCode::kInvalidArgument,
            "test materialization allocation is outside a transaction"};
  }
  if (state->allocate_count++ == state->fail_allocate) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected object allocation failure"};
  }
  try {
    state->staged.push_back({id, kind,
                             std::vector<TestMaterializedField>(field_count)});
    *handle = static_cast<unijit::runtime::ObjectHandle>(
        1000 + state->staged.size());
    return unijit::Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {unijit::StatusCode::kResourceExhausted,
            "test materializer could not allocate an object"};
  }
}

TestMaterializedField* materialized_field(
    TestMaterializer* state, unijit::runtime::ObjectHandle object,
    std::size_t field_index) noexcept {
  if (!state->active || object <= 1000) {
    return nullptr;
  }
  const std::size_t object_index = static_cast<std::size_t>(object - 1001);
  if (object_index >= state->staged.size() ||
      field_index >= state->staged[object_index].fields.size()) {
    return nullptr;
  }
  return &state->staged[object_index].fields[field_index];
}

unijit::Status store_materialized_primitive(
    void* opaque, unijit::runtime::ObjectHandle object,
    std::size_t field_index, unijit::ir::ValueType type,
    Word value) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  TestMaterializedField* field =
      materialized_field(state, object, field_index);
  if (field == nullptr) {
    return {unijit::StatusCode::kInvalidArgument,
            "test primitive field is unavailable"};
  }
  if (state->store_count++ == state->fail_store) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected object field failure"};
  }
  field->type = type;
  field->primitive = value;
  return unijit::Status::ok_status();
}

unijit::Status store_materialized_object(
    void* opaque, unijit::runtime::ObjectHandle object,
    std::size_t field_index,
    unijit::runtime::ObjectHandle value) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  TestMaterializedField* field =
      materialized_field(state, object, field_index);
  if (field == nullptr) {
    return {unijit::StatusCode::kInvalidArgument,
            "test object field is unavailable"};
  }
  if (state->store_count++ == state->fail_store) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected object field failure"};
  }
  field->is_object = true;
  field->object = value;
  return unijit::Status::ok_status();
}

unijit::Status store_materialized_frame_primitive(
    void* opaque, std::size_t slot, unijit::ir::ValueType type,
    Word value) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  if (!state->active) {
    return {unijit::StatusCode::kInvalidArgument,
            "test primitive frame slot is outside a transaction"};
  }
  if (state->frame_store_count++ == state->fail_frame_store) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected frame slot failure"};
  }
  try {
    state->staged_frame.push_back(
        {slot,
         type == unijit::ir::ValueType::kFloat64
             ? unijit::runtime::MaterializedValueKind::kFloat64
             : unijit::runtime::MaterializedValueKind::kWord,
         value, 0});
    return unijit::Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {unijit::StatusCode::kResourceExhausted,
            "test materializer could not install a primitive frame slot"};
  }
}

unijit::Status store_materialized_frame_object(
    void* opaque, std::size_t slot,
    unijit::runtime::ObjectHandle value) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  if (!state->active) {
    return {unijit::StatusCode::kInvalidArgument,
            "test object frame slot is outside a transaction"};
  }
  if (state->frame_store_count++ == state->fail_frame_store) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected frame slot failure"};
  }
  try {
    state->staged_frame.push_back(
        {slot, unijit::runtime::MaterializedValueKind::kObject, 0, value});
    return unijit::Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {unijit::StatusCode::kResourceExhausted,
            "test materializer could not install an object frame slot"};
  }
}

unijit::Status commit_materialization(void* opaque) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  if (!state->active) {
    return {unijit::StatusCode::kInvalidArgument,
            "test materialization commit has no transaction"};
  }
  if (state->fail_commit) {
    return {unijit::StatusCode::kResourceExhausted,
            "injected object commit failure"};
  }
  state->committed.clear();
  state->committed.swap(state->staged);
  state->committed_frame.clear();
  state->committed_frame.swap(state->staged_frame);
  state->active = false;
  ++state->commit_count;
  return unijit::Status::ok_status();
}

void rollback_materialization(void* opaque) noexcept {
  auto* state = static_cast<TestMaterializer*>(opaque);
  state->staged.clear();
  state->staged_frame.clear();
  state->active = false;
  ++state->rollback_count;
}

unijit::runtime::MaterializationCallbacks materialization_callbacks(
    TestMaterializer* state) noexcept {
  return {state,
          begin_materialization,
          allocate_materialized_object,
          store_materialized_primitive,
          store_materialized_object,
          store_materialized_frame_primitive,
          store_materialized_frame_object,
          commit_materialization,
          rollback_materialization};
}

Word sum_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  Word result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    result += arguments[index];
  }
  return result;
}

Word float_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  if (count != 2) {
    return unijit::ir::pack_float64(0.0);
  }
  return unijit::ir::pack_float64(
      unijit::ir::unpack_float64(arguments[0]) *
      unijit::ir::unpack_float64(arguments[1]));
}

Word mixed_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  if (count != 12) {
    return unijit::ir::pack_float64(0.0);
  }
  double result = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    result += index % 2 == 0
                  ? static_cast<double>(arguments[index])
                  : unijit::ir::unpack_float64(arguments[index]);
  }
  return unijit::ir::pack_float64(result);
}

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::unique_ptr<unijit::jit::CompiledFunction> compile_constant(Word value) {
  FunctionBuilder builder(0);
  const Value result = builder.constant(value);
  if (!builder.set_return(result).ok()) {
    return nullptr;
  }
  auto compilation = Compiler::compile(std::move(builder).build());
  return compilation.ok() ? std::move(compilation.function) : nullptr;
}

void test_target_profiles() {
  using unijit::jit::CodeCache;
  using unijit::jit::CodeCacheLimits;
  using unijit::jit::CompilationOptions;
  using unijit::jit::TargetAbi;
  using unijit::jit::TargetArchitecture;
  using unijit::jit::TargetEndianness;
  using unijit::jit::TargetFeature;
  using unijit::jit::TargetProfile;
  using unijit::jit::VectorWidthPolicy;

  const TargetProfile baseline = unijit::jit::baseline_target_profile();
  const TargetProfile host = unijit::jit::host_target_profile();
  expect(unijit::jit::validate_target_profile(baseline).ok() &&
             unijit::jit::validate_target_profile(host).ok() &&
             unijit::jit::target_profile_contains(host, baseline) &&
             unijit::jit::target_profile_key(baseline) != 0,
         "host discovery must contain the validated portable target baseline");
  expect(!unijit::jit::has_target_feature(host, TargetFeature::kAvx) ||
             (host.vector_width_policy == VectorWidthPolicy::kNative &&
              host.maximum_vector_bits >= 256),
         "AVX host discovery must report an OS-authorized native width");

  TargetProfile malformed = baseline;
  malformed.endianness = TargetEndianness::kBig;
  expect(!unijit::jit::validate_target_profile(malformed).ok(),
         "target validation must reject unsupported target endianness");

  TargetProfile alien = baseline;
  if (baseline.architecture == TargetArchitecture::kX86_64) {
    alien.features |=
        unijit::jit::target_feature_bit(TargetFeature::kNeon);
  } else {
    alien.features |=
        unijit::jit::target_feature_bit(TargetFeature::kSse2);
  }
  expect(!unijit::jit::validate_target_profile(alien).ok(),
         "target validation must reject cross-architecture feature bits");

  TargetProfile incompatible;
  incompatible.endianness = TargetEndianness::kLittle;
  incompatible.maximum_vector_bits = 128;
  if (baseline.architecture == TargetArchitecture::kX86_64) {
    incompatible.architecture = TargetArchitecture::kAArch64;
    incompatible.abi = TargetAbi::kAapcs64;
    incompatible.features =
        unijit::jit::target_feature_bit(TargetFeature::kFp64) |
        unijit::jit::target_feature_bit(TargetFeature::kNeon);
  } else {
    incompatible.architecture = TargetArchitecture::kX86_64;
    incompatible.abi = TargetAbi::kSystemV;
    incompatible.features =
        unijit::jit::target_feature_bit(TargetFeature::kFp64) |
        unijit::jit::target_feature_bit(TargetFeature::kSse2);
  }
  FunctionBuilder incompatible_builder(0);
  const Value incompatible_value = incompatible_builder.constant(1);
  expect(incompatible_builder.set_return(incompatible_value).ok(),
         "incompatible target fixture must have a return value");
  CompilationOptions incompatible_options;
  incompatible_options.target_profile = incompatible;
  const auto incompatible_compilation = Compiler::compile(
      std::move(incompatible_builder).build(), incompatible_options);
  expect(!incompatible_compilation.ok() &&
             incompatible_compilation.status.code() ==
                 unijit::StatusCode::kUnsupportedArchitecture,
         "compiler must reject a valid profile for another native backend");

  TargetProfile specialized = baseline;
  specialized.vector_width_policy = VectorWidthPolicy::kNative;
  FunctionBuilder specialized_builder(0);
  const Value specialized_value = specialized_builder.constant(29);
  expect(specialized_builder.set_return(specialized_value).ok(),
         "specialized target fixture must have a return value");
  CompilationOptions specialized_options;
  specialized_options.target_profile = specialized;
  auto specialized_compilation = Compiler::compile(
      std::move(specialized_builder).build(), specialized_options);
  expect(specialized_compilation.ok() &&
             unijit::jit::target_profiles_equal(
                 specialized_compilation.function->target_profile(),
                 specialized) &&
             specialized_compilation.function->target_profile_key() ==
                 unijit::jit::target_profile_key(specialized),
         "compiled functions must retain their immutable target identity");
  if (!specialized_compilation.ok()) {
    return;
  }

  CodeCache baseline_cache;
  auto wrong_cache = baseline_cache.publish(
      "profile", 1, std::move(specialized_compilation.function));
  expect(!wrong_cache.ok(),
         "code caches must reject functions from another target profile");

  FunctionBuilder matching_builder(0);
  const Value matching_value = matching_builder.constant(31);
  expect(matching_builder.set_return(matching_value).ok(),
         "matching target fixture must have a return value");
  auto matching_compilation = Compiler::compile(
      std::move(matching_builder).build(), specialized_options);
  CodeCache specialized_cache(CodeCacheLimits{}, specialized);
  auto publication = specialized_cache.publish(
      "profile", 2, std::move(matching_compilation.function));
  const auto result = publication.handle.invoke(nullptr, 0);
  expect(publication.ok() && publication.handle.target_profile() != nullptr &&
             publication.handle.target_profile_key() ==
                 unijit::jit::target_profile_key(specialized) &&
             result.ok() && result.value == 31,
         "profile-scoped code caches must retain and execute matching code");
}

void test_bounded_memory_interpreter() {
  using unijit::ir::MemoryAccessDescriptor;
  using unijit::ir::MemoryByteOrder;
  using unijit::ir::MemoryWidth;
  using unijit::runtime::ExecutionContext;
  using unijit::runtime::MemoryRegion;

  alignas(8) std::array<std::uint8_t, 32> bytes{};
  bytes[0] = 0x80;
  bytes[2] = 0x34;
  bytes[3] = 0x12;
  bytes[5] = 0x01;
  bytes[6] = 0x23;
  bytes[7] = 0x45;
  bytes[8] = 0x67;
  MemoryRegion writable{bytes.data(), bytes.size(), true};
  ExecutionContext context;
  expect(context.bind_memory_regions(&writable, 1).ok(),
         "execution context must accept a valid bounded region");
  expect(!context.bind_memory_regions(nullptr, 1).ok(),
         "execution context must reject missing region descriptors");
  const MemoryRegion invalid_nonempty{nullptr, 1, false};
  expect(!context.bind_memory_regions(&invalid_nonempty, 1).ok(),
         "execution context must reject a null non-empty region");
  expect(context.bind_memory_regions(&writable, 1).ok(),
         "valid memory binding must be restorable after rejection");

  const auto evaluate_load = [&](MemoryAccessDescriptor access, Word offset,
                                 std::size_t site) {
    FunctionBuilder builder(1, 1);
    const Value loaded = builder.load_word(builder.parameter(0), access, site);
    expect(builder.set_return(loaded).ok(),
           "bounded load fixture must have a return value");
    const Function function = std::move(builder).build();
    expect(unijit::ir::verify(function).ok(),
           "bounded load fixture must verify");
    return Interpreter::evaluate(function, std::vector<Word>{offset},
                                 &context);
  };

  MemoryAccessDescriptor unsigned8;
  unsigned8.width = MemoryWidth::k8;
  const auto unsigned_result = evaluate_load(unsigned8, 0, 10);
  expect(unsigned_result.ok() && unsigned_result.value == 128,
         "unsigned 8-bit loads must zero-extend");

  MemoryAccessDescriptor signed8 = unsigned8;
  signed8.sign_extend = true;
  const auto signed_result = evaluate_load(signed8, 0, 11);
  expect(signed_result.ok() && signed_result.value == -128,
         "signed 8-bit loads must sign-extend");

  MemoryAccessDescriptor little16;
  little16.width = MemoryWidth::k16;
  little16.alignment = 2;
  little16.byte_order = MemoryByteOrder::kLittleEndian;
  const auto little_result = evaluate_load(little16, 2, 12);
  expect(little_result.ok() && little_result.value == 0x1234,
         "little-endian 16-bit loads must preserve byte order");

  MemoryAccessDescriptor big32;
  big32.width = MemoryWidth::k32;
  big32.byte_order = MemoryByteOrder::kBigEndian;
  big32.is_volatile = true;
  const auto big_result = evaluate_load(big32, 5, 13);
  expect(big_result.ok() && big_result.value == 0x01234567,
         "unaligned volatile big-endian loads must be byte exact");

  FunctionBuilder store_builder(2, 1);
  MemoryAccessDescriptor little32;
  little32.width = MemoryWidth::k32;
  little32.byte_order = MemoryByteOrder::kLittleEndian;
  const Value stored = store_builder.store_word(
      store_builder.parameter(0), store_builder.parameter(1), little32, 20);
  expect(store_builder.set_return(stored).ok(),
         "bounded store fixture must return the stored word");
  const Function store_function = std::move(store_builder).build();
  const std::array<Word, 2> store_arguments = {12, 0x76543210};
  const auto store_result = Interpreter::evaluate(
      store_function, store_arguments.data(), store_arguments.size(),
      &context);
  expect(store_result.ok() && store_result.value == store_arguments[1] &&
             bytes[12] == 0x10 && bytes[13] == 0x32 && bytes[14] == 0x54 &&
             bytes[15] == 0x76,
         "bounded stores must return their value and write explicit order");
  const auto optimized_store = unijit::ir::Optimizer::run(store_function);
  const auto optimized_store_result =
      optimized_store.ok()
          ? Interpreter::evaluate(optimized_store.function,
                                  store_arguments.data(),
                                  store_arguments.size(), &context)
          : unijit::ir::EvaluationResult{optimized_store.status, 0};
  expect(optimized_store.ok() &&
             optimized_store.function.memory_region_count() == 1 &&
             optimized_store.function.memory_accesses().size() == 1 &&
             optimized_store_result.ok() &&
             optimized_store_result.value == store_arguments[1],
         "optimizer must retain effectful bounded memory descriptors");

  auto native_store = Compiler::compile(store_function);
  const std::array<Word, 2> native_store_arguments = {20, 0x12345678};
  const auto native_store_result =
      native_store.ok()
          ? native_store.function->invoke(native_store_arguments.data(),
                                          native_store_arguments.size(),
                                          &context)
          : unijit::ir::EvaluationResult{native_store.status, 0};
  expect(native_store.ok() && native_store.function->requires_context() &&
             native_store.function->stack_map(20) != nullptr &&
             native_store_result.ok() &&
             native_store_result.value == native_store_arguments[1] &&
             bytes[20] == 0x78 && bytes[21] == 0x56 && bytes[22] == 0x34 &&
             bytes[23] == 0x12,
         "native bounded stores must preserve interpreter order and metadata");
  if (native_store.ok()) {
    const std::array<Word, 2> native_oob_arguments = {31, 0x55};
    const auto native_oob = native_store.function->invoke(
        native_oob_arguments.data(), native_oob_arguments.size(), &context);
    expect(!native_oob.ok() &&
               native_oob.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               context.exit_site() == 20 && context.exit_value() == 31,
           "native bounded stores must diagnose an overflowing width");
    expect(native_store.function->native_entry()(native_store_arguments.data(),
                                                 nullptr) == 0,
           "direct native memory entry must fail closed without a context");
  }

  FunctionBuilder native_signed_builder(1, 1);
  const Value native_signed_load = native_signed_builder.load_word(
      native_signed_builder.parameter(0), signed8, 21);
  expect(native_signed_builder.set_return(native_signed_load).ok(),
         "native signed load fixture must have a return value");
  auto native_signed =
      Compiler::compile(std::move(native_signed_builder).build());
  const std::array<Word, 1> signed_arguments = {0};
  const auto native_signed_result =
      native_signed.ok()
          ? native_signed.function->invoke(signed_arguments.data(),
                                           signed_arguments.size(), &context)
          : unijit::ir::EvaluationResult{native_signed.status, 0};
  expect(native_signed_result.ok() && native_signed_result.value == -128,
         "native bounded loads must match signed extension semantics");

  MemoryAccessDescriptor aligned32 = little32;
  aligned32.alignment = 4;
  const auto misaligned = evaluate_load(aligned32, 1, 30);
  expect(!misaligned.ok() &&
             misaligned.status.code() == unijit::StatusCode::kRuntimeExit &&
             context.exit_reason() == unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 30 && context.exit_value() == 1,
         "misaligned loads must produce a diagnosed runtime exit");

  const auto out_of_bounds = evaluate_load(unsigned8, -1, 31);
  expect(!out_of_bounds.ok() && context.exit_site() == 31 &&
             context.exit_value() == -1,
         "negative offsets must fail unsigned bounds checks without wrapping");

  MemoryRegion read_only{bytes.data(), bytes.size(), false};
  expect(context.bind_memory_regions(&read_only, 1).ok(),
         "read-only region fixture must bind");
  const auto read_only_store = Interpreter::evaluate(
      store_function, store_arguments.data(), store_arguments.size(),
      &context);
  expect(!read_only_store.ok() && context.exit_site() == 20,
         "stores must reject a read-only region at their declared site");
  expect(context.bind_memory_regions(&writable, 1).ok(),
         "writable memory fixture must be restored");

  FunctionBuilder duplicate_builder(1, 1);
  const Value duplicate_load = duplicate_builder.load_word(
      duplicate_builder.parameter(0), unsigned8, 40);
  const Value duplicate_guard =
      duplicate_builder.guard_word_nonzero(duplicate_load, 40);
  expect(duplicate_builder.set_return(duplicate_guard).ok() &&
             !unijit::ir::verify(std::move(duplicate_builder).build()).ok(),
         "memory exits and guards must share one globally unique site space");

  FunctionBuilder bad_region_builder(1, 1);
  MemoryAccessDescriptor bad_region = unsigned8;
  bad_region.region = 1;
  const Value invalid_load = bad_region_builder.load_word(
      bad_region_builder.parameter(0), bad_region, 41);
  expect(bad_region_builder.set_return(invalid_load).ok() &&
             !unijit::ir::verify(std::move(bad_region_builder).build()).ok(),
         "verifier must reject an access outside the declared region table");
}

void test_bounded_memory_control_flow_interpreter() {
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::MemoryAccessDescriptor;
  using unijit::ir::MemoryByteOrder;
  using unijit::ir::MemoryWidth;
  using unijit::runtime::ExecutionContext;
  using unijit::runtime::MemoryRegion;

  alignas(8) std::array<std::uint8_t, 16> bytes{};
  MemoryRegion region{bytes.data(), bytes.size(), true};
  ExecutionContext context;
  expect(context.bind_memory_regions(&region, 1).ok(),
         "CFG bounded memory fixture must bind its region");

  ControlFlowBuilder builder(2, 1);
  MemoryAccessDescriptor big16;
  big16.width = MemoryWidth::k16;
  big16.byte_order = MemoryByteOrder::kBigEndian;
  const Value stored = builder.store_word(builder.parameter(0),
                                          builder.parameter(1), big16, 50);
  const Value loaded = builder.load_word(builder.parameter(0), big16, 51);
  const Value combined = builder.add(stored, loaded);
  expect(builder.set_return(combined).ok(),
         "CFG bounded memory fixture must return its ordered result");
  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "CFG bounded store/load sequence must verify");
  const std::array<Word, 2> arguments = {3, 0x1234};
  const auto result = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size(), 10, &context);
  expect(result.ok() && result.value == 0x2468 && bytes[3] == 0x12 &&
             bytes[4] == 0x34,
         "CFG interpreter must preserve store-before-load effects and order");
  const auto optimized = unijit::ir::Optimizer::run(function);
  const auto optimized_result =
      optimized.ok()
          ? unijit::ir::ControlFlowInterpreter::evaluate(
                optimized.function, arguments.data(), arguments.size(), 10,
                &context)
          : unijit::ir::EvaluationResult{optimized.status, 0};
  expect(optimized.ok() && optimized.function.memory_region_count() == 1 &&
             optimized.function.memory_accesses().size() == 2 &&
             optimized_result.ok() && optimized_result.value == 0x2468,
         "CFG optimization must retain ordered bounded memory effects");

  auto native = Compiler::compile(function);
  const auto native_result =
      native.ok()
          ? native.function->invoke(arguments.data(), arguments.size(),
                                    &context)
          : unijit::ir::EvaluationResult{native.status, 0};
  expect(native.ok() && native.function->stack_map(50) != nullptr &&
             native.function->stack_map(51) != nullptr &&
             native_result.ok() && native_result.value == 0x2468 &&
             bytes[3] == 0x12 && bytes[4] == 0x34,
         "native CFG memory effects must match the reference interpreter");
  if (native.ok()) {
    const std::array<Word, 2> out_of_bounds = {15, 0x1234};
    const auto failed = native.function->invoke(
        out_of_bounds.data(), out_of_bounds.size(), &context);
    expect(!failed.ok() && context.exit_site() == 50 &&
               context.exit_value() == 15,
           "native CFG stores must fail before an out-of-range write");
  }
}

void test_code_cache_lifecycle() {
  using unijit::jit::CodeCache;
  using unijit::jit::CodeCacheLimits;
  using unijit::jit::CodeHandle;

  CodeCache cache(CodeCacheLimits{2, 1024U * 1024U});
  auto first = cache.publish("alpha", 1, compile_constant(11));
  expect(first.ok() && first.cached && !first.reused,
         "code cache must publish a new compiled function");
  expect(first.handle.parameter_count() == 0 &&
             first.handle.return_type() ==
                 unijit::ir::ValueType::kWord &&
             first.handle.compilation_stats() != nullptr &&
             first.handle.compilation_stats()->executable_mapping_size >=
                 first.handle.compilation_stats()->code_size,
         "code handles must expose immutable compilation metadata");
  const CodeHandle first_lease = first.handle;

  const CodeHandle first_hit = cache.find("alpha", 1);
  const auto first_result = first_hit.invoke(nullptr, 0);
  expect(first_result.ok() && first_result.value == 11,
         "code-cache lookup must return callable native code");
  expect(!cache.find("alpha", 2).valid(),
         "code-cache lookup must reject a mismatched fingerprint");

  auto reused = cache.publish("alpha", 1, compile_constant(99));
  const auto reused_result = reused.handle.invoke(nullptr, 0);
  expect(reused.ok() && reused.cached && reused.reused &&
             reused.handle.generation() == first.handle.generation() &&
             reused_result.ok() && reused_result.value == 11,
         "duplicate publication must reuse the resident generation");

  auto replacement = cache.publish("alpha", 2, compile_constant(22));
  const auto replacement_result = replacement.handle.invoke(nullptr, 0);
  const auto stale_result = first_lease.invoke(nullptr, 0);
  expect(replacement.ok() && replacement.cached && !replacement.reused &&
             replacement.handle.generation() != first.handle.generation() &&
             replacement_result.ok() && replacement_result.value == 22,
         "new fingerprints must replace the resident generation");
  expect(stale_result.ok() && stale_result.value == 11,
         "replacement must not reclaim an active code lease");

  expect(!cache.invalidate("alpha", 1) && cache.invalidate("alpha", 2) &&
             !cache.find("alpha", 2).valid(),
         "fingerprinted invalidation must only remove the matching entry");
  const auto invalidated_lease_result = replacement.handle.invoke(nullptr, 0);
  expect(invalidated_lease_result.ok() && invalidated_lease_result.value == 22,
         "invalidated code must remain callable through an active lease");

  auto beta = cache.publish("beta", 1, compile_constant(31));
  auto gamma = cache.publish("gamma", 1, compile_constant(32));
  expect(beta.ok() && gamma.ok() && cache.find("beta", 1).valid(),
         "LRU fixture must populate and touch two entries");
  auto delta = cache.publish("delta", 1, compile_constant(33));
  expect(delta.ok() && cache.find("beta", 1).valid() &&
             !cache.find("gamma", 1).valid() &&
             cache.find("delta", 1).valid(),
         "entry budget must evict the least-recently-used generation");

  const CodeHandle surviving_clear = beta.handle;
  cache.clear();
  const auto clear_result = surviving_clear.invoke(nullptr, 0);
  const auto statistics = cache.stats();
  expect(clear_result.ok() && clear_result.value == 31,
         "cache clear must preserve active code leases");
  expect(statistics.resident_entries == 0 &&
             statistics.resident_code_bytes == 0 &&
             statistics.hits >= 4 && statistics.misses >= 3 &&
             statistics.publication_reuses == 1 &&
             statistics.replacements == 1 &&
             statistics.invalidations == 1 && statistics.evictions == 1 &&
             statistics.clears == 1,
         "code cache must report bounded lifecycle metrics");

  CodeCache disabled(CodeCacheLimits{0, 0});
  auto uncached = disabled.publish("bounded", 1, compile_constant(41));
  const auto uncached_result = uncached.handle.invoke(nullptr, 0);
  expect(uncached.ok() && !uncached.cached && uncached_result.ok() &&
             uncached_result.value == 41 &&
             disabled.stats().uncached_publications == 1,
         "disabled caching must still return an owned callable lease");

  CodeHandle surviving_cache;
  {
    CodeCache temporary;
    auto publication =
        temporary.publish("survivor", 7, compile_constant(77));
    surviving_cache = publication.handle;
  }
  const auto destroyed_cache_result = surviving_cache.invoke(nullptr, 0);
  expect(destroyed_cache_result.ok() && destroyed_cache_result.value == 77,
         "destroying a cache must not reclaim an active lease");
}

void test_code_cache_concurrency() {
  using unijit::jit::CodeCache;
  using unijit::jit::CodeCacheLimits;

  CodeCache cache(CodeCacheLimits{2, 1024U * 1024U});
  expect(cache.publish("hot", 1, compile_constant(101)).ok(),
         "concurrent cache fixture must publish its first generation");

  std::atomic<bool> start{false};
  std::atomic<std::size_t> errors{0};
  std::vector<std::thread> readers;
  for (std::size_t thread_index = 0; thread_index < 4; ++thread_index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
        for (std::uint64_t fingerprint = 1; fingerprint <= 2;
             ++fingerprint) {
          const auto handle = cache.find("hot", fingerprint);
          if (!handle.valid()) {
            continue;
          }
          const auto result = handle.invoke(nullptr, 0);
          const Word expected = fingerprint == 1 ? 101 : 202;
          if (!result.ok() || result.value != expected) {
            errors.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::size_t iteration = 0; iteration < 128; ++iteration) {
    const std::uint64_t fingerprint = (iteration & 1U) == 0 ? 2 : 1;
    const Word value = fingerprint == 1 ? 101 : 202;
    const auto publication =
        cache.publish("hot", fingerprint, compile_constant(value));
    if (!publication.ok()) {
      errors.fetch_add(1, std::memory_order_relaxed);
    }
    if ((iteration % 11U) == 0) {
      (void)cache.invalidate("hot", fingerprint);
    }
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  expect(errors.load(std::memory_order_relaxed) == 0,
         "concurrent lookup, replacement, invalidation, and invocation must be safe");
}

Function arithmetic_function() {
  FunctionBuilder builder(2);
  const Value sum = builder.add(builder.parameter(0), builder.parameter(1));
  const Value scaled = builder.multiply(sum, builder.constant(-7));
  const Value result = builder.subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "arithmetic return must be accepted");
  return std::move(builder).build();
}

void test_verifier_rejects_forward_reference() {
  FunctionBuilder builder(0);
  const Value invalid_binary = builder.add(Value{7}, Value{8});
  expect(builder.set_return(invalid_binary).ok(),
         "builder records a structurally present return value");
  const Function function = std::move(builder).build();
  const unijit::Status status = unijit::ir::verify(function);
  expect(!status.ok(), "verifier must reject non-dominating SSA operands");
  expect(status.location() == 0, "verifier must report the invalid node index");
}

void test_constant_native_function() {
  FunctionBuilder builder(0);
  const Value constant = builder.constant(std::numeric_limits<Word>::min());
  expect(builder.set_return(constant).ok(), "constant return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok(), "constant native function must execute");
  expect(result.value == std::numeric_limits<Word>::min(),
         "native constant materialization must preserve all 64 bits");
}

void test_execution_context_lifecycle() {
  unijit::runtime::ExecutionContext context;
  expect(!context.interrupt_requested(),
         "new execution contexts must not request interruption");
  context.request_interrupt();
  expect(context.interrupt_requested(),
         "execution contexts must publish interruption requests");
  context.clear_interrupt();
  expect(!context.interrupt_requested(),
         "execution contexts must clear interruption requests");

  context.record_exit(unijit::runtime::ExitReason::kRuntime, 91, -17);
  expect(context.exit_reason() == unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 91 && context.exit_value() == -17,
         "execution contexts must retain runtime-exit diagnostics");

  FunctionBuilder builder(0);
  expect(builder.set_return(builder.constant(73)).ok(),
         "execution-context fixture must have a return value");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "execution-context fixture must compile");
  if (compilation.ok()) {
    const auto result = compilation.function->invoke(nullptr, 0, &context);
    expect(result.ok() && result.value == 73,
           "invocation must clear stale exits before entering native code");
    expect(context.exit_reason() == unijit::runtime::ExitReason::kNone,
           "successful invocation must leave no exit reason");
  }
}

void test_safepoint_ir_and_interpreter() {
  FunctionBuilder builder(0);
  const Value base = builder.call(
      sum_runtime_helper, {builder.constant(70), builder.constant(3)});
  const Value safepoint = builder.safepoint(42);
  expect(safepoint.valid(), "safepoint must produce an effect value");
  const Value result = builder.add(base, safepoint);
  expect(builder.set_return(result).ok(),
         "safepoint fixture must have a return value");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "safepoint IR must verify");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "safepoint IR must optimize");
  if (optimization.ok()) {
    const bool preserved = std::any_of(
        optimization.function.nodes().begin(),
        optimization.function.nodes().end(), [](const unijit::ir::Node& node) {
          return node.opcode == unijit::ir::Opcode::kSafepoint;
        });
    expect(preserved, "optimizer must preserve a dead-result safepoint");
  }

  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interrupted = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(!interrupted.ok() &&
             interrupted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interrupted.status.location() == 42 &&
             context.exit_reason() ==
                 unijit::runtime::ExitReason::kSafepoint &&
             context.exit_site() == 42 && context.safepoint_polls() == 1,
         "interpreter safepoints must report interruption and site identity");

  context.clear_interrupt();
  const auto completed = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(completed.ok() && completed.value == 73 &&
             context.exit_reason() == unijit::runtime::ExitReason::kNone &&
             context.safepoint_polls() == 1,
         "clear safepoints must continue without changing the result");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "safepoint IR must compile to native code");
  if (compilation.ok()) {
    const unijit::jit::StackMapRecord* stack_map =
        compilation.function->stack_map(42);
    expect(stack_map != nullptr &&
               stack_map->kind == unijit::jit::StackMapKind::kSafepoint &&
               stack_map->native_offset <
                   compilation.function->stats().code_size &&
               stack_map->frame_size <=
                   compilation.function->stats().spill_slots * sizeof(Word) +
                       15 &&
               compilation.function->stats().stack_map_count == 1 &&
               compilation.function->stats().stack_map_value_count == 1 &&
               stack_map->live_values.size() == 1 &&
               stack_map->live_values[0].type ==
                   unijit::ir::ValueType::kWord &&
               stack_map->live_values[0].frame_offset <
                   stack_map->frame_size,
           "straight-line safepoints must publish canonical live-value stack maps");

    context.request_interrupt();
    const auto native_interrupted =
        compilation.function->invoke(nullptr, 0, &context);
    expect(!native_interrupted.ok() &&
               native_interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               native_interrupted.status.location() == 42 &&
               context.exit_reason() ==
                   unijit::runtime::ExitReason::kSafepoint &&
               context.safepoint_polls() == 1,
           "native safepoints must exit with the matching site identity");

    context.clear_interrupt();
    const auto native_completed =
        compilation.function->invoke(nullptr, 0, &context);
    expect(native_completed.ok() && native_completed.value == 73 &&
               context.safepoint_polls() == 1,
           "native clear safepoints must continue with a zero effect value");
    expect(compilation.function->native_entry()(nullptr, nullptr) == 73,
           "a null execution context must bypass safepoint polling");
  }

  unijit::jit::CompilationOptions unmeasured_options;
  unmeasured_options.measure_safepoint_polls = false;
  auto unmeasured_compilation =
      Compiler::compile(function, unmeasured_options);
  expect(unmeasured_compilation.ok(),
         "native safepoint telemetry must be optional");
  if (unmeasured_compilation.ok()) {
    const auto unmeasured =
        unmeasured_compilation.function->invoke(nullptr, 0, &context);
    expect(unmeasured.ok() && unmeasured.value == 73 &&
               context.safepoint_polls() == 0 && compilation.ok() &&
               unmeasured_compilation.function->stats().code_size <
                   compilation.function->stats().code_size,
           "disabled safepoint telemetry must add no execution counts");
  }
}

void test_differential_arithmetic() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "arithmetic function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }

  std::mt19937_64 random(0x554E494A4954ULL);
  for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
    const std::array<Word, 2> args = {static_cast<Word>(random()),
                                      static_cast<Word>(random())};
    const auto interpreted =
        Interpreter::evaluate(function, args.data(), args.size());
    const auto native = compilation.function->invoke(args.data(), args.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      expect(false, "native arithmetic must match the interpreter oracle");
      return;
    }
  }
}

Function float64_function() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value sum =
      builder.float64_add(builder.parameter(0), builder.parameter(1));
  const Value scaled =
      builder.float64_multiply(sum, builder.float64_constant(0.5));
  const Value result = builder.float64_subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "Float64 return must be accepted");
  return std::move(builder).build();
}

void test_float64_ir_and_interpreter() {
  const Function function = float64_function();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 SSA must pass verification");
  expect(function.return_type() == unijit::ir::ValueType::kFloat64,
         "Float64 result type must remain visible in the function");

  const std::array<Word, 2> args = {unijit::ir::pack_float64(19.25),
                                    unijit::ir::pack_float64(-4.75)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok(), "Float64 interpreter execution must succeed");
  expect(unijit::ir::unpack_float64(interpreted.value) == 12.0,
         "Float64 interpreter must preserve IEEE-754 arithmetic");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             optimization.function.return_type() ==
                 unijit::ir::ValueType::kFloat64,
         "optimizer must preserve Float64 types and operations");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 SSA must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  const std::array<double, 8> samples = {0.0,      -0.0,   1.25,   -7.5,
                                         1024.75, -33.125, 1.0e12, -1.0e-9};
  for (std::size_t lhs = 0; lhs < samples.size(); ++lhs) {
    const std::array<Word, 2> native_args = {
        unijit::ir::pack_float64(samples[lhs]),
        unijit::ir::pack_float64(samples[samples.size() - lhs - 1])};
    const auto expected =
        Interpreter::evaluate(function, native_args.data(), native_args.size());
    const auto native =
        compilation.function->invoke(native_args.data(), native_args.size());
    expect(native.ok() && expected.ok() && native.value == expected.value,
           "native Float64 arithmetic must match the interpreter bits");
  }
}

void test_float64_division() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value quotient =
      builder.float64_divide(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(quotient).ok(),
         "Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 division must pass verification");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 division must compile to native code");
  if (!compilation.ok()) {
    return;
  }

  constexpr std::array<std::array<double, 2>, 6> kSamples = {{
      {{9.0, 3.0}},
      {{-7.5, 2.5}},
      {{0.0, -3.0}},
      {{-0.0, 7.0}},
      {{1.0e-300, 2.0}},
      {{std::numeric_limits<double>::max(), 2.0}},
  }};
  for (const auto& sample : kSamples) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(sample[0]),
        unijit::ir::pack_float64(sample[1])};
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(interpreted.ok() && native.ok() &&
               native.value == interpreted.value,
           "native Float64 division must match the interpreter bits");
  }
}

void test_word_unary_operations() {
  FunctionBuilder negate_builder(1);
  expect(negate_builder
             .set_return(negate_builder.negate(negate_builder.parameter(0)))
             .ok(),
         "Word negation fixture must record its result");
  const Function negate_function = std::move(negate_builder).build();
  FunctionBuilder not_builder(1);
  expect(not_builder
             .set_return(not_builder.bitwise_not(not_builder.parameter(0)))
             .ok(),
         "Word bitwise-not fixture must record its result");
  const Function not_function = std::move(not_builder).build();
  expect(unijit::ir::verify(negate_function).ok() &&
             unijit::ir::verify(not_function).ok(),
         "typed Word unary operations must pass verification");

  const auto negate_compilation = Compiler::compile(negate_function);
  const auto not_compilation = Compiler::compile(not_function);
  expect(negate_compilation.ok() && not_compilation.ok(),
         "Word unary operations must compile to native code");
  constexpr std::array<Word, 7> kSamples = {
      0, 1, -1, 42, -97, std::numeric_limits<Word>::min(),
      std::numeric_limits<Word>::max()};
  for (const Word sample : kSamples) {
    const std::uint64_t bits = static_cast<std::uint64_t>(sample);
    const auto interpreted_negate =
        Interpreter::evaluate(negate_function, &sample, 1);
    const auto interpreted_not = Interpreter::evaluate(not_function, &sample, 1);
    expect(interpreted_negate.ok() &&
               static_cast<std::uint64_t>(interpreted_negate.value) ==
                   UINT64_C(0) - bits,
           "Word negation must wrap modulo 2^64");
    expect(interpreted_not.ok() &&
               static_cast<std::uint64_t>(interpreted_not.value) == ~bits,
           "Word bitwise-not must flip every bit");
    if (negate_compilation.ok() && not_compilation.ok()) {
      const auto native_negate =
          negate_compilation.function->invoke(&sample, 1);
      const auto native_not = not_compilation.function->invoke(&sample, 1);
      expect(native_negate.ok() && native_not.ok() &&
                 native_negate.value == interpreted_negate.value &&
                 native_not.value == interpreted_not.value,
             "native Word unary operations must match exact interpreter bits");
    }
  }

  FunctionBuilder optimized_builder(1);
  const Value double_negate = optimized_builder.negate(
      optimized_builder.negate(optimized_builder.parameter(0)));
  const Value double_not = optimized_builder.bitwise_not(
      optimized_builder.bitwise_not(optimized_builder.parameter(0)));
  expect(optimized_builder
             .set_return(optimized_builder.add(double_negate, double_not))
             .ok(),
         "Word unary optimization fixture must record its result");
  const auto optimized =
      unijit::ir::Optimizer::run(std::move(optimized_builder).build());
  expect(optimized.ok() && optimized.stats.algebraic_simplifications >= 2 &&
             optimized.function.nodes().size() == 2,
         "optimizer must cancel paired Word unary operations and remove them");

  unijit::ir::ControlFlowBuilder cfg_builder(1);
  const Value cfg_negate = cfg_builder.negate(cfg_builder.parameter(0));
  const Value cfg_not = cfg_builder.bitwise_not(cfg_builder.parameter(0));
  expect(cfg_builder.set_return(cfg_builder.add(cfg_negate, cfg_not)).ok(),
         "CFG Word unary fixture must record its result");
  const auto cfg_function = std::move(cfg_builder).build();
  expect(unijit::ir::verify(cfg_function).ok(),
         "CFG Word unary operations must pass verification");
  const auto cfg_compilation = Compiler::compile(cfg_function);
  expect(cfg_compilation.ok(), "CFG Word unary operations must compile");
  for (const Word sample : kSamples) {
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        cfg_function, &sample, 1);
    if (cfg_compilation.ok()) {
      const auto native = cfg_compilation.function->invoke(&sample, 1);
      expect(interpreted.ok() && native.ok() &&
                 native.value == interpreted.value,
             "native CFG Word unary operations must match the interpreter");
    }
  }

  unijit::ir::ControlFlowBuilder total_cfg_builder(2);
  const Value total_cfg_divide = total_cfg_builder.floor_divide(
      total_cfg_builder.parameter(0), total_cfg_builder.parameter(1));
  const Value total_cfg_modulo = total_cfg_builder.floor_modulo(
      total_cfg_builder.parameter(0), total_cfg_builder.parameter(1));
  expect(total_cfg_builder
             .set_return(total_cfg_builder.add(total_cfg_divide,
                                               total_cfg_modulo))
             .ok(),
         "total CFG floor fixture must record its result");
  const auto total_cfg_compilation = Compiler::compile(
      std::move(total_cfg_builder).build(),
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kBaseline});
  const std::array<Word, 2> total_zero_arguments = {7, 0};
  const auto total_zero_result =
      total_cfg_compilation.ok()
          ? total_cfg_compilation.function->invoke(total_zero_arguments.data(),
                                                   total_zero_arguments.size())
          : unijit::ir::EvaluationResult{};
  expect(total_cfg_compilation.ok() && total_zero_result.ok() &&
             total_zero_result.value == 0,
         "native CFG floor arithmetic must totalize a zero divisor");

  unijit::ir::ControlFlowBuilder cfg_optimized_builder(1);
  const Value cfg_double_not = cfg_optimized_builder.bitwise_not(
      cfg_optimized_builder.bitwise_not(
          cfg_optimized_builder.parameter(0)));
  expect(cfg_optimized_builder.set_return(cfg_double_not).ok(),
         "CFG unary optimization fixture must record its result");
  const auto cfg_optimized =
      unijit::ir::Optimizer::run(std::move(cfg_optimized_builder).build());
  expect(cfg_optimized.ok() &&
             cfg_optimized.stats.algebraic_simplifications >= 1 &&
             cfg_optimized.function.nodes().size() == 1,
         "CFG optimizer must cancel paired Word unary operations");

  FunctionBuilder malformed(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value invalid = malformed.bitwise_not(malformed.parameter(0));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Word unary operations over Float64 operands");
}

void test_word_bitwise_operations() {
  const auto build = [](unijit::ir::Opcode opcode) {
    FunctionBuilder builder(2);
    Value result;
    if (opcode == unijit::ir::Opcode::kBitwiseAnd) {
      result = builder.bitwise_and(builder.parameter(0), builder.parameter(1));
    } else if (opcode == unijit::ir::Opcode::kBitwiseOr) {
      result = builder.bitwise_or(builder.parameter(0), builder.parameter(1));
    } else {
      result = builder.bitwise_xor(builder.parameter(0), builder.parameter(1));
    }
    expect(builder.set_return(result).ok(),
           "Word bitwise fixture must record its result");
    return std::move(builder).build();
  };
  const Function and_function = build(unijit::ir::Opcode::kBitwiseAnd);
  const Function or_function = build(unijit::ir::Opcode::kBitwiseOr);
  const Function xor_function = build(unijit::ir::Opcode::kBitwiseXor);
  expect(unijit::ir::verify(and_function).ok() &&
             unijit::ir::verify(or_function).ok() &&
             unijit::ir::verify(xor_function).ok(),
         "typed Word bitwise operations must pass verification");
  const auto and_compilation = Compiler::compile(and_function);
  const auto or_compilation = Compiler::compile(or_function);
  const auto xor_compilation = Compiler::compile(xor_function);
  expect(and_compilation.ok() && or_compilation.ok() && xor_compilation.ok(),
         "Word bitwise operations must compile to native code");

  constexpr std::array<std::array<Word, 2>, 8> kSamples = {{
      {{0, 0}},
      {{-1, 0}},
      {{1, -1}},
      {{42, 21}},
      {{-97, 0x55}},
      {{std::numeric_limits<Word>::min(),
        std::numeric_limits<Word>::max()}},
      {{static_cast<Word>(UINT64_C(0x5555555555555555)),
        static_cast<Word>(UINT64_C(0xaaaaaaaaaaaaaaaa))}},
      {{static_cast<Word>(UINT64_C(0x0123456789abcdef)),
        static_cast<Word>(UINT64_C(0xfedcba9876543210))}},
  }};
  for (const auto& sample : kSamples) {
    const std::uint64_t lhs = static_cast<std::uint64_t>(sample[0]);
    const std::uint64_t rhs = static_cast<std::uint64_t>(sample[1]);
    const auto interpreted_and =
        Interpreter::evaluate(and_function, sample.data(), sample.size());
    const auto interpreted_or =
        Interpreter::evaluate(or_function, sample.data(), sample.size());
    const auto interpreted_xor =
        Interpreter::evaluate(xor_function, sample.data(), sample.size());
    expect(interpreted_and.ok() && interpreted_or.ok() &&
               interpreted_xor.ok() &&
               static_cast<std::uint64_t>(interpreted_and.value) ==
                   (lhs & rhs) &&
               static_cast<std::uint64_t>(interpreted_or.value) ==
                   (lhs | rhs) &&
               static_cast<std::uint64_t>(interpreted_xor.value) ==
                   (lhs ^ rhs),
           "Word bitwise interpreter semantics must preserve all 64 bits");
    if (and_compilation.ok() && or_compilation.ok() && xor_compilation.ok()) {
      const auto native_and =
          and_compilation.function->invoke(sample.data(), sample.size());
      const auto native_or =
          or_compilation.function->invoke(sample.data(), sample.size());
      const auto native_xor =
          xor_compilation.function->invoke(sample.data(), sample.size());
      expect(native_and.ok() && native_or.ok() && native_xor.ok() &&
                 native_and.value == interpreted_and.value &&
                 native_or.value == interpreted_or.value &&
                 native_xor.value == interpreted_xor.value,
             "native Word bitwise operations must match the interpreter");
    }
  }

  FunctionBuilder simplified_builder(1);
  const Value masked = simplified_builder.bitwise_and(
      simplified_builder.parameter(0), simplified_builder.constant(-1));
  const Value merged =
      simplified_builder.bitwise_or(masked, simplified_builder.constant(0));
  const Value restored =
      simplified_builder.bitwise_xor(merged, simplified_builder.constant(0));
  expect(simplified_builder.set_return(restored).ok(),
         "bitwise simplification fixture must record its result");
  const auto simplified =
      unijit::ir::Optimizer::run(std::move(simplified_builder).build());
  expect(simplified.ok() &&
             simplified.stats.algebraic_simplifications >= 3 &&
             simplified.function.nodes().size() == 1,
         "optimizer must apply Word bitwise identity and annihilator rules");

  unijit::ir::ControlFlowBuilder cfg_builder(2);
  const Value first = cfg_builder.bitwise_xor(cfg_builder.parameter(0),
                                               cfg_builder.parameter(1));
  const Value duplicate = cfg_builder.bitwise_xor(cfg_builder.parameter(0),
                                                   cfg_builder.parameter(1));
  const Value cfg_result = cfg_builder.bitwise_or(
      cfg_builder.bitwise_and(first, cfg_builder.parameter(0)), duplicate);
  expect(cfg_builder.set_return(cfg_result).ok(),
         "CFG Word bitwise fixture must record its result");
  const auto cfg_function = std::move(cfg_builder).build();
  expect(unijit::ir::verify(cfg_function).ok(),
         "CFG Word bitwise operations must pass verification");
  const auto cfg_optimization = unijit::ir::Optimizer::run(cfg_function);
  expect(cfg_optimization.ok() &&
             cfg_optimization.stats.common_subexpressions >= 1,
         "CFG optimizer must value-number duplicate Word bitwise operations");
  const auto cfg_compilation = Compiler::compile(cfg_function);
  expect(cfg_compilation.ok(), "CFG Word bitwise operations must compile");
  for (const auto& sample : kSamples) {
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        cfg_function, sample.data(), sample.size());
    if (cfg_compilation.ok()) {
      const auto native =
          cfg_compilation.function->invoke(sample.data(), sample.size());
      expect(interpreted.ok() && native.ok() &&
                 native.value == interpreted.value,
             "native CFG Word bitwise operations must match the interpreter");
    }
  }

  FunctionBuilder malformed(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value invalid =
      malformed.bitwise_and(malformed.parameter(0), malformed.parameter(1));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Word bitwise operations over Float64 operands");
}

void test_word_shift_operations() {
  FunctionBuilder builder(2);
  const Value shifted =
      builder.shift_left(builder.parameter(0), builder.parameter(1));
  const Value live_inputs =
      builder.add(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(builder.bitwise_xor(shifted, live_inputs)).ok(),
         "Word shift fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Word shifts must pass verification");
  const auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Word shifts must compile to native code");

  constexpr std::array<Word, 7> kValues = {
      0,
      1,
      -1,
      static_cast<Word>(UINT64_C(0x0123456789abcdef)),
      static_cast<Word>(UINT64_C(0x8000000000000000)),
      std::numeric_limits<Word>::min(),
      std::numeric_limits<Word>::max(),
  };
  constexpr std::array<Word, 13> kAmounts = {
      std::numeric_limits<Word>::min(), -65, -64, -63, -1, 0,  1,
      31, 63,                                64,  65, 127,
      std::numeric_limits<Word>::max(),
  };
  const auto expected_shift = [](Word value, Word amount) {
    const auto value_bits = static_cast<std::uint64_t>(value);
    const auto amount_bits = static_cast<std::uint64_t>(amount);
    if (amount < 0) {
      const std::uint64_t magnitude = UINT64_C(0) - amount_bits;
      return magnitude >= 64U ? UINT64_C(0) : value_bits >> magnitude;
    }
    return amount_bits >= 64U ? UINT64_C(0) : value_bits << amount_bits;
  };
  for (const Word value : kValues) {
    for (const Word amount : kAmounts) {
      const std::array<Word, 2> arguments = {value, amount};
      const std::uint64_t expected =
          expected_shift(value, amount) ^
          (static_cast<std::uint64_t>(value) +
           static_cast<std::uint64_t>(amount));
      const auto interpreted =
          Interpreter::evaluate(function, arguments.data(), arguments.size());
      expect(interpreted.ok() &&
                 static_cast<std::uint64_t>(interpreted.value) == expected,
             "Word shift interpreter must implement exact bidirectional semantics");
      if (compilation.ok()) {
        const auto native =
            compilation.function->invoke(arguments.data(), arguments.size());
        expect(native.ok() && native.value == interpreted.value,
               "native Word shifts must preserve live inputs and match the interpreter");
      }
    }
  }

  FunctionBuilder optimized_builder(1);
  const Value zero = optimized_builder.constant(0);
  const Value zero_shift =
      optimized_builder.shift_left(zero, optimized_builder.parameter(0));
  const Value identity =
      optimized_builder.shift_left(zero_shift, optimized_builder.constant(0));
  expect(optimized_builder.set_return(identity).ok(),
         "Word shift optimization fixture must record its result");
  const auto optimized =
      unijit::ir::Optimizer::run(std::move(optimized_builder).build());
  expect(optimized.ok() &&
             optimized.stats.algebraic_simplifications >= 1 &&
             optimized.stats.constants_folded >= 1,
         "optimizer must simplify zero values and zero shift amounts");

  FunctionBuilder folded_builder(0);
  expect(folded_builder
             .set_return(folded_builder.shift_left(
                 folded_builder.constant(-1),
                 folded_builder.constant(std::numeric_limits<Word>::min())))
             .ok(),
         "constant Word shift fixture must record its result");
  const auto folded =
      unijit::ir::Optimizer::run(std::move(folded_builder).build());
  expect(folded.ok() && folded.stats.constants_folded >= 1 &&
             folded.function.nodes().size() == 1 &&
             folded.function.nodes().front().immediate == 0,
         "optimizer must fold extreme signed shift amounts to zero");

  unijit::ir::ControlFlowBuilder cfg_builder(2);
  const Value cfg_first =
      cfg_builder.shift_left(cfg_builder.parameter(0), cfg_builder.parameter(1));
  const Value cfg_duplicate =
      cfg_builder.shift_left(cfg_builder.parameter(0), cfg_builder.parameter(1));
  expect(cfg_builder.set_return(cfg_builder.add(cfg_first, cfg_duplicate)).ok(),
         "CFG Word shift fixture must record its result");
  const auto cfg_function = std::move(cfg_builder).build();
  expect(unijit::ir::verify(cfg_function).ok(),
         "CFG Word shifts must pass verification");
  const auto cfg_optimized = unijit::ir::Optimizer::run(cfg_function);
  expect(cfg_optimized.ok() &&
             cfg_optimized.stats.common_subexpressions >= 1,
         "CFG optimizer must value-number duplicate Word shifts");
  const auto cfg_compilation = Compiler::compile(cfg_function);
  expect(cfg_compilation.ok(), "CFG Word shifts must compile to native code");
  for (const Word amount : kAmounts) {
    const std::array<Word, 2> arguments = {-1, amount};
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        cfg_function, arguments.data(), arguments.size());
    if (cfg_compilation.ok()) {
      const auto native =
          cfg_compilation.function->invoke(arguments.data(), arguments.size());
      expect(interpreted.ok() && native.ok() &&
                 native.value == interpreted.value,
             "native CFG Word shifts must match the interpreter");
    }
  }

  FunctionBuilder malformed(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value invalid =
      malformed.shift_left(malformed.parameter(0), malformed.parameter(1));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Word shifts over Float64 operands");
}

void test_word_floor_arithmetic() {
  FunctionBuilder divide_builder(2);
  const Value divided = divide_builder.floor_divide(
      divide_builder.parameter(0), divide_builder.parameter(1));
  expect(divide_builder
             .set_return(divide_builder.bitwise_xor(
                 divided,
                 divide_builder.bitwise_xor(divide_builder.parameter(0),
                                            divide_builder.parameter(1))))
             .ok(),
         "Word floor-division fixture must record its result");
  const Function divide_function = std::move(divide_builder).build();

  FunctionBuilder modulo_builder(2);
  const Value remainder = modulo_builder.floor_modulo(
      modulo_builder.parameter(0), modulo_builder.parameter(1));
  expect(modulo_builder
             .set_return(modulo_builder.bitwise_xor(
                 remainder,
                 modulo_builder.bitwise_xor(modulo_builder.parameter(0),
                                            modulo_builder.parameter(1))))
             .ok(),
         "Word floor-modulo fixture must record its result");
  const Function modulo_function = std::move(modulo_builder).build();
  expect(unijit::ir::verify(divide_function).ok() &&
             unijit::ir::verify(modulo_function).ok(),
         "typed Word floor arithmetic must pass verification");
  const auto divide_compilation = Compiler::compile(divide_function);
  const auto modulo_compilation = Compiler::compile(modulo_function);
  expect(divide_compilation.ok() && modulo_compilation.ok(),
         "Word floor arithmetic must compile to native code");

  constexpr std::array<Word, 12> kValues = {
      std::numeric_limits<Word>::min(), -17, -7, -3, -1, 0,
      1,                                  2,   3,  7,
      std::numeric_limits<Word>::max(), 3776918176276767148LL,
  };
  for (const Word lhs : kValues) {
    for (const Word rhs : kValues) {
      const std::array<Word, 2> arguments = {lhs, rhs};
      const Word preserved = lhs ^ rhs;
      const Word expected_divide =
          unijit::ir::floor_divide_word(lhs, rhs) ^ preserved;
      const Word expected_modulo =
          unijit::ir::floor_modulo_word(lhs, rhs) ^ preserved;
      const auto interpreted_divide = Interpreter::evaluate(
          divide_function, arguments.data(), arguments.size());
      const auto interpreted_modulo = Interpreter::evaluate(
          modulo_function, arguments.data(), arguments.size());
      expect(interpreted_divide.ok() && interpreted_modulo.ok() &&
                 interpreted_divide.value == expected_divide &&
                 interpreted_modulo.value == expected_modulo,
             "Word floor arithmetic interpreter must match the exact signed contract");
      if (divide_compilation.ok() && modulo_compilation.ok()) {
        const auto native_divide = divide_compilation.function->invoke(
            arguments.data(), arguments.size());
        const auto native_modulo = modulo_compilation.function->invoke(
            arguments.data(), arguments.size());
        expect(native_divide.ok() && native_modulo.ok() &&
                   native_divide.value == expected_divide &&
                   native_modulo.value == expected_modulo,
               "native Word floor arithmetic must preserve live operands and match the interpreter");
      }
    }
  }

  FunctionBuilder guarded_builder(2);
  expect(guarded_builder.guard_word_nonzero(guarded_builder.parameter(1), 91)
             .valid(),
         "Word nonzero guard must produce an SSA value");
  expect(guarded_builder
             .set_return(guarded_builder.floor_divide(
                 guarded_builder.parameter(0), guarded_builder.parameter(1)))
             .ok(),
         "guarded division fixture must record its result");
  const Function guarded_function = std::move(guarded_builder).build();
  const auto guarded_compilation = Compiler::compile(guarded_function);
  expect(guarded_compilation.ok() && guarded_compilation.function->requires_context(),
         "Word guards must compile with execution-context support");
  unijit::runtime::ExecutionContext context;
  const std::array<Word, 2> zero_arguments = {7, 0};
  if (guarded_compilation.ok()) {
    const auto exited = guarded_compilation.function->invoke(
        zero_arguments.data(), zero_arguments.size(), &context);
    expect(!exited.ok() &&
               exited.status.code() == unijit::StatusCode::kRuntimeExit &&
               exited.status.location() == 91 && context.exit_site() == 91 &&
               context.exit_value() == 0,
           "native Word guard must report a precise runtime exit on zero");
  }

  unijit::ir::ControlFlowBuilder cfg_builder(2);
  expect(cfg_builder.guard_word_nonzero(cfg_builder.parameter(1), 92).valid(),
         "CFG Word nonzero guard must produce an SSA value");
  const Value cfg_divide = cfg_builder.floor_divide(
      cfg_builder.parameter(0), cfg_builder.parameter(1));
  const Value cfg_modulo = cfg_builder.floor_modulo(
      cfg_builder.parameter(0), cfg_builder.parameter(1));
  expect(cfg_builder.set_return(cfg_builder.add(cfg_divide, cfg_modulo)).ok(),
         "CFG floor-arithmetic fixture must record its result");
  const auto cfg_function = std::move(cfg_builder).build();
  expect(unijit::ir::verify(cfg_function).ok(),
         "CFG Word guards and floor arithmetic must pass verification");
  const auto cfg_optimized = unijit::ir::Optimizer::run(cfg_function);
  expect(cfg_optimized.ok(),
         "CFG optimizer must preserve Word guards and floor arithmetic");
  const auto cfg_compilation = Compiler::compile(cfg_function);
  expect(cfg_compilation.ok(),
         "CFG Word guards and floor arithmetic must compile");
  if (cfg_compilation.ok()) {
    for (const Word lhs : kValues) {
      for (const Word rhs : kValues) {
        if (rhs == 0) {
          continue;
        }
        const std::array<Word, 2> cfg_arguments = {lhs, rhs};
        const auto native = cfg_compilation.function->invoke(
            cfg_arguments.data(), cfg_arguments.size(), &context);
        expect(native.ok() &&
                   native.value == unijit::ir::floor_divide_word(lhs, rhs) +
                                       unijit::ir::floor_modulo_word(lhs, rhs),
               "native CFG floor arithmetic must implement signed floor semantics");
      }
    }
  }

  FunctionBuilder malformed(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value invalid = malformed.floor_divide(malformed.parameter(0),
                                                malformed.parameter(1));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Word floor arithmetic over Float64 operands");
}

void test_float64_negation() {
  using unijit::ir::ValueType;
  FunctionBuilder builder(std::vector<ValueType>{ValueType::kFloat64});
  const Value negated = builder.float64_negate(builder.parameter(0));
  expect(builder.set_return(negated).ok(),
         "Float64 negation fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok() &&
             function.return_type() == ValueType::kFloat64,
         "Float64 negation must preserve its operand type");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 negation must compile to native code");
  constexpr std::array<std::uint64_t, 8> kSamples = {
      UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
      UINT64_C(0x3ff4000000000000), UINT64_C(0xbff4000000000000),
      UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
      UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000005678)};
  for (const std::uint64_t bits : kSamples) {
    const Word argument = static_cast<Word>(bits);
    const auto interpreted = Interpreter::evaluate(function, &argument, 1);
    expect(interpreted.ok() &&
               static_cast<std::uint64_t>(interpreted.value) ==
                   (bits ^ (UINT64_C(1) << 63U)),
           "interpreter negation must only toggle the Float64 sign bit");
    if (compilation.ok()) {
      const auto native = compilation.function->invoke(&argument, 1);
      expect(native.ok() && native.value == interpreted.value,
             "native negation must preserve exact zero, infinity, and NaN bits");
    }
  }

  FunctionBuilder constant_builder(0);
  const Value constant_nan = constant_builder.float64_constant_bits(
      static_cast<Word>(UINT64_C(0x7ff8000000001234)));
  expect(constant_builder
             .set_return(constant_builder.float64_negate(constant_nan))
             .ok(),
         "constant Float64 negation fixture must record its result");
  const auto optimized =
      unijit::ir::Optimizer::run(std::move(constant_builder).build());
  expect(optimized.ok() && optimized.stats.constants_folded == 1 &&
             optimized.function.nodes().size() == 1 &&
             static_cast<std::uint64_t>(
                 optimized.function.nodes()[0].immediate) ==
                 UINT64_C(0xfff8000000001234),
         "optimizer must fold Float64 negation without canonicalizing NaN");

  FunctionBuilder malformed(1);
  const Value invalid = malformed.float64_negate(malformed.parameter(0));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Float64 negation over a Word operand");
}

void test_word_comparisons() {
  FunctionBuilder builder(2);
  const Value lhs = builder.parameter(0);
  const Value rhs = builder.parameter(1);
  const Value less = builder.less_than(lhs, rhs);
  const Value less_equal = builder.less_equal(lhs, rhs);
  const Value equal = builder.equal(lhs, rhs);
  const Value not_equal = builder.not_equal(lhs, rhs);
  const Value encoded = builder.add(
      builder.multiply(less, builder.constant(1000)),
      builder.add(builder.multiply(less_equal, builder.constant(100)),
                  builder.add(builder.multiply(equal, builder.constant(10)),
                              not_equal)));
  expect(builder.set_return(encoded).ok(),
         "Word comparison fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Word comparisons must satisfy straight-line verification");
  const auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Word comparisons must compile to native code");

  const auto check = [&](Word left, Word right, Word expected,
                         const char* message) {
    const std::array<Word, 2> arguments = {left, right};
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == expected, message);
    if (compilation.ok()) {
      const auto native =
          compilation.function->invoke(arguments.data(), arguments.size());
      expect(native.ok() && native.value == interpreted.value,
             "native Word comparisons must match the interpreter");
    }
  };
  check(std::numeric_limits<Word>::min(), std::numeric_limits<Word>::max(),
        1101, "signed Word minimum must compare below the maximum");
  check(-1, 0, 1101, "negative Word values must compare below zero");
  check(0, 0, 110, "equal Word values must set inclusive and equality flags");
  check(1, 0, 1, "greater Word values must only set inequality");
  check(std::numeric_limits<Word>::max(), std::numeric_limits<Word>::min(), 1,
        "signed Word maximum must compare above the minimum");

  FunctionBuilder constant_builder(0);
  const Value constant_comparison = constant_builder.not_equal(
      constant_builder.constant(-17), constant_builder.constant(5));
  expect(constant_builder.set_return(constant_comparison).ok(),
         "constant Word comparison fixture must record its result");
  const auto constant_optimized =
      unijit::ir::Optimizer::run(std::move(constant_builder).build());
  expect(constant_optimized.ok() &&
             constant_optimized.function.nodes().size() == 1 &&
             constant_optimized.function.nodes()[0].opcode ==
                 unijit::ir::Opcode::kConstant &&
             constant_optimized.function.nodes()[0].immediate == 1,
         "optimizer must fold constant Word comparisons to canonical flags");

  FunctionBuilder identity_builder(1);
  const Value identity = identity_builder.equal(identity_builder.parameter(0),
                                                identity_builder.parameter(0));
  expect(identity_builder.set_return(identity).ok(),
         "Word comparison identity fixture must record its result");
  const auto identity_optimized =
      unijit::ir::Optimizer::run(std::move(identity_builder).build());
  expect(identity_optimized.ok() &&
             identity_optimized.stats.algebraic_simplifications == 1 &&
             identity_optimized.function.nodes().size() == 2 &&
             identity_optimized.function
                     .nodes()[identity_optimized.function.return_value().id()]
                     .opcode == unijit::ir::Opcode::kConstant &&
             identity_optimized.function
                     .nodes()[identity_optimized.function.return_value().id()]
                     .immediate == 1,
         "optimizer must reduce self equality to canonical true");

  using unijit::ir::ValueType;
  FunctionBuilder malformed(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value invalid =
      malformed.equal(malformed.parameter(0), malformed.parameter(1));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Word comparisons over Float64 operands");

  unijit::ir::ControlFlowBuilder cfg_builder(2);
  const Value cfg_lhs = cfg_builder.parameter(0);
  const Value cfg_rhs = cfg_builder.parameter(1);
  const Value cfg_encoded = cfg_builder.add(
      cfg_builder.multiply(cfg_builder.less_than(cfg_lhs, cfg_rhs),
                           cfg_builder.constant(1000)),
      cfg_builder.add(
          cfg_builder.multiply(cfg_builder.less_equal(cfg_lhs, cfg_rhs),
                               cfg_builder.constant(100)),
          cfg_builder.add(
              cfg_builder.multiply(cfg_builder.equal(cfg_lhs, cfg_rhs),
                                   cfg_builder.constant(10)),
              cfg_builder.not_equal(cfg_lhs, cfg_rhs))));
  expect(cfg_builder.set_return(cfg_encoded).ok(),
         "CFG Word comparison fixture must record its result");
  const auto cfg_function = std::move(cfg_builder).build();
  expect(unijit::ir::verify(cfg_function).ok(),
         "CFG Word comparisons must satisfy verification");
  const auto cfg_compilation = Compiler::compile(cfg_function);
  expect(cfg_compilation.ok(),
         "CFG Word comparisons must compile to native code");
  constexpr std::array<std::array<Word, 2>, 5> kPairs = {{
      {std::numeric_limits<Word>::min(), std::numeric_limits<Word>::max()},
      {-1, 0},
      {0, 0},
      {1, 0},
      {std::numeric_limits<Word>::max(), std::numeric_limits<Word>::min()},
  }};
  for (const auto& arguments : kPairs) {
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        cfg_function, arguments.data(), arguments.size());
    if (cfg_compilation.ok()) {
      const auto native =
          cfg_compilation.function->invoke(arguments.data(), arguments.size());
      expect(
          interpreted.ok() && native.ok() && native.value == interpreted.value,
          "native CFG Word comparisons must match signed interpreter "
          "semantics");
    }
  }

  unijit::ir::ControlFlowBuilder cse_builder(2);
  const Value first =
      cse_builder.equal(cse_builder.parameter(0), cse_builder.parameter(1));
  const Value duplicate =
      cse_builder.equal(cse_builder.parameter(0), cse_builder.parameter(1));
  expect(cse_builder.set_return(cse_builder.add(first, duplicate)).ok(),
         "CFG comparison CSE fixture must record its result");
  const auto cse_optimized =
      unijit::ir::Optimizer::run(std::move(cse_builder).build());
  expect(cse_optimized.ok() && cse_optimized.stats.common_subexpressions == 1,
         "CFG optimizer must value-number duplicate Word comparisons");

  unijit::ir::ControlFlowBuilder malformed_cfg(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value invalid_cfg = malformed_cfg.not_equal(malformed_cfg.parameter(0),
                                                    malformed_cfg.parameter(1));
  expect(malformed_cfg.set_return(invalid_cfg).ok() &&
             !unijit::ir::verify(std::move(malformed_cfg).build()).ok(),
         "CFG verifier must reject Word comparisons over Float64 operands");
}

void test_float64_comparisons() {
  using unijit::ir::ValueType;
  FunctionBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value less =
      builder.float64_less_than(builder.parameter(0), builder.parameter(1));
  const Value less_equal =
      builder.float64_less_equal(builder.parameter(0), builder.parameter(1));
  const Value equal =
      builder.float64_equal(builder.parameter(0), builder.parameter(1));
  const Value not_equal =
      builder.float64_not_equal(builder.parameter(0), builder.parameter(1));
  const Value encoded = builder.add(
      builder.multiply(less, builder.constant(1000)),
      builder.add(builder.multiply(less_equal, builder.constant(100)),
                  builder.add(builder.multiply(equal, builder.constant(10)),
                              not_equal)));
  expect(builder.set_return(encoded).ok(),
         "Float64 comparison fixture must record its Word result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok() &&
             function.return_type() == ValueType::kWord,
         "Float64 comparisons must accept Float64 operands and return Word");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 comparisons must compile to native code");
  const auto check = [&](double lhs, double rhs, Word expected,
                         const char* message) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == expected, message);
    if (compilation.ok()) {
      const auto native =
          compilation.function->invoke(arguments.data(), arguments.size());
      expect(native.ok() && native.value == interpreted.value,
             "native Float64 comparison must match the interpreter");
    }
  };
  check(1.0, 2.0, 1101,
        "less operands must set ordered and inequality flags");
  check(2.0, 2.0, 110,
        "equal operands must set inclusive and equality flags");
  check(3.0, 2.0, 1,
        "greater operands must only set the inequality flag");
  check(0.0, -0.0, 110,
        "signed zeroes must compare equal under Float64 semantics");
  check(std::numeric_limits<double>::quiet_NaN(), 2.0, 1,
        "unordered operands must compare unequal");

  FunctionBuilder constant_builder(0);
  const Value constant_result = constant_builder.float64_less_than(
      constant_builder.float64_constant(1.0),
      constant_builder.float64_constant(2.0));
  expect(constant_builder.set_return(constant_result).ok(),
         "constant Float64 comparison fixture must record its result");
  const auto optimized =
      unijit::ir::Optimizer::run(std::move(constant_builder).build());
  expect(optimized.ok() && optimized.function.return_type() == ValueType::kWord &&
             optimized.function.nodes().size() == 1 &&
             optimized.function.nodes()[0].opcode ==
                 unijit::ir::Opcode::kConstant &&
             optimized.function.nodes()[0].immediate == 1,
         "optimizer must fold a constant Float64 comparison to a Word");

  FunctionBuilder unordered_builder(0);
  const Value nan = unordered_builder.float64_constant(
      std::numeric_limits<double>::quiet_NaN());
  const Value unordered_equal = unordered_builder.float64_equal(nan, nan);
  const Value unordered_not_equal =
      unordered_builder.float64_not_equal(nan, nan);
  const Value unordered_result = unordered_builder.add(
      unordered_builder.multiply(unordered_equal,
                                 unordered_builder.constant(10)),
      unordered_not_equal);
  expect(unordered_builder.set_return(unordered_result).ok(),
         "unordered comparison fixture must record its result");
  const auto unordered_optimized =
      unijit::ir::Optimizer::run(std::move(unordered_builder).build());
  expect(unordered_optimized.ok() &&
             unordered_optimized.function.nodes().size() == 1 &&
             unordered_optimized.function.nodes()[0].opcode ==
                 unijit::ir::Opcode::kConstant &&
             unordered_optimized.function.nodes()[0].immediate == 1,
         "optimizer must fold NaN equality and inequality exactly");

  FunctionBuilder malformed(2);
  const Value invalid =
      malformed.float64_less_than(malformed.parameter(0),
                                  malformed.parameter(1));
  expect(malformed.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "verifier must reject Float64 comparison over Word operands");
}

void test_float64_nonzero_guard() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value numerator = builder.parameter(0);
  const Value divisor = builder.parameter(1);
  const Value guard = builder.guard_float64_nonzero(divisor, 77);
  expect(guard.valid(), "Float64 nonzero guard must produce an effect value");
  const Value quotient = builder.float64_divide(numerator, divisor);
  expect(builder.set_return(quotient).ok(),
         "guarded Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 nonzero guard must pass verification");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::any_of(optimization.function.nodes().begin(),
                         optimization.function.nodes().end(),
                         [](const unijit::ir::Node& node) {
                           return node.opcode ==
                                  unijit::ir::Opcode::kGuardFloatNonzero;
                         }),
         "optimizer must preserve a dead-result Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 nonzero guard must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  const auto* default_deoptimization =
      compilation.function->deoptimization_record(77);
  const auto* stack_map = compilation.function->stack_map(77);
  expect(default_deoptimization != nullptr &&
             default_deoptimization->reason ==
                 unijit::runtime::DeoptimizationReason::kGuardFailed &&
             default_deoptimization->recovery.size() == 1,
         "generic guards must receive default reconstruction metadata");
  expect(stack_map != nullptr &&
             stack_map->kind == unijit::jit::StackMapKind::kGuard &&
             stack_map->native_offset <
                 compilation.function->stats().code_size &&
             compilation.function->stats().stack_map_count == 1 &&
             compilation.function->stats().stack_map_value_count == 2 &&
             stack_map->live_values.size() == 2 &&
             std::all_of(stack_map->live_values.begin(),
                         stack_map->live_values.end(),
                         [stack_map](const unijit::jit::StackMapValue& value) {
                           return value.type ==
                                      unijit::ir::ValueType::kFloat64 &&
                                  value.frame_offset < stack_map->frame_size;
                         }),
         "Float64 guards must retain every live typed value in canonical frame slots");

  const std::array<Word, 2> valid = {unijit::ir::pack_float64(9.0),
                                     unijit::ir::pack_float64(3.0)};
  const auto interpreted =
      Interpreter::evaluate(function, valid.data(), valid.size());
  const auto native = compilation.function->invoke(valid.data(), valid.size());
  expect(interpreted.ok() && native.ok() &&
             unijit::ir::unpack_float64(native.value) == 3.0,
         "a nonzero Float64 divisor must pass the guard");

  constexpr std::array<double, 2> kZeroes = {0.0, -0.0};
  for (double zero : kZeroes) {
    const std::array<Word, 2> invalid = {unijit::ir::pack_float64(9.0),
                                         unijit::ir::pack_float64(zero)};
    unijit::runtime::ExecutionContext interpreter_context;
    const auto rejected_by_interpreter = Interpreter::evaluate(
        function, invalid.data(), invalid.size(), &interpreter_context);
    expect(!rejected_by_interpreter.ok() &&
               rejected_by_interpreter.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_by_interpreter.status.location() == 77 &&
               interpreter_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime &&
               interpreter_context.exit_value() == invalid[1],
           "interpreter guard must reject both signed Float64 zeroes");

    const auto rejected_with_local_context =
        compilation.function->invoke(invalid.data(), invalid.size());
    expect(!rejected_with_local_context.ok() &&
               rejected_with_local_context.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_with_local_context.status.location() == 77,
           "managed invocation must diagnose a guard without caller context");

    unijit::runtime::ExecutionContext native_context;
    const auto rejected_by_native = compilation.function->invoke(
        invalid.data(), invalid.size(), &native_context);
    const auto captured =
        compilation.function->reconstruct_stack_map(native_context);
    const auto* captured_numerator = captured.capture.find(numerator);
    const auto* captured_divisor = captured.capture.find(divisor);
    expect(!rejected_by_native.ok() &&
               native_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime &&
               native_context.exit_site() == 77 &&
               native_context.exit_value() == invalid[1] && captured.ok() &&
               captured.capture.kind == unijit::jit::StackMapKind::kGuard &&
               captured_numerator != nullptr &&
               captured_numerator->value_bits == invalid[0] &&
               captured_divisor != nullptr &&
               captured_divisor->value_bits == invalid[1],
           "native guards must publish their exit and captured live Float64 bits");
  }
}

void test_deoptimization_reconstruction() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value divisor = builder.parameter(1);
  const Value snapshot = builder.float64_add(
      builder.parameter(0), builder.float64_constant(1.5));
  expect(builder.guard_float64_nonzero(divisor, 113).valid(),
         "deoptimization fixture must create its guard");
  const Value quotient =
      builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "deoptimization fixture must record its result");
  const Function function = std::move(builder).build();

  unijit::runtime::DeoptimizationRecord record;
  record.site = 113;
  record.resume_offset = 29;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          0, unijit::ir::ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::argument(
          1, unijit::ir::ValueType::kFloat64, 1),
      unijit::runtime::RecoveryOperation::constant_value(
          2, unijit::ir::ValueType::kWord, 41),
      unijit::runtime::RecoveryOperation::exit_value(
          3, unijit::ir::ValueType::kFloat64),
      unijit::runtime::RecoveryOperation::captured_value(
          4, unijit::ir::ValueType::kFloat64, snapshot)};
  unijit::runtime::DeoptimizationTable metadata;
  expect(metadata.add(record).ok(),
         "valid deoptimization metadata must be accepted");
  expect(!metadata.add(record).ok(),
         "duplicate deoptimization sites must be rejected");

  auto compilation = Compiler::compile(function, metadata);
  expect(compilation.ok(),
         "a function with explicit deoptimization metadata must compile");
  if (!compilation.ok()) {
    return;
  }
  const auto* compiled_record =
      compilation.function->deoptimization_record(113);
  expect(compilation.function->deoptimization_table().size() == 1 &&
             compiled_record != nullptr &&
             compiled_record->reason ==
                 unijit::runtime::DeoptimizationReason::kDivisionByZero &&
             compiled_record->resume_offset == 29 &&
             compiled_record->recovery.back().capture_resolved(),
         "compiled functions must retain immutable deoptimization records");

  const std::array<Word, 2> arguments = {
      unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(-0.0)};
  unijit::runtime::ExecutionContext context;
  const auto exited = compilation.function->invoke(
      arguments.data(), arguments.size(), &context);
  expect(!exited.ok() &&
             exited.status.code() == unijit::StatusCode::kRuntimeExit &&
             context.exit_value() == arguments[1],
         "a guarded exit must retain the exact triggering value bits");

  const auto reconstruction =
      compilation.function->reconstruct_deoptimization(
          exited.status.location(), arguments.data(), arguments.size(),
          context);
  const auto* recovered_argument = reconstruction.frame.find(0);
  const auto* recovered_constant = reconstruction.frame.find(2);
  const auto* recovered_exit = reconstruction.frame.find(3);
  const auto* recovered_snapshot = reconstruction.frame.find(4);
  expect(reconstruction.ok() && reconstruction.frame.site == 113 &&
             reconstruction.frame.resume_offset == 29 &&
             reconstruction.frame.reason ==
                 unijit::runtime::DeoptimizationReason::kDivisionByZero &&
             recovered_argument != nullptr &&
             recovered_argument->value == arguments[0] &&
             recovered_constant != nullptr && recovered_constant->value == 41 &&
             recovered_exit != nullptr &&
             recovered_exit->value == arguments[1] &&
             recovered_snapshot != nullptr &&
             recovered_snapshot->value == unijit::ir::pack_float64(10.5),
         "deoptimization must reconstruct arguments, constants, exits, and metadata-only SSA values");

  auto baseline = Compiler::compile(
      function, metadata, unijit::runtime::AssumptionSet{},
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kBaseline});
  unijit::runtime::ExecutionContext baseline_context;
  expect(baseline.ok(),
         "baseline compilation must retain captured recovery values");
  if (baseline.ok()) {
    const auto baseline_exit = baseline.function->invoke(
        arguments.data(), arguments.size(), &baseline_context);
    const auto baseline_frame = baseline.function->reconstruct_deoptimization(
        113, arguments.data(), arguments.size(), baseline_context);
    expect(!baseline_exit.ok() && baseline_frame.ok() &&
               baseline_frame.frame.find(4) != nullptr &&
               baseline_frame.frame.find(4)->value ==
                   unijit::ir::pack_float64(10.5),
           "baseline guards must capture metadata-only SSA values exactly");
  }

  unijit::runtime::ExecutionContext mismatched_context;
  mismatched_context.record_exit(unijit::runtime::ExitReason::kRuntime, 114);
  expect(!compilation.function
              ->reconstruct_deoptimization(113, arguments.data(),
                                           arguments.size(),
                                           mismatched_context)
              .ok(),
         "reconstruction must reject stale or mismatched execution contexts");

  unijit::jit::CodeCache cache;
  const auto publication = cache.publish(
      "deoptimization-fixture", 1, std::move(compilation.function));
  expect(publication.ok() &&
             publication.handle.deoptimization_record(113) != nullptr &&
             publication.handle.stack_maps() != nullptr &&
             publication.handle.stack_maps()->size() == 1 &&
             publication.handle.stack_map(113) != nullptr,
         "code-cache leases must retain deoptimization and stack-map metadata");
  cache.clear();
  const auto cached_reconstruction =
      publication.handle.reconstruct_deoptimization(
          113, arguments.data(), arguments.size(), context);
  const auto cached_stack_map =
      publication.handle.reconstruct_stack_map(context);
  expect(cached_reconstruction.ok() &&
             cached_reconstruction.frame.find(3) != nullptr &&
             cached_reconstruction.frame.find(3)->value == arguments[1] &&
             cached_reconstruction.frame.find(4) != nullptr &&
             cached_reconstruction.frame.find(4)->value ==
                 unijit::ir::pack_float64(10.5) &&
             cached_stack_map.ok() &&
             cached_stack_map.capture.values.size() == 3,
         "cached execution leases must reconstruct diagnosed exits and live SSA values");

  unijit::runtime::DeoptimizationRecord unavailable = record;
  unavailable.recovery.back() =
      unijit::runtime::RecoveryOperation::captured_value(
          4, unijit::ir::ValueType::kFloat64, quotient);
  unijit::runtime::DeoptimizationTable unavailable_metadata;
  expect(unavailable_metadata.add(unavailable).ok() &&
             !Compiler::compile(function, unavailable_metadata).ok(),
         "captured recovery must reject SSA values defined after the guard");

  unijit::runtime::DeoptimizationRecord wrong_type = record;
  wrong_type.recovery.back() =
      unijit::runtime::RecoveryOperation::captured_value(
          4, unijit::ir::ValueType::kWord, snapshot);
  unijit::runtime::DeoptimizationTable wrong_type_metadata;
  expect(wrong_type_metadata.add(wrong_type).ok() &&
             !Compiler::compile(function, wrong_type_metadata).ok(),
         "captured recovery must reject types inconsistent with SSA");

  unijit::runtime::DeoptimizationRecord unknown_site = record;
  unknown_site.site = 999;
  unijit::runtime::DeoptimizationTable invalid_metadata;
  expect(invalid_metadata.add(unknown_site).ok() &&
             !Compiler::compile(function, invalid_metadata).ok(),
         "compilation must reject metadata for a nonexistent guard site");

  auto colliding_assumption =
      std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet colliding_assumptions;
  expect(colliding_assumptions.add(colliding_assumption, 113, 29).ok() &&
             !Compiler::compile(function, metadata, colliding_assumptions)
                  .ok(),
         "runtime guards and assumptions must not share an exit site");
}

void test_transactional_object_materialization() {
  unijit::runtime::ReconstructedFrame frame;
  frame.reason = unijit::runtime::DeoptimizationReason::kTypeMismatch;
  frame.site = 401;
  frame.resume_offset = 73;
  frame.values = {
      {0, unijit::ir::ValueType::kWord, 42},
      {1, unijit::ir::ValueType::kFloat64,
       unijit::ir::pack_float64(-0.0)}};

  unijit::runtime::MaterializationPlan plan(401, 73);
  expect(plan
             .add({7,
                   10,
                   101,
                   {unijit::runtime::MaterializationInput::recovered(
                        0, unijit::ir::ValueType::kWord),
                    unijit::runtime::MaterializationInput::object(9)}})
             .ok() &&
             plan
                 .add({9,
                       11,
                       202,
                       {unijit::runtime::MaterializationInput::constant_value(
                            unijit::ir::ValueType::kFloat64,
                            unijit::ir::pack_float64(3.5)),
                        unijit::runtime::MaterializationInput::object(7),
                        unijit::runtime::MaterializationInput::recovered(
                            1, unijit::ir::ValueType::kFloat64)}})
                 .ok() &&
             plan.validate().ok(),
         "cyclic object materialization recipes must validate after all objects are declared");

  TestMaterializer state;
  const auto materialized = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&state));
  const auto* first_slot = materialized.frame.find(10);
  const auto* second_slot = materialized.frame.find(11);
  expect(materialized.ok() && materialized.frame.site == 401 &&
             materialized.frame.resume_offset == 73 &&
             materialized.frame.values.size() == 4 && first_slot != nullptr &&
             first_slot->kind ==
                 unijit::runtime::MaterializedValueKind::kObject &&
             first_slot->object == 1001 && second_slot != nullptr &&
             second_slot->object == 1002 && state.begin_count == 1 &&
             state.reason ==
                 unijit::runtime::DeoptimizationReason::kTypeMismatch &&
             state.site == 401 && state.resume_offset == 73 &&
             state.frame_value_count == 4 &&
             state.commit_count == 1 && state.rollback_count == 0 &&
             state.committed.size() == 2 &&
             state.committed[0].fields[0].primitive == 42 &&
             state.committed[0].fields[1].is_object &&
             state.committed[0].fields[1].object == 1002 &&
             state.committed[1].fields[0].primitive ==
                 unijit::ir::pack_float64(3.5) &&
             state.committed[1].fields[1].is_object &&
             state.committed[1].fields[1].object == 1001 &&
             state.committed[1].fields[2].primitive ==
                 unijit::ir::pack_float64(-0.0) &&
             state.committed_frame.size() == 4 &&
             state.committed_frame[0].slot == 0 &&
             state.committed_frame[0].value == 42 &&
             state.committed_frame[1].slot == 1 &&
             state.committed_frame[1].kind ==
                 unijit::runtime::MaterializedValueKind::kFloat64 &&
             state.committed_frame[1].value ==
                 unijit::ir::pack_float64(-0.0) &&
             state.committed_frame[2].slot == 10 &&
             state.committed_frame[2].object == 1001 &&
             state.committed_frame[3].slot == 11 &&
             state.committed_frame[3].object == 1002,
         "atomic materialization must preserve metadata, primitive bits, object identity, cycles, and the logical frame");

  TestMaterializer failing_state;
  failing_state.fail_store = 1;
  const auto failed = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&failing_state));
  expect(!failed.ok() && failing_state.begin_count == 1 &&
             failing_state.commit_count == 0 &&
             failing_state.rollback_count == 1 &&
             failing_state.staged.empty() &&
             failing_state.staged_frame.empty() &&
             failing_state.committed.empty(),
         "field failures must roll back partially materialized object graphs exactly once");

  TestMaterializer frame_failure;
  frame_failure.fail_frame_store = 2;
  const auto failed_frame = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&frame_failure));
  expect(!failed_frame.ok() && frame_failure.begin_count == 1 &&
             frame_failure.frame_store_count == 3 &&
             frame_failure.commit_count == 0 &&
             frame_failure.rollback_count == 1 &&
             frame_failure.staged.empty() &&
             frame_failure.staged_frame.empty() &&
             frame_failure.committed.empty() &&
             frame_failure.committed_frame.empty(),
         "frame installation failures must roll back the object graph and logical frame together");

  TestMaterializer begin_failure;
  begin_failure.fail_begin = true;
  const auto failed_begin = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&begin_failure));
  expect(!failed_begin.ok() && begin_failure.begin_count == 1 &&
             begin_failure.rollback_count == 1 &&
             begin_failure.commit_count == 0,
         "failed transaction setup must invoke the rollback contract exactly once");

  TestMaterializer allocation_failure;
  allocation_failure.fail_allocate = 1;
  const auto failed_allocation = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&allocation_failure));
  expect(!failed_allocation.ok() && allocation_failure.begin_count == 1 &&
             allocation_failure.allocate_count == 2 &&
             allocation_failure.rollback_count == 1 &&
             allocation_failure.commit_count == 0 &&
             allocation_failure.staged.empty(),
         "object allocation failure must discard every staged shell");

  TestMaterializer commit_failure;
  commit_failure.fail_commit = true;
  const auto failed_commit = unijit::runtime::materialize_frame(
      frame, plan, materialization_callbacks(&commit_failure));
  expect(!failed_commit.ok() && commit_failure.begin_count == 1 &&
             commit_failure.commit_count == 0 &&
             commit_failure.rollback_count == 1 &&
             commit_failure.staged.empty() &&
             commit_failure.staged_frame.empty() &&
             commit_failure.committed.empty(),
         "commit failure must roll back a fully populated object graph and logical frame");

  unijit::runtime::MaterializationPlan unknown_reference(401, 73);
  expect(unknown_reference
             .add({1,
                   12,
                   0,
                   {unijit::runtime::MaterializationInput::object(999)}})
             .ok(),
         "forward object references must be accepted during plan assembly");
  TestMaterializer untouched_state;
  expect(!unijit::runtime::materialize_frame(
              frame, unknown_reference,
              materialization_callbacks(&untouched_state))
              .ok() &&
             untouched_state.begin_count == 0,
         "unknown object references must fail before opening a runtime transaction");

  unijit::runtime::MaterializationPlan duplicate_destination(401, 73);
  expect(duplicate_destination.add({1, 20, 0, {}}).ok() &&
             !duplicate_destination.add({2, 20, 0, {}}).ok(),
         "object recipes must not overwrite the same logical frame slot");
  unijit::runtime::MaterializationPlan primitive_collision(401, 73);
  expect(primitive_collision.add({1, 0, 0, {}}).ok() &&
             !unijit::runtime::materialize_frame(
                  frame, primitive_collision,
                  materialization_callbacks(&untouched_state))
                  .ok() &&
             untouched_state.begin_count == 0,
         "object recipes must not overwrite recovered primitive frame slots");
  unijit::runtime::MaterializationPlan wrong_site(402, 73);
  expect(!unijit::runtime::materialize_frame(
              frame, wrong_site, materialization_callbacks(&untouched_state))
              .ok() &&
             untouched_state.begin_count == 0,
         "object plans must be bound to the exact deoptimization site and resume offset");
}

void test_on_stack_replacement_entry() {
  FunctionBuilder sum_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kFloat64,
          unijit::ir::ValueType::kFloat64});
  expect(sum_builder
             .set_return(sum_builder.float64_add(sum_builder.parameter(0),
                                                 sum_builder.parameter(1)))
             .ok(),
         "OSR sum fixture must be constructible");
  auto sum_compilation =
      Compiler::compile(std::move(sum_builder).build());
  expect(sum_compilation.ok(), "OSR sum fixture must compile");
  if (!sum_compilation.ok()) {
    return;
  }

  unijit::jit::CodeHandle retained;
  {
    unijit::jit::CodeCache cache;
    auto publication = cache.publish(
        "osr/sum", 1, std::move(sum_compilation.function));
    expect(publication.ok(), "OSR fixture must publish through the code cache");
    retained = publication.handle;
  }

  unijit::runtime::OsrFrame frame(501, 33);
  expect(frame
             .add(9, unijit::ir::ValueType::kFloat64,
                  unijit::ir::pack_float64(1.25))
             .ok() &&
             frame
                 .add(4, unijit::ir::ValueType::kFloat64,
                      unijit::ir::pack_float64(2.75))
                 .ok() &&
             frame.add(99, unijit::ir::ValueType::kWord, 123).ok() &&
             !frame.add(9, unijit::ir::ValueType::kWord, 0).ok(),
         "OSR frames must retain typed values and reject duplicate slots");
  unijit::runtime::OsrEntryPlan plan(501, 33);
  expect(plan.add_argument(9, unijit::ir::ValueType::kFloat64).ok() &&
             plan.add_argument(4, unijit::ir::ValueType::kFloat64).ok() &&
             !plan.add_argument(9, unijit::ir::ValueType::kFloat64).ok(),
         "OSR plans must define one typed source slot per native argument");
  const auto entry = retained.enter_osr(frame, plan);
  expect(entry.ok() && entry.arguments.entry_site == 501 &&
             entry.arguments.resume_offset == 33 &&
             entry.arguments.count == 2 &&
             entry.arguments.values[0] ==
                 unijit::ir::pack_float64(1.25) &&
             entry.arguments.values[1] ==
                 unijit::ir::pack_float64(2.75) &&
             unijit::ir::unpack_float64(entry.result.value) == 4.0,
         "OSR must enter a retained native generation with exact typed frame bits");

  unijit::runtime::OsrFrame wrong_site(502, 33);
  expect(wrong_site
             .add(9, unijit::ir::ValueType::kFloat64,
                  unijit::ir::pack_float64(1.0))
             .ok() &&
             !retained.enter_osr(wrong_site, plan).entered(),
         "OSR must reject a frame from a different interpreter site");
  unijit::runtime::OsrFrame missing_value(501, 33);
  expect(missing_value
             .add(9, unijit::ir::ValueType::kFloat64,
                  unijit::ir::pack_float64(1.0))
             .ok() &&
             !retained.enter_osr(missing_value, plan).entered(),
         "OSR must reject an incomplete interpreter frame");
  unijit::runtime::OsrEntryPlan wrong_signature(501, 33);
  expect(wrong_signature.add_argument(9, unijit::ir::ValueType::kWord).ok() &&
             wrong_signature
                 .add_argument(4, unijit::ir::ValueType::kFloat64)
                 .ok() &&
             !retained.enter_osr(frame, wrong_signature).entered(),
         "OSR must reject plans that differ from the native signature");
  expect(!unijit::jit::CodeHandle{}.enter_osr(frame, plan).entered(),
         "OSR must reject an invalid code lease");

  unijit::runtime::OsrFrame bounded_frame(1, 1);
  unijit::runtime::OsrEntryPlan bounded_plan(1, 1);
  bool within_limits = true;
  for (std::size_t index = 0;
       index < unijit::runtime::OsrFrame::kMaximumValues; ++index) {
    within_limits =
        within_limits &&
        bounded_frame.add(index, unijit::ir::ValueType::kWord, 0).ok() &&
        bounded_plan.add_argument(index, unijit::ir::ValueType::kWord).ok();
  }
  expect(within_limits &&
             !bounded_frame
                  .add(unijit::runtime::OsrFrame::kMaximumValues,
                       unijit::ir::ValueType::kWord, 0)
                  .ok() &&
             !bounded_plan
                  .add_argument(
                      unijit::runtime::OsrEntryPlan::kMaximumArguments,
                      unijit::ir::ValueType::kWord)
                  .ok(),
         "OSR metadata must enforce fixed transfer limits");

  FunctionBuilder guarded_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kFloat64,
          unijit::ir::ValueType::kFloat64});
  const Value divisor = guarded_builder.parameter(1);
  expect(guarded_builder.guard_float64_nonzero(divisor, 602).valid() &&
             guarded_builder
                 .set_return(guarded_builder.float64_divide(
                     guarded_builder.parameter(0), divisor))
                 .ok(),
         "guarded OSR fixture must be constructible");
  unijit::runtime::DeoptimizationRecord record;
  record.site = 602;
  record.resume_offset = 88;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          20, unijit::ir::ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::argument(
          21, unijit::ir::ValueType::kFloat64, 1)};
  unijit::runtime::DeoptimizationTable metadata;
  expect(metadata.add(record).ok(),
         "guarded OSR fixture must accept deoptimization metadata");
  auto guarded = Compiler::compile(std::move(guarded_builder).build(), metadata);
  expect(guarded.ok(), "guarded OSR fixture must compile");
  if (!guarded.ok()) {
    return;
  }
  unijit::runtime::OsrFrame guarded_frame(601, 77);
  const Word negative_zero = unijit::ir::pack_float64(-0.0);
  expect(guarded_frame
             .add(10, unijit::ir::ValueType::kFloat64,
                  unijit::ir::pack_float64(8.0))
             .ok() &&
             guarded_frame
                 .add(11, unijit::ir::ValueType::kFloat64, negative_zero)
                 .ok(),
         "guarded OSR frame must retain signed-zero state");
  unijit::runtime::OsrEntryPlan guarded_plan(601, 77);
  expect(guarded_plan
             .add_argument(10, unijit::ir::ValueType::kFloat64)
             .ok() &&
             guarded_plan
                 .add_argument(11, unijit::ir::ValueType::kFloat64)
                 .ok(),
         "guarded OSR plan must map live interpreter slots");
  unijit::runtime::ExecutionContext context;
  const auto guarded_entry =
      guarded.function->enter_osr(guarded_frame, guarded_plan, &context);
  const auto reconstruction = guarded.function->reconstruct_deoptimization(
      602, guarded_entry.arguments.data(), guarded_entry.arguments.count,
      context);
  const auto* recovered_divisor = reconstruction.frame.find(21);
  expect(guarded_entry.entered() && !guarded_entry.result.ok() &&
             guarded_entry.result.status.code() ==
                 unijit::StatusCode::kRuntimeExit &&
             guarded_entry.result.status.location() == 602 &&
             reconstruction.ok() && recovered_divisor != nullptr &&
             recovered_divisor->value == negative_zero,
         "OSR exits must retain marshalled arguments for exact deoptimization");
}

void test_compilation_resource_limits() {
  FunctionBuilder arithmetic_builder(2);
  expect(arithmetic_builder
             .set_return(arithmetic_builder.add(
                 arithmetic_builder.parameter(0),
                 arithmetic_builder.parameter(1)))
             .ok(),
         "resource-limit arithmetic fixture must be constructible");
  const Function arithmetic = std::move(arithmetic_builder).build();

  unijit::jit::CompilationOptions parameter_options;
  parameter_options.limits.maximum_parameters = 1;
  const auto parameter_limited =
      Compiler::compile(arithmetic, parameter_options);
  expect(!parameter_limited.ok() &&
             parameter_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             parameter_limited.status.location() == 2,
         "compilation must reject parameter counts before verification");

  unijit::jit::CompilationOptions node_options;
  node_options.limits.maximum_ir_nodes = 2;
  const auto node_limited = Compiler::compile(arithmetic, node_options);
  expect(!node_limited.ok() &&
             node_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             node_limited.status.location() == 3,
         "compilation must reject oversized IR before optimization");

  unijit::jit::CompilationOptions code_options;
  code_options.limits.maximum_code_bytes = 1;
  const auto code_limited = Compiler::compile(arithmetic, code_options);
  expect(!code_limited.ok() &&
             code_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             code_limited.status.location() > 1,
         "compilation must reject oversized machine code before W^X publication");

  unijit::jit::CompilationOptions invalid_options;
  invalid_options.limits.maximum_metadata_values = 0;
  const auto invalid_limits = Compiler::compile(arithmetic, invalid_options);
  expect(!invalid_limits.ok() &&
             invalid_limits.status.code() ==
                 unijit::StatusCode::kInvalidArgument,
         "compilation must reject disabled resource limits");

  FunctionBuilder safepoint_builder(0);
  expect(safepoint_builder.safepoint(701).valid() &&
             safepoint_builder.safepoint(702).valid() &&
             safepoint_builder
                 .set_return(safepoint_builder.constant(1))
                 .ok(),
         "resource-limit stack-map fixture must be constructible");
  unijit::jit::CompilationOptions stack_map_options;
  stack_map_options.limits.maximum_stack_maps = 1;
  const auto stack_map_limited = Compiler::compile(
      std::move(safepoint_builder).build(), stack_map_options);
  expect(!stack_map_limited.ok() &&
             stack_map_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted,
         "compilation must bound generated stack-map records");

  FunctionBuilder recovery_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kFloat64,
          unijit::ir::ValueType::kFloat64});
  const Value recovery_divisor = recovery_builder.parameter(1);
  expect(recovery_builder
             .guard_float64_nonzero(recovery_divisor, 703)
             .valid() &&
             recovery_builder.set_return(recovery_builder.parameter(0)).ok(),
         "resource-limit recovery fixture must be constructible");
  unijit::runtime::DeoptimizationRecord recovery_record;
  recovery_record.site = 703;
  recovery_record.resume_offset = 45;
  recovery_record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          0, unijit::ir::ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::argument(
          1, unijit::ir::ValueType::kFloat64, 1)};
  unijit::runtime::DeoptimizationTable recovery_metadata;
  expect(recovery_metadata.add(recovery_record).ok(),
         "resource-limit recovery metadata must validate structurally");
  unijit::jit::CompilationOptions recovery_options;
  recovery_options.limits.maximum_metadata_values = 1;
  const auto recovery_limited = Compiler::compile(
      std::move(recovery_builder).build(), recovery_metadata,
      unijit::runtime::AssumptionSet{}, recovery_options);
  expect(!recovery_limited.ok() &&
             recovery_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             recovery_limited.status.location() == 703,
         "compilation must bound requested deoptimization values");

  unijit::ir::ControlFlowBuilder cfg_builder(1);
  const unijit::ir::Block body = cfg_builder.create_block(1);
  expect(cfg_builder.jump(body, {cfg_builder.parameter(0)}).ok() &&
             cfg_builder.set_insertion_block(body).ok() &&
             cfg_builder.set_return(cfg_builder.block_parameter(body, 0)).ok(),
         "resource-limit CFG fixture must be constructible");
  unijit::jit::CompilationLimits cfg_limits;
  cfg_limits.maximum_cfg_blocks = 1;
  const auto cfg_limited =
      Compiler::compile(std::move(cfg_builder).build(), cfg_limits);
  expect(!cfg_limited.ok() &&
             cfg_limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             cfg_limited.status.location() == 2,
         "compilation must reject oversized CFGs before dominance analysis");
}

void test_constant_float64_nonzero_guard_elimination() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value divisor = builder.float64_constant(2.0);
  const Value guard = builder.guard_float64_nonzero(divisor, 81);
  expect(guard.valid(), "constant nonzero guard fixture must be constructible");
  const Value quotient = builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "constant nonzero guard fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::none_of(optimization.function.nodes().begin(),
                          optimization.function.nodes().end(),
                          [](const unijit::ir::Node& node) {
                            return node.opcode ==
                                   unijit::ir::Opcode::kGuardFloatNonzero;
                          }),
         "optimizer must eliminate a provably passing Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant-guard fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(!compilation.function->requires_context(),
         "eliminated guards must not require a managed execution context");
  expect(compilation.function->deoptimization_table().empty(),
         "eliminated guards must not retain stale deoptimization metadata");
  expect(compilation.function->stack_maps().empty(),
         "eliminated guards must not retain stale stack maps");
  const std::array<Word, 1> arguments = {unijit::ir::pack_float64(9.0)};
  const auto result =
      compilation.function->invoke(arguments.data(), arguments.size());
  expect(result.ok() && unijit::ir::unpack_float64(result.value) == 4.5,
         "constant-guard elimination must preserve the quotient");
}

void test_runtime_exit_site_identity() {
  FunctionBuilder straight_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kFloat64});
  const Value straight_parameter = straight_builder.parameter(0);
  expect(straight_builder.guard_float64_nonzero(straight_parameter, 91)
             .valid() &&
             straight_builder.safepoint(91).valid() &&
             straight_builder.set_return(straight_parameter).ok() &&
             !unijit::ir::verify(std::move(straight_builder).build()).ok(),
         "straight-line runtime exits must have unique stack-map sites");

  unijit::ir::ControlFlowBuilder control_builder(0);
  expect(control_builder.safepoint(92).valid() &&
             control_builder.safepoint(92).valid() &&
             control_builder.set_return(control_builder.constant(0)).ok() &&
             !unijit::ir::verify(std::move(control_builder).build()).ok(),
         "CFG runtime exits must have unique stack-map sites");

  FunctionBuilder assumed_builder(0);
  expect(assumed_builder.safepoint(93).valid() &&
             assumed_builder.set_return(assumed_builder.constant(0)).ok(),
         "assumption collision fixture must contain a safepoint");
  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  expect(assumptions.add(assumption, 93, 0).ok() &&
             !Compiler::compile(std::move(assumed_builder).build(), assumptions)
                  .ok(),
         "assumptions and explicit runtime exits must not share a site");

  constexpr std::size_t kOverCapacity =
      unijit::runtime::ExecutionContext::kMaximumCapturedValues + 1;
  FunctionBuilder oversized_builder(kOverCapacity);
  expect(oversized_builder.safepoint(94).valid(),
         "capture-capacity fixture must contain a safepoint");
  Value sum = oversized_builder.parameter(0);
  for (std::size_t index = 1; index < kOverCapacity; ++index) {
    sum = oversized_builder.add(sum, oversized_builder.parameter(index));
  }
  expect(oversized_builder.set_return(sum).ok(),
         "capture-capacity fixture must return all live parameters");
  const auto oversized = Compiler::compile(
      std::move(oversized_builder).build(),
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kBaseline});
  expect(!oversized.ok() &&
             oversized.status.code() ==
                 unijit::StatusCode::kResourceExhausted,
         "compilation must reject a stack map larger than its fixed capture area");
}

void test_verifier_rejects_mixed_arithmetic() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value invalid =
      builder.add(builder.parameter(0), builder.float64_constant(1.0));
  expect(builder.set_return(invalid).ok(),
         "mixed-type fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject mixed Word and Float64 arithmetic");
}

void test_float64_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(std::vector<unijit::ir::ValueType>(
      kParameters, unijit::ir::ValueType::kFloat64));
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(
            builder.float64_add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "Float64 spill fixture must return its reduction");
  const Function function = std::move(builder).build();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 register pressure must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "Float64 register pressure must exercise spill slots");

  std::array<Word, kParameters> args{};
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = unijit::ir::pack_float64(
        static_cast<double>(index + 1) * 0.25);
  }
  const auto expected =
      Interpreter::evaluate(function, args.data(), args.size());
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && expected.ok() && native.value == expected.value,
         "spilled native Float64 values must match the interpreter bits");
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool preserves_host_float_registers(unijit::jit::NativeEntry entry) {
  double lhs = 1.25;
  double rhs = -7.5;
  for (std::size_t iteration = 0; iteration < 512; ++iteration) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const double expected = (lhs + rhs) * (lhs - 3.25) + rhs * 0.75;
    const double native =
        unijit::ir::unpack_float64(entry(arguments.data(), nullptr));
    if (native != expected) {
      return false;
    }
    lhs += 0.125;
    rhs -= 0.0625;
  }
  return lhs == 65.25 && rhs == -39.5;
}

void test_float64_preserves_host_abi() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value lhs = builder.parameter(0);
  const Value rhs = builder.parameter(1);
  const Value product = builder.float64_multiply(
      builder.float64_add(lhs, rhs),
      builder.float64_subtract(lhs, builder.float64_constant(3.25)));
  const Value result = builder.float64_add(
      product,
      builder.float64_multiply(rhs, builder.float64_constant(0.75)));
  expect(builder.set_return(result).ok(),
         "host-ABI fixture must return its Float64 expression");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "host-ABI Float64 fixture must compile");
  if (compilation.ok()) {
    expect(preserves_host_float_registers(
               compilation.function->native_entry()),
           "native Float64 code must preserve host callee-saved registers");
  }
}

void test_runtime_helper_call() {
  FunctionBuilder builder(2);
  const Value live = builder.add(builder.parameter(0), builder.constant(5));
  const Value called = builder.call(
      sum_runtime_helper, {live, builder.parameter(1), builder.constant(7)});
  const Value result = builder.multiply(live, called);
  expect(builder.set_return(result).ok(),
         "runtime-call fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "runtime helper call must satisfy SSA verification");

  const std::array<Word, 2> args = {3, 4};
  runtime_call_count = 0;
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 152 &&
             runtime_call_count == 1,
         "interpreter must execute runtime helpers with ordered arguments");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "runtime helper call must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native runtime call must preserve live values and helper effects");
}

void test_effectful_dead_runtime_call() {
  FunctionBuilder builder(0);
  const Value ignored = builder.call(sum_runtime_helper, {});
  (void)ignored;
  expect(builder.set_return(builder.constant(42)).ok(),
         "effectful-call fixture must record a return");
  const Function function = std::move(builder).build();
  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept an effectful runtime call");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "dead-result runtime call must remain compilable");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok() && result.value == 42 && runtime_call_count == 1,
         "optimizer must preserve runtime calls whose result is dead");
}

void test_float64_runtime_helper_call() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value live =
      builder.float64_add(builder.parameter(0), builder.float64_constant(0.5));
  const Value called = builder.call(
      float_runtime_helper, {live, builder.parameter(1)},
      unijit::ir::ValueType::kFloat64);
  const Value result = builder.float64_add(live, called);
  expect(builder.set_return(result).ok(),
         "Float64 runtime call must record its result");
  const Function function = std::move(builder).build();
  const std::array<Word, 2> args = {unijit::ir::pack_float64(3.5),
                                    unijit::ir::pack_float64(-2.0)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 runtime helper call must compile");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && interpreted.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native Float64 call must preserve live FP registers and result bits");
}

void test_verifier_rejects_null_runtime_helper() {
  FunctionBuilder builder(0);
  const Value invalid = builder.call(nullptr, {});
  expect(builder.set_return(invalid).ok(),
         "null-helper fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject a null runtime helper target");
}

void test_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(kParameters);
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(builder.add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "spill return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "register-pressure function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "register-pressure function must exercise spill slots");

  std::array<Word, kParameters> args{};
  Word expected = 0;
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = static_cast<Word>(index + 1);
    expected += args[index];
  }
  const auto result = compilation.function->invoke(args.data(), args.size());
  expect(result.ok(), "spilled native function must execute");
  expect(result.value == expected, "spilled values must survive allocation");
}

void test_argument_validation() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "validation fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  const std::array<Word, 1> too_few = {1};
  expect(!compilation.function->invoke(too_few.data(), too_few.size()).ok(),
         "compiled invocation must reject the wrong argument count");
  expect(!Interpreter::evaluate(function, nullptr, 2).ok(),
         "interpreter must reject null argument storage");
}

void test_optimization_pipeline() {
  FunctionBuilder builder(1);
  const Value zero = builder.constant(0);
  const Value dead_sum =
      builder.add(builder.parameter(0), builder.constant(99));
  const Value annihilated = builder.multiply(dead_sum, zero);
  const Value answer = builder.add(annihilated, builder.constant(42));
  expect(builder.set_return(answer).ok(), "optimized return must be accepted");
  const Function function = std::move(builder).build();

  auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimization pipeline must accept verified SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.input_nodes == 7,
         "optimizer must report the input SSA size");
  expect(optimization.stats.output_nodes == 2,
         "optimizer must fold the graph to one parameter and one constant");
  expect(optimization.stats.constants_folded > 0,
         "optimizer must exercise constant folding");
  expect(optimization.stats.algebraic_simplifications > 0,
         "optimizer must exercise algebraic simplification");

  const std::array<Word, 1> args = {std::numeric_limits<Word>::max()};
  const auto interpreted =
      Interpreter::evaluate(optimization.function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 42,
         "optimized SSA must preserve observable semantics");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "compiler must consume optimized SSA");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().input_ir_nodes == 7,
         "compiler must report input IR nodes");
  expect(compilation.function->stats().optimized_ir_nodes == 2,
         "compiler must report optimized IR nodes");
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == 42,
         "optimized native code must preserve observable semantics");

  auto baseline = Compiler::compile(
      function, unijit::jit::CompilationOptions{
                    unijit::jit::OptimizationLevel::kBaseline});
  expect(baseline.ok() &&
             baseline.function->stats().input_ir_nodes == 7 &&
             baseline.function->stats().optimized_ir_nodes == 7,
         "baseline compilation must preserve verified input SSA");
  if (baseline.ok()) {
    const auto baseline_native =
        baseline.function->invoke(args.data(), args.size());
    expect(baseline_native.ok() && baseline_native.value == 42,
           "baseline native code must preserve observable semantics");
  }

  auto invalid_level = Compiler::compile(
      function, unijit::jit::CompilationOptions{
                    static_cast<unijit::jit::OptimizationLevel>(255)});
  expect(!invalid_level.ok() &&
             invalid_level.status.code() ==
                 unijit::StatusCode::kInvalidArgument,
         "compiler must reject an unknown optimization level");
}

void test_control_flow_optimization_pipeline() {
  unijit::ir::ControlFlowBuilder builder(1);
  const Value zero = builder.constant(0);
  const Value folded =
      builder.add(builder.constant(40), builder.constant(2));
  const Value simplified = builder.add(builder.parameter(0), zero);
  expect(builder.set_return(builder.add(simplified, folded)).ok(),
         "CFG optimization fixture must record its result");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() && optimization.stats.input_nodes == 7 &&
             optimization.stats.output_nodes == 3 &&
             optimization.stats.constants_folded == 1 &&
             optimization.stats.algebraic_simplifications == 1,
         "CFG optimizer must fold constants and canonicalize Word identities");
  if (!optimization.ok()) {
    return;
  }

  const std::array<Word, 1> arguments = {9};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      optimization.function, arguments.data(), arguments.size());
  expect(interpreted.ok() && interpreted.value == 51,
         "optimized CFG must preserve observable semantics");

  auto baseline = Compiler::compile(
      function, unijit::jit::CompilationOptions{
                    unijit::jit::OptimizationLevel::kBaseline});
  auto optimized = Compiler::compile(
      function, unijit::jit::CompilationOptions{
                    unijit::jit::OptimizationLevel::kOptimized});
  expect(baseline.ok() && optimized.ok() &&
             baseline.function->stats().input_ir_nodes == 7 &&
             baseline.function->stats().optimized_ir_nodes == 7 &&
             optimized.function->stats().input_ir_nodes == 7 &&
             optimized.function->stats().optimized_ir_nodes == 3,
         "CFG compilation levels must expose distinct baseline and optimized tiers");
  if (baseline.ok() && optimized.ok()) {
    const auto baseline_result =
        baseline.function->invoke(arguments.data(), arguments.size());
    const auto optimized_result =
        optimized.function->invoke(arguments.data(), arguments.size());
    expect(baseline_result.ok() && optimized_result.ok() &&
               baseline_result.value == 51 && optimized_result.value == 51,
           "both CFG compilation tiers must preserve native results");
  }

  auto invalid_level = Compiler::compile(
      function, unijit::jit::CompilationOptions{
                    static_cast<unijit::jit::OptimizationLevel>(255)});
  expect(!invalid_level.ok() &&
             invalid_level.status.code() ==
                 unijit::StatusCode::kInvalidArgument,
         "CFG compiler must reject an unknown optimization level");
}

void test_control_flow_branch_and_value_canonicalization() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block selected = builder.create_block(0);
  const unijit::ir::Block discarded = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition =
      builder.less_than(builder.constant(0), builder.constant(1));
  expect(builder.branch(condition, selected, {}, discarded, {}).ok(),
         "constant-branch fixture must select two valid successors");

  expect(builder.set_insertion_block(selected).ok(),
         "selected constant branch must exist");
  const Value first =
      builder.add(builder.parameter(0), builder.constant(5));
  const Value duplicate =
      builder.add(builder.parameter(0), builder.constant(5));
  expect(builder.jump(merge, {builder.add(first, duplicate)}).ok(),
         "selected constant branch must reach its merge");

  expect(builder.set_insertion_block(discarded).ok(),
         "discarded constant branch must exist before optimization");
  const Value unreachable_call = builder.call(
      sum_runtime_helper, {builder.constant(100), builder.constant(200)});
  expect(unreachable_call.valid() && builder.jump(merge, {unreachable_call}).ok(),
         "discarded branch must retain a structurally reachable effect");

  expect(builder.set_insertion_block(merge).ok() &&
             builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "constant-branch merge must return its selected value");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "constant-branch canonicalization fixture must verify");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() && optimization.function.blocks().size() == 3 &&
             optimization.stats.branches_folded == 1 &&
             optimization.stats.common_subexpressions == 2 &&
             optimization.stats.output_nodes < optimization.stats.input_nodes,
         "CFG optimizer must prune constant branches and reuse local pure expressions");
  if (!optimization.ok()) {
    return;
  }

  const std::array<Word, 1> arguments = {7};
  runtime_call_count = 0;
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      optimization.function, arguments.data(), arguments.size());
  auto compilation = Compiler::compile(function);
  const auto native =
      compilation.ok()
          ? compilation.function->invoke(arguments.data(), arguments.size())
          : unijit::ir::EvaluationResult{};
  expect(interpreted.ok() && native.ok() && interpreted.value == 24 &&
             native.value == 24 && runtime_call_count == 0,
         "canonical CFG execution must preserve the selected result and remove unreachable effects");

  constexpr std::size_t kDiscardedGuardSite = 719;
  unijit::ir::ControlFlowBuilder guard_builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const unijit::ir::Block guard_selected = guard_builder.create_block(0);
  const unijit::ir::Block guard_discarded = guard_builder.create_block(0);
  expect(guard_builder
             .branch(guard_builder.constant(1), guard_selected, {},
                     guard_discarded, {})
             .ok() &&
             guard_builder.set_insertion_block(guard_selected).ok() &&
             guard_builder.set_return(guard_builder.parameter(0)).ok() &&
             guard_builder.set_insertion_block(guard_discarded).ok() &&
             guard_builder
                 .guard_float64_nonzero(guard_builder.parameter(0),
                                        kDiscardedGuardSite)
                 .valid() &&
             guard_builder.set_return(guard_builder.parameter(0)).ok(),
         "discarded-guard fixture must retain a valid pre-optimization exit");
  const auto guard_function = std::move(guard_builder).build();
  auto guard_baseline = Compiler::compile(
      guard_function, unijit::jit::CompilationOptions{
                          unijit::jit::OptimizationLevel::kBaseline});
  auto guard_optimized = Compiler::compile(guard_function);
  expect(guard_baseline.ok() && guard_optimized.ok() &&
             guard_baseline.function->requires_context() &&
             guard_baseline.function->deoptimization_record(
                 kDiscardedGuardSite) != nullptr &&
             !guard_optimized.function->requires_context() &&
             guard_optimized.function->deoptimization_table().empty(),
         "constant-branch pruning must remove unreachable guard metadata only from the optimized tier");
}

void test_optimization_exit_state_mapping() {
  FunctionBuilder builder(2);
  const Value dead = builder.add(builder.parameter(0), builder.constant(91));
  const Value snapshot = builder.multiply(
      builder.add(builder.parameter(0), builder.constant(0)),
      builder.constant(2));
  const Value guard = builder.guard_float64_nonzero(
      builder.float64_constant(1.0), 501);
  expect(dead.valid() && snapshot.valid() && guard.valid() &&
             builder.set_return(builder.parameter(1)).ok(),
         "optimizer exit-state fixture must be constructible");
  const Function function = std::move(builder).build();

  const auto retained = unijit::ir::Optimizer::run(
      function, {{guard, {snapshot}}});
  expect(retained.ok() && !retained.map(snapshot).valid(),
         "optimizer must drop frame-only values with an eliminated guard");

  FunctionBuilder live_guard_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kWord,
          unijit::ir::ValueType::kFloat64});
  const Value live_snapshot = live_guard_builder.multiply(
      live_guard_builder.add(live_guard_builder.parameter(0),
                             live_guard_builder.constant(0)),
      live_guard_builder.constant(2));
  const Value live_guard = live_guard_builder.guard_float64_nonzero(
      live_guard_builder.parameter(1), 502);
  expect(live_snapshot.valid() && live_guard.valid() &&
             live_guard_builder.set_return(live_guard_builder.parameter(0))
                 .ok(),
         "live optimizer exit-state fixture must be constructible");
  const Function live_function = std::move(live_guard_builder).build();
  const auto mapped = unijit::ir::Optimizer::run(
      live_function, {{live_guard, {live_snapshot}}});
  const auto unpreserved = unijit::ir::Optimizer::run(live_function);
  expect(mapped.ok() && mapped.map(live_snapshot).valid() &&
             mapped.function.value_type(mapped.map(live_snapshot)) ==
                 unijit::ir::ValueType::kWord &&
             unpreserved.ok() && !unpreserved.map(live_snapshot).valid(),
         "live exit states must preserve metadata-only SSA values through canonicalization");
}

void test_float64_constant_folding() {
  FunctionBuilder builder(0);
  const Value numerator = builder.float64_add(
      builder.float64_constant(12.0), builder.float64_constant(5.0));
  const Value quotient =
      builder.float64_divide(numerator, builder.float64_constant(2.0));
  expect(builder.set_return(quotient).ok(),
         "Float64 folding fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept constant Float64 SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.output_nodes == 1 &&
             optimization.stats.constants_folded == 2,
         "optimizer must fold chained Float64 arithmetic to one constant");
  const auto interpreted =
      Interpreter::evaluate(optimization.function, nullptr, 0);
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(8.5),
         "folded Float64 constants must preserve result bits");
}

void test_control_flow_counted_loop() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);

  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {one, zero}).ok(),
         "entry must jump to the loop header");

  expect(builder.set_insertion_block(loop).ok(),
         "loop block must accept insertion");
  const Value index = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value next_sum = builder.add(sum, index);
  const Value next_index = builder.add(index, one);
  const Value continue_loop =
      builder.less_equal(next_index, builder.parameter(0));
  expect(
      builder
          .branch(continue_loop, loop, {next_index, next_sum}, exit, {next_sum})
          .ok(),
      "loop latch must branch with explicit block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "exit block must accept insertion");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "exit block must return its block parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "counted-loop CFG must satisfy dominance and edge invariants");
  const std::array<Word, 1> args = {100};
  const auto result = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(result.ok() && result.value == 5050,
         "CFG interpreter must execute loop-carried block parameters");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "counted-loop CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == result.value,
           "native counted loop must match the CFG interpreter");
  }
}

void test_control_flow_runtime_helper_ir() {
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ControlFlowInterpreter;
  using unijit::ir::ValueType;

  ControlFlowBuilder builder(
      {ValueType::kFloat64, ValueType::kFloat64});
  const Value live = builder.float64_add(builder.parameter(0),
                                         builder.float64_constant(0.5));
  const Value called = builder.call(float_runtime_helper,
                                    {live, builder.parameter(1)},
                                    ValueType::kFloat64);
  const Value result = builder.float64_add(live, called);
  expect(builder.set_return(result).ok(),
         "CFG runtime-call fixture must record its result");
  const auto function = std::move(builder).build();
  expect(function.call_arguments().size() == 2 &&
             unijit::ir::verify(function).ok(),
         "CFG runtime calls must retain verified flattened arguments");
  const auto allocation =
      unijit::jit::detail::allocate_control_flow_registers(
          function, 4, 4, unijit::jit::detail::StackMapRequirements{});
  expect(allocation.status.ok() &&
             allocation.live_across_calls[called.id()].size() == 1 &&
             allocation.live_across_calls[called.id()][0] == live,
         "CFG allocation must identify only register values live after a call");

  const std::array<Word, 2> arguments = {
      unijit::ir::pack_float64(3.5),
      unijit::ir::pack_float64(-2.0)};
  runtime_call_count = 0;
  const auto interpreted = ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(-4.0) &&
             runtime_call_count == 1,
         "CFG interpreter must execute typed helpers and preserve live values");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "typed CFG runtime calls must compile");
  if (compilation.ok()) {
    runtime_call_count = 0;
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value &&
               runtime_call_count == 1,
           "native CFG calls must preserve Float64 results and live registers");
  }

  ControlFlowBuilder null_builder(0);
  const Value invalid = null_builder.call(nullptr, {});
  expect(null_builder.set_return(invalid).ok() &&
             !unijit::ir::verify(std::move(null_builder).build()).ok(),
         "CFG verifier must reject a null runtime helper");

  ControlFlowBuilder terminated_builder(0);
  const Value existing = terminated_builder.constant(7);
  expect(terminated_builder.set_return(existing).ok() &&
             !terminated_builder.call(sum_runtime_helper, {existing}).valid(),
         "terminated CFG blocks must reject runtime calls");
  const auto terminated = std::move(terminated_builder).build();
  expect(terminated.call_arguments().empty() &&
             unijit::ir::verify(terminated).ok(),
         "failed CFG call construction must roll back flattened arguments");

  ControlFlowBuilder limited_builder(0);
  const Value limited_call = limited_builder.call(
      sum_runtime_helper,
      {limited_builder.constant(1), limited_builder.constant(2),
       limited_builder.constant(3)});
  expect(limited_builder.set_return(limited_call).ok(),
         "CFG call-limit fixture must record its result");
  unijit::jit::CompilationLimits limits;
  limits.maximum_ir_arguments = 2;
  const auto limited =
      Compiler::compile(std::move(limited_builder).build(), limits);
  expect(!limited.ok() &&
             limited.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             limited.status.location() == 3,
         "CFG compilation must bound flattened runtime-call arguments");
}

void test_control_flow_runtime_call_loop() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ControlFlowInterpreter;
  using unijit::ir::ValueType;

  ControlFlowBuilder builder({ValueType::kWord, ValueType::kFloat64});
  const Block loop = builder.create_block(
      {ValueType::kWord, ValueType::kWord, ValueType::kFloat64});
  const Block exit =
      builder.create_block({ValueType::kWord, ValueType::kFloat64});
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0), zero,
                             builder.parameter(1)})
             .ok(),
         "CFG runtime-call loop must enter with mixed state");

  expect(builder.set_insertion_block(loop).ok(),
         "CFG runtime-call loop block must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value marker = builder.block_parameter(loop, 2);
  const Value next_sum =
      builder.call(sum_runtime_helper, {sum, remaining});
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continues = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continues, loop,
                     {next_remaining, next_sum, marker}, exit,
                     {next_sum, marker})
             .ok(),
         "CFG runtime-call loop must carry helper results across its backedge");

  expect(builder.set_insertion_block(exit).ok(),
         "CFG runtime-call loop exit must exist");
  const Value final_sum = builder.block_parameter(exit, 0);
  const Value final_marker = builder.block_parameter(exit, 1);
  const Value marker_is_positive = builder.float64_less_than(
      builder.float64_constant(0.0), final_marker);
  expect(builder.set_return(builder.add(final_sum, marker_is_positive)).ok(),
         "CFG runtime-call loop must consume live Word and Float64 state");

  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "CFG runtime-call loop must satisfy verifier invariants");
  const std::array<Word, 2> arguments = {
      4, unijit::ir::pack_float64(3.5)};
  runtime_call_count = 0;
  const auto interpreted = ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() && interpreted.value == 11 &&
             runtime_call_count == 4,
         "CFG interpreter must execute one Word helper call per iteration");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "CFG runtime-call loop must compile");
  if (compilation.ok()) {
    runtime_call_count = 0;
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value &&
               runtime_call_count == 4,
           "native CFG loops must preserve mixed values across helper calls");
  }
}

void test_control_flow_runtime_call_spills() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ControlFlowInterpreter;
  using unijit::ir::ValueType;

  std::vector<ValueType> types;
  std::vector<Value> call_arguments;
  std::array<Word, 12> arguments{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    types.push_back(index % 2 == 0 ? ValueType::kWord
                                  : ValueType::kFloat64);
    arguments[index] = index % 2 == 0
                           ? static_cast<Word>(index / 2 + 1)
                           : unijit::ir::pack_float64(
                                 static_cast<double>(index / 2) + 0.5);
  }
  ControlFlowBuilder builder(types);
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    call_arguments.push_back(builder.parameter(index));
  }
  const Block positive = builder.create_block(0);
  const Block negative = builder.create_block(0);
  const Value called =
      builder.call(mixed_runtime_helper, call_arguments, ValueType::kFloat64);
  const Value condition =
      builder.less_than(builder.parameter(0), builder.constant(1000));
  expect(builder.branch(condition, positive, {}, negative, {}).ok(),
         "mixed runtime-call fixture must branch on a live Word value");
  expect(builder.set_insertion_block(positive).ok() &&
             builder
                 .set_return(builder.float64_add(called,
                                                 builder.parameter(1)))
                 .ok(),
         "mixed runtime-call positive path must return its Float64 result");
  expect(builder.set_insertion_block(negative).ok() &&
             builder
                 .set_return(builder.float64_subtract(called,
                                                      builder.parameter(1)))
                 .ok(),
         "mixed runtime-call negative path must return its Float64 result");

  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "mixed runtime-call spill fixture must verify");
  runtime_call_count = 0;
  const auto interpreted = ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(39.5) &&
             runtime_call_count == 1,
         "CFG interpreter must preserve ordered mixed helper arguments");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "mixed CFG runtime-call spills must compile");
  if (compilation.ok()) {
    runtime_call_count = 0;
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value &&
               runtime_call_count == 1,
           "native CFG calls must marshal mixed register and stack arguments");
  }
}

void test_control_flow_effectful_dead_runtime_call() {
  unijit::ir::ControlFlowBuilder builder(0);
  builder.call(sum_runtime_helper,
               {builder.constant(19), builder.constant(23)});
  expect(builder.set_return(builder.constant(42)).ok(),
         "dead-result CFG call fixture must record its return value");
  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "effectful dead-result CFG calls must verify");

  runtime_call_count = 0;
  const auto interpreted =
      unijit::ir::ControlFlowInterpreter::evaluate(function, nullptr, 0);
  expect(interpreted.ok() && interpreted.value == 42 &&
             runtime_call_count == 1,
         "CFG interpreter must retain effectful dead-result calls");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "effectful dead-result CFG calls must compile");
  if (compilation.ok()) {
    runtime_call_count = 0;
    const auto native = compilation.function->invoke(nullptr, 0);
    expect(native.ok() && native.value == 42 && runtime_call_count == 1,
           "native CFG code must retain effectful dead-result calls");
  }
}

void test_control_flow_runtime_call_exits() {
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;

  constexpr std::size_t kGuardSite = 316;
  ControlFlowBuilder guard_builder(
      {ValueType::kFloat64, ValueType::kFloat64});
  const Value divisor = guard_builder.call(
      float_runtime_helper,
      {guard_builder.parameter(0), guard_builder.parameter(1)},
      ValueType::kFloat64);
  expect(guard_builder.guard_float64_nonzero(divisor, kGuardSite).valid() &&
             guard_builder.set_return(divisor).ok(),
         "CFG call-before-guard fixture must be constructible");
  const auto guard_function = std::move(guard_builder).build();
  auto guard_compilation = Compiler::compile(guard_function);
  expect(guard_compilation.ok(), "CFG calls followed by guards must compile");
  if (guard_compilation.ok()) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(2.0),
        unijit::ir::pack_float64(0.0)};
    unijit::runtime::ExecutionContext context;
    runtime_call_count = 0;
    const auto guarded = guard_compilation.function->invoke(
        arguments.data(), arguments.size(), &context);
    expect(!guarded.ok() &&
               guarded.status.code() == unijit::StatusCode::kRuntimeExit &&
               guarded.status.location() == kGuardSite &&
               runtime_call_count == 1,
           "CFG guard exits must return safely after a helper call");
  }

  constexpr std::size_t kSafepointSite = 317;
  ControlFlowBuilder safepoint_builder(1);
  const Value called = safepoint_builder.call(
      sum_runtime_helper,
      {safepoint_builder.parameter(0), safepoint_builder.constant(1)});
  const Value safepoint = safepoint_builder.safepoint(kSafepointSite);
  expect(safepoint_builder
             .set_return(safepoint_builder.add(called, safepoint))
             .ok(),
         "CFG call-before-safepoint fixture must be constructible");
  const auto safepoint_function = std::move(safepoint_builder).build();
  auto safepoint_compilation = Compiler::compile(safepoint_function);
  expect(safepoint_compilation.ok(),
         "CFG calls followed by safepoints must compile");
  if (safepoint_compilation.ok()) {
    const std::array<Word, 1> arguments = {41};
    unijit::runtime::ExecutionContext context;
    context.request_interrupt();
    runtime_call_count = 0;
    const auto interrupted = safepoint_compilation.function->invoke(
        arguments.data(), arguments.size(), &context);
    expect(!interrupted.ok() &&
               interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               interrupted.status.location() == kSafepointSite &&
               runtime_call_count == 1,
           "CFG safepoint exits must return safely after a helper call");
  }
}

void test_control_flow_float64_loop() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block loop = builder.create_block(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block exit =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1)}).ok(),
         "typed CFG entry must pass Word and Float64 parameters");

  expect(builder.set_insertion_block(loop).ok(),
         "typed Float64 loop block must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value accumulator = builder.block_parameter(loop, 1);
  const Value scaled = builder.float64_multiply(
      accumulator, builder.float64_constant(1.5));
  const Value adjusted = builder.float64_subtract(
      scaled, builder.float64_constant(0.25));
  const Value divided =
      builder.float64_divide(adjusted, builder.float64_constant(2.0));
  const Value next_accumulator =
      builder.float64_add(divided, builder.float64_constant(0.5));
  const Value next_remaining = builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  expect(builder
             .branch(continues, loop,
                     {next_remaining, next_accumulator}, exit,
                     {next_accumulator})
             .ok(),
         "typed Float64 loop must preserve edge types");

  expect(builder.set_insertion_block(exit).ok(),
         "typed Float64 exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "typed Float64 exit must return its block parameter");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 CFG must satisfy verifier invariants");

  double expected = 1.0;
  for (int iteration = 0; iteration < 4; ++iteration) {
    expected = ((expected * 1.5) - 0.25) / 2.0 + 0.5;
  }
  const std::array<Word, 2> arguments = {
      4, unijit::ir::pack_float64(1.0)};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(expected),
         "typed CFG interpreter must preserve Float64 loop values");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "typed Float64 CFG must compile to native code");
  if (compilation.ok()) {
    expect(compilation.function->return_type() == ValueType::kFloat64 &&
               compilation.function->parameter_type(0) == ValueType::kWord &&
               compilation.function->parameter_type(1) == ValueType::kFloat64,
           "compiled CFG must retain its complete typed signature");
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native typed Float64 CFG must match the interpreter");
  }
}

void test_control_flow_float64_guard_deoptimization() {
  using unijit::ir::ValueType;
  constexpr std::size_t kGuardSite = 313;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value divisor = builder.parameter(1);
  const Value snapshot = builder.float64_add(
      builder.parameter(0),
      builder.float64_multiply(builder.float64_constant(0.5),
                               builder.float64_constant(2.5)));
  expect(builder.guard_float64_nonzero(divisor, kGuardSite).valid(),
         "CFG Float64 guard must produce an effect value");
  const Value quotient =
      builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "guarded CFG division fixture must record its result");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "CFG Float64 nonzero guard must pass verification");

  unijit::runtime::DeoptimizationRecord record;
  record.site = kGuardSite;
  record.resume_offset = 77;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          0, ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::argument(
          1, ValueType::kFloat64, 1),
      unijit::runtime::RecoveryOperation::exit_value(
          2, ValueType::kFloat64),
      unijit::runtime::RecoveryOperation::captured_value(
          3, ValueType::kFloat64, snapshot)};
  unijit::runtime::DeoptimizationTable metadata;
  expect(metadata.add(record).ok(),
         "CFG guard reconstruction metadata must be accepted");

  auto compilation = Compiler::compile(function, metadata);
  expect(compilation.ok(),
         "CFG Float64 guard must compile with deoptimization metadata");
  if (!compilation.ok()) {
    return;
  }
  const auto* compiled_record =
      compilation.function->deoptimization_record(kGuardSite);
  expect(compilation.function->requires_context() &&
             compilation.function->stats().optimized_ir_nodes <
                 compilation.function->stats().input_ir_nodes &&
             compiled_record != nullptr &&
             compiled_record->resume_offset == 77 &&
             compiled_record->reason ==
                 unijit::runtime::DeoptimizationReason::kDivisionByZero &&
             compiled_record->recovery.back().capture_resolved(),
         "compiled CFG guard must retain its diagnosed exit metadata");

  const std::array<Word, 2> valid = {
      unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(3.0)};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, valid.data(), valid.size());
  const auto native =
      compilation.function->invoke(valid.data(), valid.size());
  expect(interpreted.ok() && native.ok() &&
             native.value == unijit::ir::pack_float64(3.0) &&
             native.value == interpreted.value,
         "a nonzero CFG divisor must pass interpreter and native guards");

  constexpr std::array<double, 2> kZeroes = {0.0, -0.0};
  for (double zero : kZeroes) {
    const std::array<Word, 2> invalid = {
        unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(zero)};
    unijit::runtime::ExecutionContext interpreter_context;
    const auto interpreter_exit =
        unijit::ir::ControlFlowInterpreter::evaluate(
            function, invalid.data(), invalid.size(), 100,
            &interpreter_context);
    expect(!interpreter_exit.ok() &&
               interpreter_exit.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               interpreter_exit.status.location() == kGuardSite &&
               interpreter_context.exit_value() == invalid[1],
           "CFG interpreter guard must retain either signed zero divisor");

    unijit::runtime::ExecutionContext native_context;
    const auto native_exit = compilation.function->invoke(
        invalid.data(), invalid.size(), &native_context);
    const auto reconstruction =
        compilation.function->reconstruct_deoptimization(
            kGuardSite, invalid.data(), invalid.size(), native_context);
    const auto* recovered_lhs = reconstruction.frame.find(0);
    const auto* recovered_divisor = reconstruction.frame.find(2);
    const auto* recovered_snapshot = reconstruction.frame.find(3);
    expect(!native_exit.ok() &&
               native_exit.status.code() == unijit::StatusCode::kRuntimeExit &&
               native_exit.status.location() == kGuardSite &&
               native_context.exit_value() == invalid[1] &&
               reconstruction.ok() &&
               reconstruction.frame.resume_offset == 77 &&
               recovered_lhs != nullptr && recovered_lhs->value == invalid[0] &&
               recovered_divisor != nullptr &&
               recovered_divisor->value == invalid[1] &&
               recovered_snapshot != nullptr &&
               recovered_snapshot->value ==
                   unijit::ir::pack_float64(10.25),
           "native CFG guard must reconstruct exact arguments, exits, and metadata-only SSA values");
  }

  unijit::ir::ControlFlowBuilder malformed(1);
  const Value invalid_guard =
      malformed.guard_float64_nonzero(malformed.parameter(0), 1);
  expect(malformed.set_return(invalid_guard).ok() &&
             !unijit::ir::verify(std::move(malformed).build()).ok(),
         "CFG verifier must reject a Float64 guard over a Word operand");
}

void test_control_flow_rejects_mixed_edge_types() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(!builder.jump(target, {builder.constant(1)}).ok(),
         "CFG builder must reject a Word edge argument for Float64 parameter");
}

void test_control_flow_rejects_mixed_return_types() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block word_return = builder.create_block(0);
  const unijit::ir::Block float_return = builder.create_block(0);
  expect(builder.branch(builder.constant(1), word_return, {}, float_return, {})
             .ok(),
         "mixed-return fixture must branch to both return blocks");
  expect(builder.set_insertion_block(word_return).ok() &&
             builder.set_return(builder.constant(1)).ok(),
         "mixed-return fixture must create its Word return");
  expect(builder.set_insertion_block(float_return).ok() &&
             builder.set_return(builder.float64_constant(1.0)).ok(),
         "mixed-return fixture must create its Float64 return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "CFG verifier must reject inconsistent return types");
}

void test_control_flow_float64_comparisons() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value less =
      builder.float64_less_than(builder.parameter(0), builder.parameter(1));
  const Value less_equal =
      builder.float64_less_equal(builder.parameter(0), builder.parameter(1));
  const Value equal =
      builder.float64_equal(builder.parameter(0), builder.parameter(1));
  const Value not_equal =
      builder.float64_not_equal(builder.parameter(0), builder.parameter(1));
  const Value encoded = builder.add(
      builder.multiply(less, builder.constant(1000)),
      builder.add(builder.multiply(less_equal, builder.constant(100)),
                  builder.add(builder.multiply(equal, builder.constant(10)),
                              not_equal)));
  expect(builder.set_return(encoded).ok(),
         "Float64 CFG comparison fixture must return its encoded flags");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 CFG comparisons must satisfy type verification");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 CFG comparisons must compile");

  const auto check = [&](double lhs, double rhs, Word expected,
                         const char *message) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == expected, message);
    if (compilation.ok()) {
      const auto native =
          compilation.function->invoke(arguments.data(), arguments.size());
      expect(native.ok() && native.value == interpreted.value,
             "native Float64 CFG comparison must match the interpreter");
    }
  };
  check(1.0, 2.0, 1101,
        "less operands must set ordered and inequality flags");
  check(2.0, 2.0, 110,
        "equal operands must set inclusive and equality flags");
  check(3.0, 2.0, 1,
        "greater operands must only set the inequality flag");
  check(0.0, -0.0, 110,
        "signed zeroes must compare equal under Float64 semantics");
  check(std::numeric_limits<double>::quiet_NaN(), 2.0, 1,
        "unordered operands must compare unequal");
}

void test_control_flow_float64_negation() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64});
  const Value first = builder.float64_negate(builder.parameter(0));
  const Value duplicate = builder.float64_negate(builder.parameter(0));
  const Value result = builder.float64_add(first, duplicate);
  expect(builder.set_return(result).ok(),
         "Float64 CFG negation fixture must record its result");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 CFG negation must satisfy type verification");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             optimization.stats.common_subexpressions == 1,
         "CFG optimizer must eliminate duplicate Float64 negations");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 CFG negation must compile");
  constexpr std::array<std::uint64_t, 4> kSamples = {
      UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
      UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000)};
  for (const std::uint64_t bits : kSamples) {
    const Word argument = static_cast<Word>(bits);
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        function, &argument, 1);
    if (compilation.ok()) {
      const auto native = compilation.function->invoke(&argument, 1);
      expect(interpreted.ok() && native.ok() &&
                 native.value == interpreted.value,
             "native CFG negation must match exact interpreter bits");
    }
  }
}

void test_control_flow_merge() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block take_lhs = builder.create_block(0);
  const unijit::ir::Block take_rhs = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition =
      builder.less_than(builder.parameter(0), builder.parameter(1));
  expect(builder.branch(condition, take_rhs, {}, take_lhs, {}).ok(),
         "entry comparison must branch to both arms");

  expect(builder.set_insertion_block(take_lhs).ok(),
         "left arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(0)}).ok(),
         "left arm must pass its result to the merge");
  expect(builder.set_insertion_block(take_rhs).ok(),
         "right arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(1)}).ok(),
         "right arm must pass its result to the merge");
  expect(builder.set_insertion_block(merge).ok(),
         "merge block must accept insertion");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "merge must return its explicit SSA parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "diamond CFG must pass the dominance verifier");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {17, 91}).value ==
          91,
      "true branch must select the right operand");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {117, -4}).value ==
          117,
      "false branch must select the left operand");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "diamond CFG must compile to native code");
  if (compilation.ok()) {
    const std::array<Word, 2> true_args = {17, 91};
    const std::array<Word, 2> false_args = {117, -4};
    expect(compilation.function->invoke(true_args.data(), true_args.size())
                   .value == 91,
           "native true branch must select the right operand");
    expect(compilation.function->invoke(false_args.data(), false_args.size())
                   .value == 117,
           "native false branch must select the left operand");
  }
}

void test_control_flow_parallel_edge_copy() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block loop = builder.create_block(3);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  const Value two = builder.constant(2);
  const Value hundred = builder.constant(100);
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1), two})
             .ok(),
         "parallel-copy fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "parallel-copy loop block must exist");
  const Value lhs = builder.block_parameter(loop, 0);
  const Value rhs = builder.block_parameter(loop, 1);
  const Value remaining = builder.block_parameter(loop, 2);
  const Value encoded = builder.add(builder.multiply(lhs, hundred), rhs);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {rhs, lhs, next_remaining}, exit,
                     {encoded})
             .ok(),
         "backedge must support swapped block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "parallel-copy exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "parallel-copy exit must return its encoded pair");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  const std::array<Word, 2> args = {3, 7};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 703,
         "interpreter edge copies must be parallel");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "parallel-copy CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == interpreted.value,
           "native edge copies must preserve swapped loop parameters");
  }
}

void test_control_flow_float64_parallel_edge_copy() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;

  ControlFlowBuilder builder(
      {ValueType::kWord, ValueType::kFloat64, ValueType::kFloat64});
  const Block loop = builder.create_block(
      {ValueType::kWord, ValueType::kFloat64, ValueType::kFloat64});
  const Block exit =
      builder.create_block({ValueType::kFloat64, ValueType::kFloat64});
  expect(builder
             .jump(loop, {builder.parameter(0), builder.parameter(1),
                          builder.parameter(2)})
             .ok(),
         "Float64 parallel-copy fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "Float64 parallel-copy loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value lhs = builder.block_parameter(loop, 1);
  const Value rhs = builder.block_parameter(loop, 2);
  const Value next_remaining =
      builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  expect(builder
             .branch(continues, loop, {next_remaining, rhs, lhs}, exit,
                     {rhs, lhs})
             .ok(),
         "Float64 backedge must swap native register values in parallel");

  expect(builder.set_insertion_block(exit).ok(),
         "Float64 parallel-copy exit must exist");
  const Value encoded = builder.float64_add(
      builder.float64_multiply(builder.block_parameter(exit, 0),
                               builder.float64_constant(100.0)),
      builder.block_parameter(exit, 1));
  expect(builder.set_return(encoded).ok(),
         "Float64 parallel-copy exit must encode both values");
  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 parallel-copy CFG must verify");

  const std::array<Word, 3> arguments = {
      3, unijit::ir::pack_float64(3.25),
      unijit::ir::pack_float64(7.5)};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  auto compilation = Compiler::compile(function);
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(753.25) &&
             compilation.ok(),
         "Float64 register-cycle fixture must interpret and compile");
  if (compilation.ok()) {
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native Float64 edge cycles must preserve parallel semantics");
  }
}

void test_control_flow_float64_edge_spill_copy() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;

  constexpr std::size_t kValueCount = 10;
  const std::vector<ValueType> types(kValueCount, ValueType::kFloat64);
  ControlFlowBuilder builder(types);
  const Block merge = builder.create_block(types);
  std::vector<Value> reversed;
  reversed.reserve(kValueCount);
  for (std::size_t index = kValueCount; index > 0; --index) {
    reversed.push_back(builder.parameter(index - 1));
  }
  expect(builder.jump(merge, reversed).ok(),
         "oversubscribed Float64 edge must accept all values");
  expect(builder.set_insertion_block(merge).ok(),
         "oversubscribed Float64 merge must exist");
  Value encoded = builder.float64_constant(0.0);
  double expected = 0.0;
  std::array<Word, kValueCount> arguments{};
  for (std::size_t index = 0; index < kValueCount; ++index) {
    const double input = static_cast<double>(index + 1);
    arguments[index] = unijit::ir::pack_float64(input);
    const double weight = static_cast<double>(index + 1);
    encoded = builder.float64_add(
        encoded,
        builder.float64_multiply(builder.block_parameter(merge, index),
                                 builder.float64_constant(weight)));
    expected += static_cast<double>(kValueCount - index) * weight;
  }
  expect(builder.set_return(encoded).ok(),
         "oversubscribed Float64 merge must return its weighted state");
  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "oversubscribed Float64 edge CFG must verify");

  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  auto compilation = Compiler::compile(function);
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(expected) &&
             compilation.ok(),
         "Float64 spill-copy fixture must interpret and compile");
  if (compilation.ok()) {
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native Float64 spill copies must preserve every edge value");
  }
}

void test_control_flow_duplicate_float64_edge_arguments() {
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;

  const std::vector<ValueType> parameter_types(4, ValueType::kFloat64);
  const std::vector<ValueType> state_types(8, ValueType::kFloat64);
  ControlFlowBuilder builder(parameter_types);
  const auto positive = builder.create_block(0);
  const auto nonpositive = builder.create_block(0);
  const auto dispatch = builder.create_block(state_types);
  const auto body = builder.create_block(state_types);
  const Value value0 = builder.parameter(0);
  const Value value1 = builder.parameter(1);
  const Value value2 = builder.parameter(2);
  const Value value3 = builder.parameter(3);
  const Value zero = builder.float64_constant(0.0);
  expect(builder.guard_float64_nonzero(value2, 99).valid() &&
             builder
                 .branch(builder.float64_less_than(zero, value2), positive,
                         {}, nonpositive, {})
                 .ok(),
         "duplicate Float64 edge fixture must select an empty block");
  const std::vector<Value> arguments_to_dispatch = {
      value0, value1, value2, value3, value3, value1, value2, value0};
  expect(builder.set_insertion_block(positive).ok() &&
             builder.jump(dispatch, arguments_to_dispatch).ok() &&
             builder.set_insertion_block(nonpositive).ok() &&
             builder.jump(dispatch, arguments_to_dispatch).ok(),
         "duplicate Float64 edge arguments must cross empty blocks");
  expect(builder.set_insertion_block(dispatch).ok() &&
             builder.safepoint(100).valid(),
         "duplicate Float64 edge fixture must contain a safepoint");
  std::vector<Value> dispatch_arguments;
  for (std::size_t index = 0; index < state_types.size(); ++index) {
    dispatch_arguments.push_back(builder.block_parameter(dispatch, index));
  }
  expect(builder.jump(body, dispatch_arguments).ok() &&
             builder.set_insertion_block(body).ok() &&
             builder.set_return(builder.block_parameter(body, 5)).ok(),
         "duplicate Float64 edge fixture must return its sixth value");
  const auto function = std::move(builder).build();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(),
         "duplicate Float64 edge fixture must compile");
  const std::array<std::array<Word, 4>, 2> cases = {{
      {unijit::ir::pack_float64(1.0), unijit::ir::pack_float64(20.0),
       unijit::ir::pack_float64(1.0), unijit::ir::pack_float64(0.25)},
      {unijit::ir::pack_float64(20.0), unijit::ir::pack_float64(1.0),
       unijit::ir::pack_float64(-1.0), unijit::ir::pack_float64(-0.25)},
  }};
  for (const auto& arguments : cases) {
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == arguments[1],
           "duplicate Float64 edge fixture must interpret each direction");
    if (!compilation.ok()) {
      continue;
    }
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native duplicate Float64 edge arguments must preserve positions");
  }
}

void test_control_flow_preserves_nonlocal_merge_state() {
  using unijit::ir::ValueType;
  const std::vector<ValueType> state_types(4, ValueType::kFloat64);
  unijit::ir::ControlFlowBuilder builder(state_types);
  const unijit::ir::Block state = builder.create_block(state_types);
  const unijit::ir::Block true_arm = builder.create_block(0);
  const unijit::ir::Block false_arm = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(state_types);
  expect(builder
             .jump(state, {builder.parameter(0), builder.parameter(1),
                           builder.parameter(2), builder.parameter(3)})
             .ok(),
         "nonlocal merge fixture must enter its state block");

  expect(builder.set_insertion_block(state).ok(),
         "nonlocal merge state block must exist");
  const Value condition = builder.float64_less_than(
      builder.block_parameter(state, 1), builder.float64_constant(10.0));
  expect(builder.branch(condition, true_arm, {}, false_arm, {}).ok(),
         "nonlocal merge fixture must branch through empty arms");

  const std::vector<Value> state_values = {
      builder.block_parameter(state, 0), builder.block_parameter(state, 1),
      builder.block_parameter(state, 2), builder.block_parameter(state, 3)};
  expect(builder.set_insertion_block(true_arm).ok() &&
             builder.jump(merge, state_values).ok(),
         "true arm must forward nonlocal state");
  expect(builder.set_insertion_block(false_arm).ok() &&
             builder.jump(merge, state_values).ok(),
         "false arm must forward nonlocal state");
  expect(builder.set_insertion_block(merge).ok() &&
             builder.set_return(builder.block_parameter(merge, 2)).ok(),
         "merge must return the third state value");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "nonlocal merge state must satisfy CFG verification");
  const std::array<Word, 4> arguments = {
      unijit::ir::pack_float64(1.0), unijit::ir::pack_float64(2.0),
      unijit::ir::pack_float64(3.0), unijit::ir::pack_float64(4.0)};
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "nonlocal merge state must compile");
  if (compilation.ok()) {
    const auto result =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(result.ok() &&
               result.value == unijit::ir::pack_float64(3.0),
           "edge copies must preserve simultaneously live nonlocal state");
  }
}

void test_control_flow_rejects_non_dominating_value() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block left = builder.create_block(0);
  const unijit::ir::Block right = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition = builder.constant(1);
  expect(builder.branch(condition, left, {}, right, {}).ok(),
         "invalid-dominance fixture entry must be terminated");

  expect(builder.set_insertion_block(left).ok(),
         "invalid-dominance left block must exist");
  const Value leaked = builder.constant(7);
  expect(builder.jump(merge, {leaked}).ok(), "left block must reach the merge");

  expect(builder.set_insertion_block(right).ok(),
         "invalid-dominance right block must exist");
  const Value invalid = builder.add(leaked, builder.constant(1));
  expect(builder.jump(merge, {invalid}).ok(),
         "right block must reach the merge");

  expect(builder.set_insertion_block(merge).ok(),
         "invalid-dominance merge block must exist");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "invalid-dominance merge must be terminated");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(!unijit::ir::verify(function).ok(),
         "CFG verifier must reject a sibling-block SSA use");
}

void test_control_flow_safepoint() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0), zero}).ok(),
         "CFG safepoint fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "CFG safepoint loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value safepoint = builder.safepoint(314);
  expect(safepoint.valid(), "CFG safepoint must be inserted in the loop");
  const Value next_sum = builder.add(builder.add(sum, remaining), safepoint);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {next_remaining, next_sum}, exit,
                     {next_sum})
             .ok(),
         "CFG safepoint loop must branch to its backedge and exit");

  expect(builder.set_insertion_block(exit).ok(),
         "CFG safepoint exit must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "CFG safepoint exit must return its sum");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "CFG safepoint IR must verify");

  const std::array<Word, 1> args = {4};
  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size(), 100, &context);
  expect(!interpreted.ok() &&
             interpreted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interpreted.status.location() == 314 &&
             context.safepoint_polls() == 1,
         "CFG interpreter must exit at the requested loop safepoint");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "CFG safepoint loop must compile");
  if (compilation.ok()) {
    const unijit::jit::StackMapRecord* stack_map =
        compilation.function->stack_map(314);
    std::vector<std::size_t> stack_offsets;
    if (stack_map != nullptr) {
      stack_offsets.reserve(stack_map->live_values.size());
      for (const unijit::jit::StackMapValue& value :
           stack_map->live_values) {
        stack_offsets.push_back(value.frame_offset);
      }
      std::sort(stack_offsets.begin(), stack_offsets.end());
    }
    expect(stack_map != nullptr &&
               stack_map->kind == unijit::jit::StackMapKind::kSafepoint &&
               stack_map->native_offset <
                   compilation.function->stats().code_size &&
               compilation.function->stats().stack_map_count == 1 &&
               compilation.function->stats().stack_map_value_count == 4 &&
               stack_map->live_values.size() == 4 &&
               stack_map->find(remaining) != nullptr &&
               stack_map->find(sum) != nullptr &&
               stack_map->find(zero) != nullptr &&
               stack_map->find(one) != nullptr &&
               stack_map->find(safepoint) == nullptr &&
               std::adjacent_find(stack_offsets.begin(),
                                  stack_offsets.end()) ==
                   stack_offsets.end() &&
               std::all_of(
                   stack_map->live_values.begin(),
                   stack_map->live_values.end(),
                   [stack_map](const unijit::jit::StackMapValue& value) {
                     return value.type == unijit::ir::ValueType::kWord &&
                            value.frame_offset % sizeof(Word) == 0 &&
                            value.frame_offset < stack_map->frame_size;
                   }),
           "CFG safepoints must publish precise loop-live canonical stack maps");
    expect(compilation.function->requires_context(),
           "compiled CFG safepoints must require an execution context");
    context.request_interrupt();
    const auto interrupted =
        compilation.function->invoke(args.data(), args.size(), &context);
    const auto captured =
        compilation.function->reconstruct_stack_map(context);
    const auto* captured_remaining = captured.capture.find(remaining);
    const auto* captured_sum = captured.capture.find(sum);
    const auto* captured_zero = captured.capture.find(zero);
    const auto* captured_one = captured.capture.find(one);
    expect(!interrupted.ok() &&
               interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               interrupted.status.location() == 314 &&
               context.exit_site() == 314 &&
               context.safepoint_polls() == 1 && captured.ok() &&
               captured_remaining != nullptr &&
               captured_remaining->value_bits == 4 &&
               captured_sum != nullptr && captured_sum->value_bits == 0 &&
               captured_zero != nullptr && captured_zero->value_bits == 0 &&
               captured_one != nullptr && captured_one->value_bits == 1,
           "native CFG loop must exit with its complete live iteration state");

    context.clear_interrupt();
    const auto completed =
        compilation.function->invoke(args.data(), args.size(), &context);
    expect(completed.ok() && completed.value == 10 &&
               context.safepoint_polls() == 4 &&
               context.captured_value_count() == 0 &&
               !compilation.function->reconstruct_stack_map(context).ok(),
           "successful invocation must clear stale captured stack-map state");
    const auto completed_with_local_context =
        compilation.function->invoke(args.data(), args.size());
    expect(completed_with_local_context.ok() &&
               completed_with_local_context.value == 10,
           "CFG invocation must provision a local safepoint context");
    expect(compilation.function->native_entry()(args.data(), nullptr) == 10,
           "null execution contexts must bypass CFG safepoints");
  }

  unijit::jit::CompilationOptions unmeasured_options;
  unmeasured_options.measure_safepoint_polls = false;
  auto unmeasured_compilation =
      Compiler::compile(function, unmeasured_options);
  expect(unmeasured_compilation.ok(),
         "CFG safepoint telemetry must be optional");
  if (unmeasured_compilation.ok()) {
    const auto unmeasured = unmeasured_compilation.function->invoke(
        args.data(), args.size(), &context);
    expect(unmeasured.ok() && unmeasured.value == 10 &&
               context.safepoint_polls() == 0 && compilation.ok() &&
               unmeasured_compilation.function->stats().code_size <
                   compilation.function->stats().code_size,
           "disabled CFG safepoint telemetry must add no loop counts");
  }
}

void test_control_flow_stack_map_edge_liveness() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value parameter = builder.parameter(0);
  expect(builder.safepoint(315).valid() &&
             builder.jump(exit, {parameter}).ok(),
         "edge-liveness fixture must poll before passing its value");
  expect(builder.set_insertion_block(exit).ok(),
         "edge-liveness exit block must exist");
  const Value result = builder.block_parameter(exit, 0);
  expect(builder.set_return(result).ok(),
         "edge-liveness exit block must return its parameter");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "edge-liveness CFG must verify");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "edge-liveness CFG must compile");
  if (!compilation.ok()) {
    return;
  }
  const unijit::jit::StackMapRecord* stack_map =
      compilation.function->stack_map(315);
  expect(stack_map != nullptr && stack_map->live_values.size() == 1 &&
             stack_map->find(parameter) != nullptr &&
             stack_map->find(result) == nullptr,
         "CFG stack maps must translate live successor parameters to edge arguments");

  const std::array<Word, 1> arguments = {27};
  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interrupted = compilation.function->invoke(
      arguments.data(), arguments.size(), &context);
  const auto captured =
      compilation.function->reconstruct_stack_map(context);
  const auto* captured_parameter = captured.capture.find(parameter);
  expect(!interrupted.ok() && captured.ok() &&
             captured_parameter != nullptr &&
             captured_parameter->value_bits == arguments[0],
         "captured CFG edge state must survive native frame restoration");
}

void test_assumption_invalidation() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(1);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0)}).ok(),
         "assumption fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "assumption fixture loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  expect(builder.safepoint(401).valid(),
         "assumption fixture must contain a safepoint");
  const Value next = builder.subtract(remaining, one);
  const Value continues = builder.less_than(zero, next);
  expect(builder.branch(continues, loop, {next}, exit, {next}).ok(),
         "assumption fixture must branch to its backedge and exit");
  expect(builder.set_insertion_block(exit).ok(),
         "assumption fixture exit must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "assumption fixture must return its loop state");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();

  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  expect(assumptions.add(assumption, 2718, 73).ok(),
         "a valid runtime assumption must be attachable");
  expect(!assumptions.add(assumption, 2719, 74).ok(),
         "an assumption token must not be registered twice");

  unijit::runtime::DeoptimizationRecord wrong_reason;
  wrong_reason.site = 2718;
  wrong_reason.resume_offset = 73;
  wrong_reason.reason = unijit::runtime::DeoptimizationReason::kGuardFailed;
  unijit::runtime::DeoptimizationTable wrong_metadata;
  expect(wrong_metadata.add(wrong_reason).ok() &&
             !Compiler::compile(function, wrong_metadata, assumptions).ok(),
         "assumption metadata must use the invalidation semantic reason");

  auto compilation = Compiler::compile(function, assumptions);
  expect(compilation.ok(), "CFG compilation must accept valid assumptions");
  if (!compilation.ok()) {
    return;
  }
  const auto* record = compilation.function->deoptimization_record(2718);
  expect(compilation.function->requires_context() &&
             compilation.function->assumptions().size() == 1 &&
             record != nullptr && record->resume_offset == 73 &&
             record->reason ==
                 unijit::runtime::DeoptimizationReason::kAssumptionInvalidated &&
             record->recovery.size() == 1,
         "assumption compilation must publish entry recovery metadata");

  unijit::jit::CodeCache cache;
  const auto publication = cache.publish(
      "assumption-fixture", 1, std::move(compilation.function));
  expect(publication.ok() && publication.handle.assumption_count() == 1,
         "cached code leases must retain their assumption dependencies");
  if (!publication.ok()) {
    return;
  }

  const std::array<Word, 1> arguments = {1000000000};
  unijit::runtime::ExecutionContext context;
  unijit::ir::EvaluationResult outcome;
  std::thread execution([&] {
    outcome = publication.handle.invoke(arguments.data(), arguments.size(),
                                        &context);
  });
  bool observed_active = false;
  for (std::size_t spin = 0; spin < 1000000; ++spin) {
    if (assumption->active_invocations() != 0) {
      observed_active = true;
      break;
    }
    std::this_thread::yield();
  }
  const bool invalidated = assumption->invalidate();
  execution.join();
  expect(observed_active && invalidated && !assumption->valid() &&
             assumption->active_invocations() == 0,
         "assumption invalidation must wait for active code to quiesce");
  expect(!outcome.ok() &&
             outcome.status.code() == unijit::StatusCode::kRuntimeExit &&
             outcome.status.location() == 2718 &&
             context.exit_reason() ==
                 unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 2718 && !context.exit_poll_requested(),
         "invalidation must wake native safepoints and report its exit site");

  const auto reconstruction =
      publication.handle.reconstruct_deoptimization(
          2718, arguments.data(), arguments.size(), context);
  const auto* recovered_argument = reconstruction.frame.find(0);
  expect(reconstruction.ok() && reconstruction.frame.resume_offset == 73 &&
             recovered_argument != nullptr &&
             recovered_argument->value == arguments[0],
         "assumption exits must reconstruct the original entry frame");

  auto replacement_assumption =
      std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet replacement_assumptions;
  expect(replacement_assumptions.add(replacement_assumption, 2718, 73).ok(),
         "replacement code must bind a fresh assumption token");
  auto replacement_compilation =
      Compiler::compile(function, replacement_assumptions);
  const auto replacement = cache.publish(
      "assumption-fixture", 1, std::move(replacement_compilation.function));
  const std::array<Word, 1> short_arguments = {4};
  const auto replacement_result = replacement.handle.invoke(
      short_arguments.data(), short_arguments.size());
  expect(replacement_compilation.status.ok() && replacement.ok() &&
             !replacement.reused &&
             replacement.handle.generation() != publication.handle.generation() &&
             replacement_result.ok() && replacement_result.value == 0 &&
             cache.stats().assumption_invalidations == 1,
         "cache publication must replace an assumption-invalid generation");
  expect(replacement_assumption->invalidate() &&
             !cache.find("assumption-fixture", 1).valid() &&
             cache.stats().assumption_invalidations == 2,
         "cache lookup must retire an assumption-invalid generation");

  context.request_interrupt();
  const auto rejected_again = publication.handle.invoke(
      arguments.data(), arguments.size(), &context);
  expect(!rejected_again.ok() &&
             rejected_again.status.code() ==
                 unijit::StatusCode::kRuntimeExit &&
             rejected_again.status.location() == 2718 &&
             context.interrupt_requested(),
         "entry invalidation must preserve an independent sticky interrupt");
  context.clear_interrupt();
  expect(!assumption->invalidate(),
         "repeated assumption invalidation must be idempotent");
  expect(!Compiler::compile(function, assumptions).ok(),
         "compilation must reject an already invalidated assumption");
}

void test_hotness_and_tiered_switching() {
  unijit::jit::CodeCache cache({16, 1024U * 1024U});
  FunctionBuilder baseline_builder(1);
  expect(baseline_builder
             .set_return(baseline_builder.add(baseline_builder.parameter(0),
                                              baseline_builder.constant(1)))
             .ok(),
         "tiered baseline fixture must record its result");
  auto baseline_compilation =
      Compiler::compile(std::move(baseline_builder).build());
  auto baseline_publication = cache.publish(
      "tiered-baseline", 1, std::move(baseline_compilation.function));
  expect(baseline_compilation.status.ok() && baseline_publication.ok(),
         "tiered baseline fixture must compile and publish");

  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  expect(assumptions.add(assumption, 808, 12).ok(),
         "tiered optimized fixture must bind an assumption");
  FunctionBuilder optimized_builder(1);
  expect(optimized_builder
             .set_return(optimized_builder.add(optimized_builder.parameter(0),
                                               optimized_builder.constant(1)))
             .ok(),
         "tiered optimized fixture must record its result");
  auto optimized_compilation = Compiler::compile(
      std::move(optimized_builder).build(), assumptions);
  auto optimized_publication = cache.publish(
      "tiered-optimized", 1, std::move(optimized_compilation.function));
  expect(optimized_compilation.status.ok() && optimized_publication.ok(),
         "tiered optimized fixture must compile and publish");

  unijit::jit::TieredCode tiered({3, 5, 2});
  const std::array<Word, 1> arguments = {41};
  unijit::runtime::OsrFrame unpublished_osr_frame(850, 17);
  unijit::runtime::OsrEntryPlan unpublished_osr_plan(850, 17);
  expect(unpublished_osr_frame
             .add(1, unijit::ir::ValueType::kWord, 41)
             .ok() &&
             unpublished_osr_plan
                 .add_argument(1, unijit::ir::ValueType::kWord)
                 .ok(),
         "tiered OSR fixture must define its interpreter state");
  expect(!tiered.snapshot().valid() &&
             !tiered.invoke(arguments.data(), arguments.size()).ok() &&
             !tiered
                  .enter_osr(unpublished_osr_frame, unpublished_osr_plan)
                  .entered(),
         "tiered invocation and OSR must reject a missing baseline");
  expect(tiered.publish_baseline(baseline_publication.handle).ok(),
         "tiered code must publish an assumption-free baseline");
  const auto baseline_snapshot = tiered.snapshot();
  expect(baseline_snapshot.valid() &&
             baseline_snapshot.tier == unijit::jit::CodeTier::kBaseline &&
             baseline_snapshot.generation != 0,
         "tiered baseline publication must expose a stable generation");

  FunctionBuilder return_mismatch_builder(1);
  expect(return_mismatch_builder
             .set_return(return_mismatch_builder.float64_constant(1.0))
             .ok(),
         "return-mismatch fixture must record its Float64 result");
  auto return_mismatch_compilation =
      Compiler::compile(std::move(return_mismatch_builder).build());
  auto return_mismatch_publication = cache.publish(
      "tiered-return-mismatch", 1,
      std::move(return_mismatch_compilation.function));
  expect(return_mismatch_publication.ok() &&
             !tiered.publish_optimized(return_mismatch_publication.handle,
                                       baseline_snapshot.generation)
                  .ok(),
         "tiered publication must reject a mismatched return type");

  FunctionBuilder parameter_mismatch_builder(
      std::vector<unijit::ir::ValueType>{
          unijit::ir::ValueType::kFloat64});
  expect(parameter_mismatch_builder
             .set_return(parameter_mismatch_builder.constant(1))
             .ok(),
         "parameter-mismatch fixture must record its Word result");
  auto parameter_mismatch_compilation =
      Compiler::compile(std::move(parameter_mismatch_builder).build());
  auto parameter_mismatch_publication = cache.publish(
      "tiered-parameter-mismatch", 1,
      std::move(parameter_mismatch_compilation.function));
  expect(parameter_mismatch_publication.ok() &&
             !tiered.publish_optimized(parameter_mismatch_publication.handle,
                                       baseline_snapshot.generation)
                  .ok(),
         "tiered publication must reject mismatched parameter types");

  for (std::size_t invocation = 0; invocation < 2; ++invocation) {
    const auto result = tiered.invoke(arguments.data(), arguments.size());
    expect(result.ok() && result.result.value == 42 &&
               result.attempted_tier ==
                   unijit::jit::CodeTier::kBaseline,
           "cold tiered calls must execute the baseline");
  }
  expect(!tiered.try_begin_optimization(),
         "hotness must not trigger before the invocation threshold");
  expect(tiered.invoke(arguments.data(), arguments.size()).ok() &&
             tiered.try_begin_optimization() &&
             !tiered.try_begin_optimization(),
         "one compiler must claim a hot tiered generation");
  expect(tiered
             .publish_optimized(optimized_publication.handle,
                                baseline_snapshot.generation)
             .ok(),
         "the claimed optimized tier must publish over its baseline");
  const auto optimized_snapshot = tiered.snapshot();
  expect(optimized_snapshot.tier == unijit::jit::CodeTier::kOptimized &&
             optimized_snapshot.generation != baseline_snapshot.generation &&
             tiered.stats().hotness.successful_compilations == 1,
         "optimized publication must advance the tiered generation");
  expect(!tiered
              .publish_optimized(optimized_publication.handle,
                                 baseline_snapshot.generation)
              .ok(),
         "late compilation must not overwrite a newer generation");

  const auto optimized_result =
      tiered.invoke(arguments.data(), arguments.size());
  expect(optimized_result.ok() && optimized_result.result.value == 42 &&
             optimized_result.attempted_handle.generation() ==
                 optimized_snapshot.handle.generation() &&
             optimized_result.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized,
         "valid assumptions must select the optimized tier");
  expect(assumption->invalidate(),
         "tiered optimized assumption must invalidate once");
  unijit::runtime::ExecutionContext context;
  const auto fallback = tiered.invoke(
      arguments.data(), arguments.size(), &context,
      unijit::jit::DeoptimizationPolicy::kRetryBaseline);
  expect(fallback.ok() && fallback.result.value == 42 &&
             fallback.deoptimized && fallback.retried_baseline &&
             fallback.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized &&
             tiered.snapshot().tier ==
                 unijit::jit::CodeTier::kBaseline,
         "restartable assumption exits must withdraw and retry the baseline");
  const auto retained_exit = optimized_snapshot.handle.invoke(
      arguments.data(), arguments.size(), &context);
  expect(!retained_exit.ok() &&
             retained_exit.status.code() ==
                 unijit::StatusCode::kRuntimeExit,
         "retained optimized snapshots must stay safe after withdrawal");
  expect(!optimized_snapshot.handle.assumptions_valid() &&
             !tiered.publish_optimized(optimized_snapshot.handle).ok(),
         "tiered publication must reject an already stale optimized handle");

  expect(!tiered.try_begin_optimization(),
         "withdrawal must apply a hotness retry delay");
  tiered.record_backedges(1);
  expect(!tiered.try_begin_optimization(),
         "one backedge must not exhaust a two-event retry delay");
  tiered.record_backedges(1);
  expect(tiered.try_begin_optimization() &&
             tiered.report_optimization_failure().ok() &&
             tiered.stats().hotness.failed_compilations == 1,
         "failed tier compilation must rearm profiling without a claim storm");

  FunctionBuilder stable_optimized_builder(1);
  expect(stable_optimized_builder
             .set_return(stable_optimized_builder.add(
                 stable_optimized_builder.parameter(0),
                 stable_optimized_builder.constant(1)))
             .ok(),
         "stable optimized fixture must record its result");
  auto stable_compilation =
      Compiler::compile(std::move(stable_optimized_builder).build());
  auto stable_publication = cache.publish(
      "tiered-stable", 1, std::move(stable_compilation.function));
  expect(stable_compilation.status.ok() && stable_publication.ok(),
         "stable optimized fixture must compile and publish");

  unijit::runtime::OsrFrame switching_frame(851, 18);
  unijit::runtime::OsrEntryPlan switching_plan(851, 18);
  expect(switching_frame.add(70, unijit::ir::ValueType::kWord, 41).ok() &&
             switching_plan
                 .add_argument(70, unijit::ir::ValueType::kWord)
                 .ok(),
         "concurrent tiered OSR fixture must define its live slot");
  std::atomic<std::size_t> switching_errors{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> readers;
  for (std::size_t index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t invocation = 0; invocation < 2000; ++invocation) {
        bool valid = false;
        if (invocation % 2 == 0) {
          const auto result =
              tiered.invoke(arguments.data(), arguments.size());
          valid = result.ok() && result.result.value == 42;
        } else {
          const auto result = tiered.enter_osr(switching_frame,
                                               switching_plan);
          valid = result.ok() && result.entry.result.value == 42 &&
                  result.attempted_handle.valid();
        }
        if (!valid) {
          switching_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::size_t iteration = 0; iteration < 200; ++iteration) {
    const auto current = tiered.snapshot();
    if (current.tier == unijit::jit::CodeTier::kBaseline) {
      if (!tiered
               .publish_optimized(stable_publication.handle,
                                  current.generation)
               .ok()) {
        switching_errors.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (!tiered.withdraw_optimized(current.generation)) {
      switching_errors.fetch_add(1, std::memory_order_relaxed);
    }
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  const auto tiered_stats = tiered.stats();
  expect(switching_errors.load(std::memory_order_relaxed) == 0 &&
             tiered_stats.promotions > 0 && tiered_stats.withdrawals > 0 &&
             tiered_stats.assumption_deoptimizations == 1 &&
             tiered_stats.baseline_retries == 1 &&
             tiered_stats.osr_attempts == 4001 &&
             tiered_stats.osr_entries == 4000 &&
             tiered_stats.osr_exits == 0,
         "concurrent invocation and OSR must retain safe immutable tier snapshots");

  auto osr_assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet osr_assumptions;
  expect(osr_assumptions.add(osr_assumption, 809, 13).ok(),
         "tiered OSR assumption fixture must bind its dependency");
  FunctionBuilder osr_optimized_builder(1);
  expect(osr_optimized_builder
             .set_return(osr_optimized_builder.add(
                 osr_optimized_builder.parameter(0),
                 osr_optimized_builder.constant(1)))
             .ok(),
         "tiered OSR optimized fixture must record its result");
  auto osr_optimized_compilation = Compiler::compile(
      std::move(osr_optimized_builder).build(), osr_assumptions);
  auto osr_optimized_publication = cache.publish(
      "tiered-osr-optimized", 1,
      std::move(osr_optimized_compilation.function));
  expect(osr_optimized_compilation.status.ok() &&
             osr_optimized_publication.ok(),
         "tiered OSR optimized fixture must compile and publish");

  unijit::jit::TieredCode osr_tiered;
  expect(osr_tiered.publish_baseline(baseline_publication.handle).ok(),
         "tiered OSR must publish its baseline");
  const auto osr_baseline = osr_tiered.snapshot();
  expect(osr_tiered
             .publish_optimized(osr_optimized_publication.handle,
                                osr_baseline.generation)
             .ok(),
         "tiered OSR must publish its optimized generation");
  unijit::runtime::OsrFrame osr_frame(852, 19);
  unijit::runtime::OsrEntryPlan osr_plan(852, 19);
  expect(osr_frame.add(71, unijit::ir::ValueType::kWord, 41).ok() &&
             osr_plan.add_argument(71, unijit::ir::ValueType::kWord).ok(),
         "tiered OSR transfer must define its frame mapping");
  const auto successful_osr = osr_tiered.enter_osr(osr_frame, osr_plan);
  expect(successful_osr.ok() && successful_osr.entry.result.value == 42 &&
             successful_osr.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized,
         "tiered OSR must enter the active optimized generation");
  expect(osr_assumption->invalidate(),
         "tiered OSR assumption must invalidate once");
  unijit::runtime::ExecutionContext osr_context;
  const auto exited_osr =
      osr_tiered.enter_osr(osr_frame, osr_plan, &osr_context);
  const auto osr_reconstruction =
      exited_osr.attempted_handle.reconstruct_deoptimization(
          809, exited_osr.entry.arguments.data(),
          exited_osr.entry.arguments.count, osr_context);
  const auto osr_stats = osr_tiered.stats();
  expect(exited_osr.entered() && !exited_osr.ok() &&
             exited_osr.deoptimized &&
             exited_osr.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized &&
             exited_osr.entry.result.status.location() == 809 &&
             osr_reconstruction.ok() &&
             osr_reconstruction.frame.find(0) != nullptr &&
             osr_reconstruction.frame.find(0)->value == 41 &&
             osr_tiered.snapshot().tier ==
                 unijit::jit::CodeTier::kBaseline &&
             osr_stats.osr_attempts == 2 && osr_stats.osr_entries == 2 &&
             osr_stats.osr_exits == 1 &&
             osr_stats.assumption_deoptimizations == 1,
         "assumption exits from tiered OSR must retain the attempted generation and withdraw it");
}

void test_compilation_scheduler() {
  using namespace std::chrono_literals;
  using unijit::jit::CompilationPriority;
  using unijit::jit::CompilationRequest;
  using unijit::jit::CompilationScheduler;
  using unijit::jit::CompilationSchedulerOptions;
  using unijit::jit::CompilationTaskState;
  using unijit::jit::SchedulerShutdownMode;

  auto invalid_workers = CompilationScheduler::create(
      CompilationSchedulerOptions{257, 1, 1});
  auto invalid_capacity = CompilationScheduler::create(
      CompilationSchedulerOptions{1, 0, 1});
  expect(!invalid_workers.ok() && !invalid_capacity.ok(),
         "scheduler creation must reject unsafe resource limits");

  auto creation = CompilationScheduler::create(
      CompilationSchedulerOptions{1, 2, 2});
  expect(creation.ok(), "bounded compilation scheduler must start");
  if (!creation.ok()) {
    return;
  }
  auto& scheduler = *creation.scheduler;

  std::mutex gate_mutex;
  std::condition_variable gate;
  bool release_blocker = false;
  std::atomic<bool> blocker_started{false};
  const auto blocker = scheduler.try_submit(CompilationRequest{
      "scheduler/blocker", 1, 1, CompilationPriority::kNormal,
      [&](const unijit::jit::CompilationCancellation& cancellation) {
        blocker_started.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate.wait(lock, [&] {
          return release_blocker || cancellation.stop_requested();
        });
        return cancellation.stop_requested()
                   ? unijit::Status{unijit::StatusCode::kCancelled,
                                    "blocker cancelled"}
                   : unijit::Status::ok_status();
      }});
  expect(blocker.ok() && blocker.enqueued &&
             blocker.ticket.result().code() ==
                 unijit::StatusCode::kUnavailable,
         "accepted compilation must expose a pending ticket");
  for (std::size_t spin = 0; spin < 1000000 &&
                             !blocker_started.load(std::memory_order_acquire);
       ++spin) {
    std::this_thread::yield();
  }
  expect(blocker_started.load(std::memory_order_acquire),
         "scheduler worker must begin the blocking fixture");

  std::mutex order_mutex;
  std::vector<int> execution_order;
  const auto background = scheduler.try_submit(CompilationRequest{
      "scheduler/background", 7, 1, CompilationPriority::kBackground,
      [&](const unijit::jit::CompilationCancellation&) {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1);
        return unijit::Status::ok_status();
      }});
  const auto urgent = scheduler.try_submit(CompilationRequest{
      "scheduler/urgent", 7, 1, CompilationPriority::kUrgent,
      [&](const unijit::jit::CompilationCancellation&) {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2);
        return unijit::Status::ok_status();
      }});
  const auto duplicate = scheduler.try_submit(CompilationRequest{
      "scheduler/background", 7, 1, CompilationPriority::kUrgent,
      [](const unijit::jit::CompilationCancellation&) {
        return unijit::Status{unijit::StatusCode::kCodeGenerationFailed,
                              "deduplicated job must not execute"};
      }});
  const auto rejected = scheduler.try_submit(CompilationRequest{
      "scheduler/rejected", 1, 1, CompilationPriority::kNormal,
      [](const unijit::jit::CompilationCancellation&) {
        return unijit::Status::ok_status();
      }});
  const auto timed_out = scheduler.submit_for(
      CompilationRequest{
          "scheduler/timed-out", 1, 1, CompilationPriority::kNormal,
          [](const unijit::jit::CompilationCancellation&) {
            return unijit::Status::ok_status();
          }},
      10ms);
  expect(background.ok() && urgent.ok() && duplicate.ok() &&
             duplicate.deduplicated && !duplicate.enqueued &&
             duplicate.ticket.id() == background.ticket.id(),
         "scheduler must deduplicate the same identity and generation");
  expect(!rejected.ok() &&
             rejected.status.code() ==
                 unijit::StatusCode::kResourceExhausted &&
             !timed_out.ok() &&
             timed_out.status.code() ==
                 unijit::StatusCode::kDeadlineExceeded,
         "bounded admission must reject or time out under backpressure");
  expect(!scheduler.wait_idle_for(1ms),
         "busy scheduler must not report an idle state");

  expect(background.ticket.cancel() &&
             background.ticket.wait().code() ==
                 unijit::StatusCode::kCancelled &&
             background.ticket.state() == CompilationTaskState::kCancelled,
         "queued cancellation must complete its ticket immediately");
  const auto normal = scheduler.try_submit(CompilationRequest{
      "scheduler/normal", 8, 1, CompilationPriority::kNormal,
      [&](const unijit::jit::CompilationCancellation&) {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(3);
        return unijit::Status::ok_status();
      }});
  expect(normal.ok(),
         "queued cancellation must immediately release admission capacity");

  unijit::jit::CompilationSubmission throwing;
  std::thread blocked_submitter([&] {
    throwing = scheduler.submit_for(
        CompilationRequest{
            "scheduler/throwing", 1, 1, CompilationPriority::kBackground,
            [](const unijit::jit::CompilationCancellation&) -> unijit::Status {
              throw std::runtime_error("fixture");
            }},
        1s);
  });
  for (std::size_t spin = 0;
       spin < 1000000 && scheduler.stats().submission_waits != 2; ++spin) {
    std::this_thread::yield();
  }
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_blocker = true;
  }
  gate.notify_all();
  blocked_submitter.join();
  expect(throwing.ok(),
         "blocking submission must acquire capacity when cancellation wakes it");
  expect(blocker.ticket.wait().ok() && urgent.ticket.wait().ok() &&
             normal.ticket.wait().ok() &&
             throwing.ticket.wait().code() ==
                 unijit::StatusCode::kCodeGenerationFailed &&
             throwing.ticket.state() == CompilationTaskState::kFailed,
         "scheduler must isolate job exceptions and complete other work");
  scheduler.wait_idle();
  expect(execution_order.size() == 2 && execution_order[0] == 2 &&
             execution_order[1] == 3,
         "weighted scheduler must select urgent work before normal work");

  expect(scheduler.shutdown(SchedulerShutdownMode::kDrain).ok() &&
             scheduler.shutdown(SchedulerShutdownMode::kDrain).ok() &&
             !scheduler.accepting(),
         "draining shutdown must be deterministic and idempotent");
  const auto closed = scheduler.try_submit(CompilationRequest{
      "scheduler/closed", 1, 1, CompilationPriority::kNormal,
      [](const unijit::jit::CompilationCancellation&) {
        return unijit::Status::ok_status();
      }});
  const auto statistics = scheduler.stats();
  expect(!closed.ok() &&
             closed.status.code() == unijit::StatusCode::kUnavailable &&
             statistics.submitted == 5 && statistics.deduplicated == 1 &&
             statistics.started == 4 && statistics.succeeded == 3 &&
             statistics.failed == 1 && statistics.cancelled == 1 &&
             statistics.rejected_capacity == 1 &&
             statistics.rejected_closed == 1 &&
             statistics.submission_waits == 2 &&
             statistics.submission_timeouts == 1 &&
             statistics.peak_queued_tasks == 2 &&
             statistics.peak_queued_bytes == 2 &&
             statistics.queued_tasks == 0 &&
             statistics.active_workers == 0,
         "scheduler telemetry must reconcile its complete lifecycle");

  auto cancellation_creation = CompilationScheduler::create(
      CompilationSchedulerOptions{1, 2, 2});
  expect(cancellation_creation.ok(),
         "cancellation scheduler fixture must start");
  if (!cancellation_creation.ok()) {
    return;
  }
  auto& cancellation_scheduler = *cancellation_creation.scheduler;
  std::atomic<bool> running_started{false};
  const auto running = cancellation_scheduler.try_submit(CompilationRequest{
      "scheduler/running-cancel", 1, 1, CompilationPriority::kNormal,
      [&](const unijit::jit::CompilationCancellation& cancellation) {
        running_started.store(true, std::memory_order_release);
        while (!cancellation.stop_requested()) {
          std::this_thread::yield();
        }
        return unijit::Status{unijit::StatusCode::kCancelled,
                              "cooperative cancellation"};
      }});
  for (std::size_t spin = 0; spin < 1000000 &&
                             !running_started.load(std::memory_order_acquire);
       ++spin) {
    std::this_thread::yield();
  }
  expect(running.ticket.cancel() && running.ticket.cancellation_requested() &&
             running.ticket.wait().code() ==
                 unijit::StatusCode::kCancelled &&
             cancellation_scheduler
                 .shutdown(SchedulerShutdownMode::kCancel)
                 .ok(),
         "running jobs must observe cooperative ticket cancellation");

  auto shutdown_creation = CompilationScheduler::create(
      CompilationSchedulerOptions{1, 2, 2});
  expect(shutdown_creation.ok(), "cancel-shutdown fixture must start");
  if (!shutdown_creation.ok()) {
    return;
  }
  auto& shutdown_scheduler = *shutdown_creation.scheduler;
  std::atomic<bool> shutdown_running{false};
  std::atomic<bool> queued_executed{false};
  const auto active = shutdown_scheduler.try_submit(CompilationRequest{
      "scheduler/shutdown-active", 1, 1, CompilationPriority::kNormal,
      [&](const unijit::jit::CompilationCancellation& cancellation) {
        shutdown_running.store(true, std::memory_order_release);
        while (!cancellation.stop_requested()) {
          std::this_thread::yield();
        }
        return unijit::Status::ok_status();
      }});
  for (std::size_t spin = 0; spin < 1000000 &&
                             !shutdown_running.load(std::memory_order_acquire);
       ++spin) {
    std::this_thread::yield();
  }
  const auto queued = shutdown_scheduler.try_submit(CompilationRequest{
      "scheduler/shutdown-queued", 1, 1, CompilationPriority::kNormal,
      [&](const unijit::jit::CompilationCancellation&) {
        queued_executed.store(true, std::memory_order_release);
        return unijit::Status::ok_status();
      }});
  expect(active.ok() && queued.ok() &&
             shutdown_scheduler.shutdown(SchedulerShutdownMode::kCancel).ok() &&
             active.ticket.wait().code() ==
                 unijit::StatusCode::kCancelled &&
             queued.ticket.wait().code() ==
                 unijit::StatusCode::kCancelled &&
             !queued_executed.load(std::memory_order_acquire) &&
             shutdown_scheduler.stats().cancelled == 2,
         "cancel shutdown must stop queued admission and join active work");
}

void test_control_flow_execution_budget() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block loop = builder.create_block(0);
  expect(builder.jump(loop, {}).ok(), "entry must reach the infinite loop");
  expect(builder.set_insertion_block(loop).ok(),
         "infinite-loop block must exist");
  expect(builder.jump(loop, {}).ok(), "loop must jump to itself");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "self-loop must be a valid control-flow graph");
  const auto result =
      unijit::ir::ControlFlowInterpreter::evaluate(function, nullptr, 0, 10);
  expect(!result.ok() &&
             result.status.code() == unijit::StatusCode::kResourceExhausted,
         "CFG interpreter must stop when its execution budget is exhausted");
}

void test_control_flow_compact_spill_frame() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::jit::CompilationOptions;
  using unijit::jit::OptimizationLevel;

  ControlFlowBuilder builder(1);
  const Value returned = builder.parameter(0);
  std::vector<Block> blocks;
  blocks.reserve(80);
  for (std::size_t index = 0; index < 80; ++index) {
    blocks.push_back(builder.create_block(0));
  }
  expect(builder.jump(blocks.front(), {}).ok(),
         "large compact-frame CFG must enter its first block");
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    expect(builder.set_insertion_block(blocks[index]).ok(),
           "large compact-frame CFG block must exist");
    std::array<Value, 16> locals;
    for (std::size_t local_index = 0; local_index < locals.size();
         ++local_index) {
      locals[local_index] = builder.constant(
          static_cast<Word>(index * locals.size() + local_index));
    }
    Value combined = locals.front();
    for (std::size_t local_index = 1; local_index < locals.size();
         ++local_index) {
      combined = builder.add(combined, locals[local_index]);
    }
    (void)combined;
    if (index + 1 < blocks.size()) {
      expect(builder.jump(blocks[index + 1], {}).ok(),
             "large compact-frame CFG blocks must remain connected");
    } else {
      expect(builder.set_return(returned).ok(),
             "large compact-frame CFG must return its dominating input");
    }
  }
  const auto function = std::move(builder).build();
  expect(function.nodes().size() > 2048 && unijit::ir::verify(function).ok(),
         "large compact-frame CFG fixture must exceed fixed AArch64 and "
         "RISC-V node-indexed frame limits");

  auto compilation = Compiler::compile(
      function, CompilationOptions{OptimizationLevel::kBaseline});
  expect(compilation.ok(),
         "large CFGs with little cross-block state must compile");
  if (!compilation.ok()) {
    return;
  }
  const std::array<Word, 1> arguments = {0x123456789};
  const auto result =
      compilation.function->invoke(arguments.data(), arguments.size());
  expect(result.ok() && result.value == arguments.front() &&
             compilation.function->stats().input_ir_nodes > 2048 &&
             compilation.function->stats().spill_slots <= 16,
         "CFG spill frames must track actual stack-resident values, not SSA "
         "node count");
}

void test_control_flow_split_register_classes() {
  using unijit::ir::Block;
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;
  using unijit::jit::detail::ControlFlowMoveSource;

  ControlFlowBuilder builder({ValueType::kWord, ValueType::kWord,
                              ValueType::kFloat64, ValueType::kFloat64});
  const Value word0 = builder.parameter(0);
  const Value word1 = builder.parameter(1);
  const Value float0 = builder.parameter(2);
  const Value float1 = builder.parameter(3);
  const Block merge = builder.create_block(
      {ValueType::kWord, ValueType::kWord, ValueType::kFloat64,
       ValueType::kFloat64});
  expect(builder.jump(merge, {word1, word0, float1, float0}).ok(),
         "mixed-class edge must accept independently permuted values");
  expect(builder.set_insertion_block(merge).ok(),
         "mixed-class merge block must exist");
  const Value merged_word0 = builder.block_parameter(merge, 0);
  const Value merged_word1 = builder.block_parameter(merge, 1);
  const Value merged_float0 = builder.block_parameter(merge, 2);
  const Value merged_float1 = builder.block_parameter(merge, 3);
  const Value word_sum = builder.add(merged_word0, merged_word1);
  const Value float_sum = builder.float64_add(merged_float0, merged_float1);
  const Value float_positive =
      builder.float64_less_than(builder.float64_constant(0.0), float_sum);
  expect(builder.set_return(builder.add(word_sum, float_positive)).ok(),
         "mixed-class merge must return a Word result");
  const auto function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "mixed-class allocation fixture must verify");

  const unijit::jit::detail::StackMapRequirements requirements;
  const auto allocation =
      unijit::jit::detail::allocate_control_flow_registers(
          function, 2, 2, requirements);
  expect(allocation.status.ok(),
         "split allocation must provide independent register banks");
  expect(allocation.register_indices[word0.id()] == 0 &&
             allocation.register_indices[float0.id()] == 0,
         "Word and Float64 values may occupy the same class-local index");

  const auto moves = unijit::jit::detail::plan_control_flow_edge_moves(
      function, function.blocks()[function.entry_block().id()]
                    .terminator.true_edge,
      allocation, function.entry_block().id());
  expect(moves.uses_registers && moves.moves.size() == 6,
         "two independent register cycles must use typed scratch moves");
  std::size_t word_temporaries = 0;
  std::size_t float_temporaries = 0;
  for (const auto& move : moves.moves) {
    if (move.destination_index !=
        unijit::jit::detail::ValueLocation::kNone) {
      continue;
    }
    expect(move.source_kind == ControlFlowMoveSource::kRegister,
           "cycle scratch must save a live register");
    if (move.type == ValueType::kFloat64) {
      ++float_temporaries;
    } else {
      ++word_temporaries;
    }
  }
  expect(word_temporaries == 1 && float_temporaries == 1,
         "parallel copies must break one cycle in each register class");
}

void test_control_flow_float64_lhs_register_reuse() {
  using unijit::ir::ControlFlowBuilder;
  using unijit::ir::ValueType;

  ControlFlowBuilder builder({ValueType::kFloat64, ValueType::kFloat64});
  const Value lhs = builder.parameter(0);
  const Value rhs = builder.parameter(1);
  const Value sum = builder.float64_add(lhs, rhs);
  const Value product = builder.float64_multiply(sum, rhs);
  expect(builder.set_return(product).ok(),
         "Float64 reuse fixture must return its recurrence");
  const auto function = std::move(builder).build();
  const auto allocation =
      unijit::jit::detail::allocate_control_flow_registers(
          function, 1, 2,
          unijit::jit::detail::StackMapRequirements{}, true);
  expect(allocation.status.ok() &&
             allocation.register_indices[lhs.id()] ==
                 allocation.register_indices[sum.id()] &&
             allocation.register_indices[sum.id()] ==
                 allocation.register_indices[product.id()],
         "dead Float64 left operands must donate their register to the result");
}

void test_control_flow_builder_rejects_edge_arity() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target = builder.create_block(1);
  expect(!builder.jump(target, {}).ok(),
         "builder must reject an edge with missing block arguments");
}

}  // namespace

int main() {
  test_target_profiles();
  test_bounded_memory_interpreter();
  test_bounded_memory_control_flow_interpreter();
  test_code_cache_lifecycle();
  test_code_cache_concurrency();
  test_verifier_rejects_forward_reference();
  test_constant_native_function();
  test_execution_context_lifecycle();
  test_safepoint_ir_and_interpreter();
  test_differential_arithmetic();
  test_float64_ir_and_interpreter();
  test_float64_division();
  test_word_unary_operations();
  test_word_bitwise_operations();
  test_word_shift_operations();
  test_word_floor_arithmetic();
  test_word_comparisons();
  test_float64_negation();
  test_float64_comparisons();
  test_float64_nonzero_guard();
  test_deoptimization_reconstruction();
  test_transactional_object_materialization();
  test_on_stack_replacement_entry();
  test_compilation_resource_limits();
  test_constant_float64_nonzero_guard_elimination();
  test_runtime_exit_site_identity();
  test_verifier_rejects_mixed_arithmetic();
  test_float64_spill_path();
  test_float64_preserves_host_abi();
  test_runtime_helper_call();
  test_effectful_dead_runtime_call();
  test_float64_runtime_helper_call();
  test_verifier_rejects_null_runtime_helper();
  test_spill_path();
  test_argument_validation();
  test_optimization_pipeline();
  test_control_flow_optimization_pipeline();
  test_control_flow_branch_and_value_canonicalization();
  test_optimization_exit_state_mapping();
  test_float64_constant_folding();
  test_control_flow_runtime_helper_ir();
  test_control_flow_runtime_call_loop();
  test_control_flow_runtime_call_spills();
  test_control_flow_effectful_dead_runtime_call();
  test_control_flow_runtime_call_exits();
  test_control_flow_counted_loop();
  test_control_flow_float64_loop();
  test_control_flow_float64_guard_deoptimization();
  test_control_flow_rejects_mixed_edge_types();
  test_control_flow_rejects_mixed_return_types();
  test_control_flow_float64_comparisons();
  test_control_flow_float64_negation();
  test_control_flow_merge();
  test_control_flow_parallel_edge_copy();
  test_control_flow_float64_parallel_edge_copy();
  test_control_flow_float64_edge_spill_copy();
  test_control_flow_duplicate_float64_edge_arguments();
  test_control_flow_preserves_nonlocal_merge_state();
  test_control_flow_rejects_non_dominating_value();
  test_control_flow_safepoint();
  test_control_flow_stack_map_edge_liveness();
  test_assumption_invalidation();
  test_hotness_and_tiered_switching();
  test_compilation_scheduler();
  test_control_flow_split_register_classes();
  test_control_flow_float64_lhs_register_reuse();
  test_control_flow_compact_spill_frame();
  test_control_flow_execution_budget();
  test_control_flow_builder_rejects_edge_arity();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all UniJIT tests passed\n";
  return EXIT_SUCCESS;
}
