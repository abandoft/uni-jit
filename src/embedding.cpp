#include "unijit/embedding.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/target.h"
#include "unijit/runtime/execution_context.h"
#include "unijit/status.h"

struct unijit_error_v1 final {
  unijit_status_code_v1 code{UNIJIT_STATUS_OK_V1};
  std::uint64_t detail{0};
  std::string message;
};

struct unijit_builder_v1 final {
  std::unique_ptr<unijit::ir::FunctionBuilder> implementation;
  std::vector<unijit::ir::ValueType> value_types;
  std::vector<unijit::ir::FastCallDescriptor> fast_calls;
  std::size_t maximum_nodes{0};
  std::size_t maximum_ir_arguments{0};
  std::size_t maximum_fast_calls{0};
  std::size_t maximum_patch_cells{0};
  std::size_t call_arguments{0};
  std::size_t patch_cells{0};
};

struct unijit_function_v1 final {
  explicit unijit_function_v1(unijit::ir::Function value)
      : implementation(std::move(value)) {}

  unijit::ir::Function implementation;
};

struct unijit_compiler_v1 final {
  unijit::jit::CompilationOptions options;
};

struct unijit_compiled_function_v1 final {
  explicit unijit_compiled_function_v1(
      std::unique_ptr<unijit::jit::CompiledFunction> value)
      : implementation(std::move(value)) {}

  std::unique_ptr<unijit::jit::CompiledFunction> implementation;
};

struct unijit_execution_context_v1 final {
  unijit::runtime::ExecutionContext implementation;
};

struct unijit_code_cache_v1 final {
  unijit_code_cache_v1(unijit::jit::CodeCacheLimits limits,
                       unijit::jit::TargetProfile target)
      : implementation(limits, target) {}

  unijit::jit::CodeCache implementation;
};

struct unijit_code_handle_v1 final {
  explicit unijit_code_handle_v1(unijit::jit::CodeHandle value)
      : implementation(std::move(value)) {}

  unijit::jit::CodeHandle implementation;
};

namespace {

constexpr std::size_t kMaximumDiagnosticBytes = 1024;

static_assert(std::is_same<unijit_word_v1, unijit::ir::Word>::value,
              "the C ABI word must match the internal value-bits word");
static_assert(sizeof(void*) == 8, "the commercial C ABI requires 64-bit hosts");

unijit_status_code_v1 status_code(unijit::StatusCode code) noexcept {
  switch (code) {
    case unijit::StatusCode::kOk:
      return UNIJIT_STATUS_OK_V1;
    case unijit::StatusCode::kInvalidArgument:
      return UNIJIT_STATUS_INVALID_ARGUMENT_V1;
    case unijit::StatusCode::kInvalidIr:
      return UNIJIT_STATUS_INVALID_IR_V1;
    case unijit::StatusCode::kUnsupportedArchitecture:
      return UNIJIT_STATUS_UNSUPPORTED_ARCHITECTURE_V1;
    case unijit::StatusCode::kResourceExhausted:
      return UNIJIT_STATUS_RESOURCE_EXHAUSTED_V1;
    case unijit::StatusCode::kCodeGenerationFailed:
      return UNIJIT_STATUS_CODE_GENERATION_FAILED_V1;
    case unijit::StatusCode::kMemoryProtectionFailed:
      return UNIJIT_STATUS_MEMORY_PROTECTION_FAILED_V1;
    case unijit::StatusCode::kExecutionInterrupted:
      return UNIJIT_STATUS_EXECUTION_INTERRUPTED_V1;
    case unijit::StatusCode::kRuntimeExit:
      return UNIJIT_STATUS_RUNTIME_EXIT_V1;
    case unijit::StatusCode::kCancelled:
      return UNIJIT_STATUS_CANCELLED_V1;
    case unijit::StatusCode::kDeadlineExceeded:
      return UNIJIT_STATUS_DEADLINE_EXCEEDED_V1;
    case unijit::StatusCode::kUnavailable:
      return UNIJIT_STATUS_UNAVAILABLE_V1;
  }
  return UNIJIT_STATUS_INTERNAL_V1;
}

unijit_status_code_v1 publish_error(unijit_status_code_v1 code,
                                    std::string_view message,
                                    std::uint64_t detail,
                                    unijit_error_v1** output) noexcept {
  if (output != nullptr) {
    *output = nullptr;
    try {
      auto error = std::make_unique<unijit_error_v1>();
      error->code = code;
      error->detail = detail;
      const std::size_t count =
          std::min(message.size(), kMaximumDiagnosticBytes);
      error->message.assign(message.data(), count);
      *output = error.release();
    } catch (...) {
    }
  }
  return code;
}

unijit_status_code_v1 publish_status(const unijit::Status& status,
                                     unijit_error_v1** output) noexcept {
  if (status.ok()) {
    return UNIJIT_STATUS_OK_V1;
  }
  return publish_error(status_code(status.code()), status.message(),
                       static_cast<std::uint64_t>(status.location()), output);
}

unijit_status_code_v1 invalid(std::string_view message,
                              unijit_error_v1** output,
                              std::uint64_t detail = 0) noexcept {
  return publish_error(UNIJIT_STATUS_INVALID_ARGUMENT_V1, message, detail,
                       output);
}

template <typename Function>
unijit_status_code_v1 guarded(unijit_error_v1** output,
                              Function&& function) noexcept {
  if (output != nullptr) {
    if (*output != nullptr) {
      return UNIJIT_STATUS_INVALID_ARGUMENT_V1;
    }
    *output = nullptr;
  }
  try {
    return function();
  } catch (const std::bad_alloc&) {
    return publish_error(UNIJIT_STATUS_RESOURCE_EXHAUSTED_V1,
                         "allocation failed at the C ABI boundary", 0,
                         output);
  } catch (const std::exception&) {
    return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                         "unexpected exception blocked at the C ABI boundary",
                         0, output);
  } catch (...) {
    return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                         "unknown exception blocked at the C ABI boundary", 0,
                         output);
  }
}

bool to_size(std::uint64_t input, std::size_t* output) noexcept {
  if (input > static_cast<std::uint64_t>(
                  std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  *output = static_cast<std::size_t>(input);
  return true;
}

template <std::size_t Size>
bool reserved_zero(const std::uint64_t (&values)[Size]) noexcept {
  return std::all_of(std::begin(values), std::end(values),
                     [](std::uint64_t value) { return value == 0; });
}

bool value_type_from_c(unijit_value_type_v1 input,
                       unijit::ir::ValueType* output) noexcept {
  if (input == UNIJIT_VALUE_WORD_V1) {
    *output = unijit::ir::ValueType::kWord;
    return true;
  }
  if (input == UNIJIT_VALUE_FLOAT64_V1) {
    *output = unijit::ir::ValueType::kFloat64;
    return true;
  }
  return false;
}

unijit_value_type_v1 value_type_to_c(unijit::ir::ValueType input) noexcept {
  if (input == unijit::ir::ValueType::kWord) {
    return UNIJIT_VALUE_WORD_V1;
  }
  if (input == unijit::ir::ValueType::kFloat64) {
    return UNIJIT_VALUE_FLOAT64_V1;
  }
  return UINT32_MAX;
}

bool target_from_c(const unijit_target_profile_v1& input,
                   unijit::jit::TargetProfile* output) noexcept {
  if (input.struct_size < sizeof(input) ||
      !reserved_zero(input.reserved)) {
    return false;
  }
  switch (input.architecture) {
    case UNIJIT_TARGET_UNKNOWN_V1:
      output->architecture = unijit::jit::TargetArchitecture::kUnknown;
      break;
    case UNIJIT_TARGET_AARCH64_V1:
      output->architecture = unijit::jit::TargetArchitecture::kAArch64;
      break;
    case UNIJIT_TARGET_X86_64_V1:
      output->architecture = unijit::jit::TargetArchitecture::kX86_64;
      break;
    case UNIJIT_TARGET_RISCV64_V1:
      output->architecture = unijit::jit::TargetArchitecture::kRiscV64;
      break;
    default:
      return false;
  }
  switch (input.abi) {
    case UNIJIT_ABI_UNKNOWN_V1:
      output->abi = unijit::jit::TargetAbi::kUnknown;
      break;
    case UNIJIT_ABI_AAPCS64_V1:
      output->abi = unijit::jit::TargetAbi::kAapcs64;
      break;
    case UNIJIT_ABI_SYSTEM_V_V1:
      output->abi = unijit::jit::TargetAbi::kSystemV;
      break;
    case UNIJIT_ABI_WINDOWS_X64_V1:
      output->abi = unijit::jit::TargetAbi::kWindowsX64;
      break;
    case UNIJIT_ABI_RISCV_ELF_V1:
      output->abi = unijit::jit::TargetAbi::kRiscVElf;
      break;
    default:
      return false;
  }
  switch (input.endianness) {
    case UNIJIT_ENDIAN_UNKNOWN_V1:
      output->endianness = unijit::jit::TargetEndianness::kUnknown;
      break;
    case UNIJIT_ENDIAN_LITTLE_V1:
      output->endianness = unijit::jit::TargetEndianness::kLittle;
      break;
    case UNIJIT_ENDIAN_BIG_V1:
      output->endianness = unijit::jit::TargetEndianness::kBig;
      break;
    default:
      return false;
  }
  if (input.vector_width_policy == UNIJIT_VECTOR_WIDTH_PORTABLE_128_V1) {
    output->vector_width_policy =
        unijit::jit::VectorWidthPolicy::kPortable128;
  } else if (input.vector_width_policy == UNIJIT_VECTOR_WIDTH_NATIVE_V1) {
    output->vector_width_policy = unijit::jit::VectorWidthPolicy::kNative;
  } else {
    return false;
  }
  if (input.maximum_vector_bits > UINT16_MAX) {
    return false;
  }
  output->features = input.features;
  output->maximum_vector_bits =
      static_cast<std::uint16_t>(input.maximum_vector_bits);
  return true;
}

void target_to_c(const unijit::jit::TargetProfile& input,
                 unijit_target_profile_v1* output) noexcept {
  const std::uint32_t capacity = output->struct_size;
  std::memset(output, 0, sizeof(*output));
  output->struct_size = capacity;
  output->architecture =
      static_cast<unijit_target_architecture_v1>(input.architecture);
  output->abi = static_cast<unijit_target_abi_v1>(input.abi);
  output->endianness =
      static_cast<unijit_target_endianness_v1>(input.endianness);
  output->features = input.features;
  output->vector_width_policy =
      static_cast<unijit_vector_width_policy_v1>(input.vector_width_policy);
  output->maximum_vector_bits = input.maximum_vector_bits;
}

bool limits_from_c(const unijit_compilation_limits_v1& input,
                   unijit::jit::CompilationLimits* output) noexcept {
  if (input.struct_size < sizeof(input) || input.reserved0 != 0 ||
      !reserved_zero(input.reserved)) {
    return false;
  }
  const std::uint64_t values[] = {
      input.maximum_parameters,
      input.maximum_ir_nodes,
      input.maximum_cfg_blocks,
      input.maximum_ir_arguments,
      input.maximum_memory_regions,
      input.maximum_memory_accesses,
      input.maximum_atomic_accesses,
      input.maximum_vector_constants,
      input.maximum_vector_shuffles,
      input.maximum_vector_selects,
      input.maximum_frame_slots,
      input.maximum_trusted_objects,
      input.maximum_patch_cells,
      input.maximum_fast_calls,
      input.maximum_stack_maps,
      input.maximum_metadata_values,
      input.maximum_code_bytes};
  if (std::any_of(std::begin(values), std::end(values),
                  [](std::uint64_t value) {
                    return value == 0 ||
                           value > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::size_t>::max());
                  })) {
    return false;
  }
  std::size_t* destinations[] = {
      &output->maximum_parameters,
      &output->maximum_ir_nodes,
      &output->maximum_cfg_blocks,
      &output->maximum_ir_arguments,
      &output->maximum_memory_regions,
      &output->maximum_memory_accesses,
      &output->maximum_atomic_accesses,
      &output->maximum_vector_constants,
      &output->maximum_vector_shuffles,
      &output->maximum_vector_selects,
      &output->maximum_frame_slots,
      &output->maximum_trusted_objects,
      &output->maximum_patch_cells,
      &output->maximum_fast_calls,
      &output->maximum_stack_maps,
      &output->maximum_metadata_values,
      &output->maximum_code_bytes};
  for (std::size_t index = 0; index < std::size(values); ++index) {
    *destinations[index] = static_cast<std::size_t>(values[index]);
  }
  return true;
}

void limits_to_c(const unijit::jit::CompilationLimits& input,
                 unijit_compilation_limits_v1* output) noexcept {
  std::memset(output, 0, sizeof(*output));
  output->struct_size = sizeof(*output);
  output->maximum_parameters = input.maximum_parameters;
  output->maximum_ir_nodes = input.maximum_ir_nodes;
  output->maximum_cfg_blocks = input.maximum_cfg_blocks;
  output->maximum_ir_arguments = input.maximum_ir_arguments;
  output->maximum_memory_regions = input.maximum_memory_regions;
  output->maximum_memory_accesses = input.maximum_memory_accesses;
  output->maximum_atomic_accesses = input.maximum_atomic_accesses;
  output->maximum_vector_constants = input.maximum_vector_constants;
  output->maximum_vector_shuffles = input.maximum_vector_shuffles;
  output->maximum_vector_selects = input.maximum_vector_selects;
  output->maximum_frame_slots = input.maximum_frame_slots;
  output->maximum_trusted_objects = input.maximum_trusted_objects;
  output->maximum_patch_cells = input.maximum_patch_cells;
  output->maximum_fast_calls = input.maximum_fast_calls;
  output->maximum_stack_maps = input.maximum_stack_maps;
  output->maximum_metadata_values = input.maximum_metadata_values;
  output->maximum_code_bytes = input.maximum_code_bytes;
}

bool options_from_c(const unijit_compiler_options_v1& input,
                    unijit::jit::CompilationOptions* output) noexcept {
  if (input.struct_size < sizeof(input) || input.reserved0 != 0 ||
      !reserved_zero(input.reserved) || input.measure_safepoint_polls > 1 ||
      !limits_from_c(input.limits, &output->limits) ||
      !target_from_c(input.target_profile, &output->target_profile)) {
    return false;
  }
  if (input.optimization_level == UNIJIT_OPTIMIZATION_BASELINE_V1) {
    output->optimization_level = unijit::jit::OptimizationLevel::kBaseline;
  } else if (input.optimization_level == UNIJIT_OPTIMIZATION_OPTIMIZED_V1) {
    output->optimization_level = unijit::jit::OptimizationLevel::kOptimized;
  } else {
    return false;
  }
  output->measure_safepoint_polls = input.measure_safepoint_polls != 0;
  return true;
}

bool cache_limits_from_c(const unijit_code_cache_limits_v1& input,
                         unijit::jit::CodeCacheLimits* output) noexcept {
  if (input.struct_size < sizeof(input) || input.reserved0 != 0 ||
      !reserved_zero(input.reserved) || input.maximum_entries == 0 ||
      input.maximum_code_bytes == 0) {
    return false;
  }
  return to_size(input.maximum_entries, &output->maximum_entries) &&
         to_size(input.maximum_code_bytes, &output->maximum_code_bytes);
}

unijit_status_code_v1 require_builder(unijit_builder_v1* builder,
                                      unijit_error_v1**) noexcept {
  return builder != nullptr && builder->implementation != nullptr
             ? UNIJIT_STATUS_OK_V1
             : UNIJIT_STATUS_INVALID_ARGUMENT_V1;
}

unijit_status_code_v1 require_value(
    const unijit_builder_v1& builder, unijit_value_v1 value,
    unijit::ir::ValueType expected, unijit_error_v1** error) noexcept {
  if (value.id >= builder.value_types.size()) {
    return invalid("value does not belong to the active builder", error,
                   value.id);
  }
  if (builder.value_types[value.id] != expected) {
    return invalid("value type does not match the requested operation", error,
                   value.id);
  }
  return UNIJIT_STATUS_OK_V1;
}

unijit_status_code_v1 prepare_value(unijit_builder_v1* builder,
                                    unijit_error_v1** error) {
  if (builder->value_types.size() >= builder->maximum_nodes ||
      builder->value_types.size() >= unijit::ir::Value::kInvalidId) {
    return publish_error(UNIJIT_STATUS_RESOURCE_EXHAUSTED_V1,
                         "builder exceeds its IR node budget",
                         builder->value_types.size(), error);
  }
  if (builder->value_types.size() == builder->value_types.capacity()) {
    const std::size_t available =
        builder->maximum_nodes - builder->value_types.size();
    const std::size_t growth =
        std::min<std::size_t>(std::max<std::size_t>(available, 1), 64);
    builder->value_types.reserve(builder->value_types.size() + growth);
  }
  return UNIJIT_STATUS_OK_V1;
}

unijit_status_code_v1 finish_value(unijit_builder_v1* builder,
                                   unijit::ir::Value native,
                                   unijit::ir::ValueType type,
                                   unijit_value_v1* output,
                                   unijit_error_v1** error) noexcept {
  if (!native.valid() || native.id() != builder->value_types.size()) {
    return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                         "builder produced an inconsistent value token", 0,
                         error);
  }
  builder->value_types.push_back(type);
  if (output != nullptr) {
    output->id = native.id();
  }
  return UNIJIT_STATUS_OK_V1;
}

unijit::ir::PatchCellKind patch_kind_from_c(unijit_patch_cell_kind_v1 kind,
                                            bool* valid) noexcept {
  *valid = true;
  switch (kind) {
    case UNIJIT_PATCH_CELL_VALUE_V1:
      return unijit::ir::PatchCellKind::kValue;
    case UNIJIT_PATCH_CELL_TARGET_V1:
      return unijit::ir::PatchCellKind::kTarget;
    case UNIJIT_PATCH_CELL_SHAPE_V1:
      return unijit::ir::PatchCellKind::kShape;
    case UNIJIT_PATCH_CELL_GENERATION_V1:
      return unijit::ir::PatchCellKind::kGeneration;
    case UNIJIT_PATCH_CELL_COUNTER_V1:
      return unijit::ir::PatchCellKind::kCounter;
    default:
      *valid = false;
      return unijit::ir::PatchCellKind::kValue;
  }
}

std::string_view key_view(const char* key, std::size_t size) noexcept {
  return size == 0 ? std::string_view{} : std::string_view(key, size);
}

}  // namespace

extern "C" {

uint32_t unijit_v1_abi_version(void) { return UNIJIT_C_ABI_VERSION; }

unijit_status_code_v1 unijit_v1_abi_info(unijit_abi_info_v1* info,
                                         unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (info == nullptr || info->struct_size < sizeof(*info)) {
      return invalid("ABI info structure is null or too small", error);
    }
    const std::uint32_t capacity = info->struct_size;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = capacity;
    info->abi_version = UNIJIT_C_ABI_VERSION;
    info->word_bits = sizeof(unijit_word_v1) * 8U;
    info->pointer_bits = sizeof(void*) * 8U;
    const std::uint16_t marker = 1;
    info->little_endian =
        *reinterpret_cast<const std::uint8_t*>(&marker) == 1 ? 1U : 0U;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_error_code(const unijit_error_v1* error) {
  return error == nullptr ? UNIJIT_STATUS_OK_V1 : error->code;
}

uint64_t unijit_v1_error_detail(const unijit_error_v1* error) {
  return error == nullptr ? 0 : error->detail;
}

const char* unijit_v1_error_message(const unijit_error_v1* error) {
  return error == nullptr ? "" : error->message.c_str();
}

uint64_t unijit_v1_error_message_size(const unijit_error_v1* error) {
  return error == nullptr ? 0 : static_cast<std::uint64_t>(error->message.size());
}

void unijit_v1_error_destroy(unijit_error_v1* error) {
  try {
    delete error;
  } catch (...) {
  }
}

unijit_status_code_v1 unijit_v1_target_profile_baseline(
    unijit_target_profile_v1* profile, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (profile == nullptr || profile->struct_size < sizeof(*profile)) {
      return invalid("target profile structure is null or too small", error);
    }
    target_to_c(unijit::jit::baseline_target_profile(), profile);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_target_profile_host(
    unijit_target_profile_v1* profile, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (profile == nullptr || profile->struct_size < sizeof(*profile)) {
      return invalid("target profile structure is null or too small", error);
    }
    target_to_c(unijit::jit::host_target_profile(), profile);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_target_profile_validate(
    const unijit_target_profile_v1* profile, unijit_error_v1** error) {
  return guarded(error, [&]() {
    unijit::jit::TargetProfile native;
    if (profile == nullptr || !target_from_c(*profile, &native)) {
      return invalid("target profile structure is malformed", error);
    }
    return publish_status(unijit::jit::validate_target_profile(native), error);
  });
}

unijit_status_code_v1 unijit_v1_target_profile_key(
    const unijit_target_profile_v1* profile, uint64_t* key,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    unijit::jit::TargetProfile native;
    if (profile == nullptr || key == nullptr ||
        !target_from_c(*profile, &native)) {
      return invalid("target profile or key output is malformed", error);
    }
    const unijit::Status status = unijit::jit::validate_target_profile(native);
    if (!status.ok()) {
      return publish_status(status, error);
    }
    *key = unijit::jit::target_profile_key(native);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_compilation_limits_init(
    unijit_compilation_limits_v1* limits, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (limits == nullptr) {
      return invalid("compilation limits output is null", error);
    }
    limits_to_c(unijit::jit::CompilationLimits{}, limits);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_compiler_options_init(
    unijit_compiler_options_v1* options, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (options == nullptr) {
      return invalid("compiler options output is null", error);
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->optimization_level = UNIJIT_OPTIMIZATION_OPTIMIZED_V1;
    options->measure_safepoint_polls = 1;
    limits_to_c(unijit::jit::CompilationLimits{}, &options->limits);
    options->target_profile.struct_size = sizeof(options->target_profile);
    target_to_c(unijit::jit::baseline_target_profile(),
                &options->target_profile);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_code_cache_limits_init(
    unijit_code_cache_limits_v1* limits, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (limits == nullptr) {
      return invalid("code-cache limits output is null", error);
    }
    const unijit::jit::CodeCacheLimits defaults;
    std::memset(limits, 0, sizeof(*limits));
    limits->struct_size = sizeof(*limits);
    limits->maximum_entries = defaults.maximum_entries;
    limits->maximum_code_bytes = defaults.maximum_code_bytes;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_builder_create(
    const unijit_value_type_v1* parameter_types, uint64_t parameter_count,
    const unijit_compilation_limits_v1* limits, unijit_builder_v1** builder,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (builder == nullptr) {
      return invalid("builder output is null", error);
    }
    *builder = nullptr;
    unijit::jit::CompilationLimits native_limits;
    if (limits != nullptr && !limits_from_c(*limits, &native_limits)) {
      return invalid("compilation limits are malformed", error);
    }
    std::size_t count = 0;
    if (!to_size(parameter_count, &count) ||
        count > native_limits.maximum_parameters ||
        count >= unijit::ir::Value::kInvalidId ||
        (count != 0 && parameter_types == nullptr)) {
      return invalid("builder parameter list exceeds its validated bounds",
                     error, parameter_count);
    }
    std::vector<unijit::ir::ValueType> native_types;
    native_types.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      unijit::ir::ValueType type;
      if (!value_type_from_c(parameter_types[index], &type)) {
        return invalid("builder parameter type is unsupported", error, index);
      }
      native_types.push_back(type);
    }
    auto result = std::make_unique<unijit_builder_v1>();
    result->value_types = native_types;
    const std::size_t initial_capacity =
        std::min(native_limits.maximum_ir_nodes, count + 64U);
    result->value_types.reserve(initial_capacity);
    result->implementation =
        std::make_unique<unijit::ir::FunctionBuilder>(std::move(native_types));
    result->maximum_nodes = native_limits.maximum_ir_nodes;
    result->maximum_ir_arguments = native_limits.maximum_ir_arguments;
    result->maximum_fast_calls = native_limits.maximum_fast_calls;
    result->maximum_patch_cells = native_limits.maximum_patch_cells;
    *builder = result.release();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_builder_destroy(unijit_builder_v1* builder) {
  try {
    delete builder;
  } catch (...) {
  }
}

unijit_status_code_v1 unijit_v1_builder_parameter(
    unijit_builder_v1* builder, uint64_t index, unijit_value_v1* value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr || index >= builder->value_types.size()) {
      return invalid("builder parameter index or output is invalid", error,
                     index);
    }
    const unijit::ir::Value native =
        builder->implementation->parameter(static_cast<std::size_t>(index));
    if (!native.valid()) {
      return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                           "builder failed to resolve a validated parameter",
                           index, error);
    }
    value->id = native.id();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_builder_word_constant(
    unijit_builder_v1* builder, unijit_word_v1 bits, unijit_value_v1* value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr) {
      return invalid("builder or value output is null", error);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    return finish_value(builder, builder->implementation->constant(bits),
                        unijit::ir::ValueType::kWord, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_float64_constant_bits(
    unijit_builder_v1* builder, unijit_word_v1 bits, unijit_value_v1* value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr) {
      return invalid("builder or value output is null", error);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    return finish_value(
        builder, builder->implementation->float64_constant_bits(bits),
        unijit::ir::ValueType::kFloat64, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_binary(
    unijit_builder_v1* builder, unijit_binary_operation_v1 operation,
    unijit_value_v1 lhs, unijit_value_v1 rhs, unijit_value_v1* value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr) {
      return invalid("builder or value output is null", error);
    }
    const bool float_operation = operation >= UNIJIT_BINARY_FLOAT64_ADD_V1 &&
                                 operation <= UNIJIT_BINARY_FLOAT64_NOT_EQUAL_V1;
    const unijit::ir::ValueType input_type =
        float_operation ? unijit::ir::ValueType::kFloat64
                        : unijit::ir::ValueType::kWord;
    auto validation = require_value(*builder, lhs, input_type, error);
    if (validation != UNIJIT_STATUS_OK_V1) {
      return validation;
    }
    validation = require_value(*builder, rhs, input_type, error);
    if (validation != UNIJIT_STATUS_OK_V1) {
      return validation;
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    const unijit::ir::Value native_lhs{lhs.id};
    const unijit::ir::Value native_rhs{rhs.id};
    unijit::ir::Value native;
    unijit::ir::ValueType result_type = input_type;
    switch (operation) {
      case UNIJIT_BINARY_WORD_ADD_V1:
        native = builder->implementation->add(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_SUBTRACT_V1:
        native = builder->implementation->subtract(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_MULTIPLY_V1:
        native = builder->implementation->multiply(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_AND_V1:
        native = builder->implementation->bitwise_and(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_OR_V1:
        native = builder->implementation->bitwise_or(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_XOR_V1:
        native = builder->implementation->bitwise_xor(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_SHIFT_V1:
        native = builder->implementation->shift_left(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_FLOOR_DIVIDE_V1:
        native = builder->implementation->floor_divide(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_FLOOR_MODULO_V1:
        native = builder->implementation->floor_modulo(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_LESS_THAN_V1:
        native = builder->implementation->less_than(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_LESS_EQUAL_V1:
        native = builder->implementation->less_equal(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_EQUAL_V1:
        native = builder->implementation->equal(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_WORD_NOT_EQUAL_V1:
        native = builder->implementation->not_equal(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_FLOAT64_ADD_V1:
        native = builder->implementation->float64_add(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_FLOAT64_SUBTRACT_V1:
        native =
            builder->implementation->float64_subtract(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_FLOAT64_MULTIPLY_V1:
        native =
            builder->implementation->float64_multiply(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_FLOAT64_DIVIDE_V1:
        native =
            builder->implementation->float64_divide(native_lhs, native_rhs);
        break;
      case UNIJIT_BINARY_FLOAT64_LESS_THAN_V1:
        native = builder->implementation->float64_less_than(native_lhs,
                                                             native_rhs);
        result_type = unijit::ir::ValueType::kWord;
        break;
      case UNIJIT_BINARY_FLOAT64_LESS_EQUAL_V1:
        native = builder->implementation->float64_less_equal(native_lhs,
                                                              native_rhs);
        result_type = unijit::ir::ValueType::kWord;
        break;
      case UNIJIT_BINARY_FLOAT64_EQUAL_V1:
        native =
            builder->implementation->float64_equal(native_lhs, native_rhs);
        result_type = unijit::ir::ValueType::kWord;
        break;
      case UNIJIT_BINARY_FLOAT64_NOT_EQUAL_V1:
        native = builder->implementation->float64_not_equal(native_lhs,
                                                             native_rhs);
        result_type = unijit::ir::ValueType::kWord;
        break;
      default:
        return invalid("binary operation is unsupported", error, operation);
    }
    return finish_value(builder, native, result_type, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_unary(
    unijit_builder_v1* builder, unijit_unary_operation_v1 operation,
    unijit_value_v1 input, unijit_value_v1* value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr) {
      return invalid("builder or value output is null", error);
    }
    const unijit::ir::ValueType type =
        operation == UNIJIT_UNARY_FLOAT64_NEGATE_V1
            ? unijit::ir::ValueType::kFloat64
            : unijit::ir::ValueType::kWord;
    const auto validation = require_value(*builder, input, type, error);
    if (validation != UNIJIT_STATUS_OK_V1) {
      return validation;
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    unijit::ir::Value native;
    if (operation == UNIJIT_UNARY_WORD_NEGATE_V1) {
      native = builder->implementation->negate(unijit::ir::Value{input.id});
    } else if (operation == UNIJIT_UNARY_WORD_NOT_V1) {
      native =
          builder->implementation->bitwise_not(unijit::ir::Value{input.id});
    } else if (operation == UNIJIT_UNARY_FLOAT64_NEGATE_V1) {
      native =
          builder->implementation->float64_negate(unijit::ir::Value{input.id});
    } else {
      return invalid("unary operation is unsupported", error, operation);
    }
    return finish_value(builder, native, type, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_guard_nonzero(
    unijit_builder_v1* builder, unijit_value_v1 input, uint64_t site,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        input.id >= builder->value_types.size() ||
        site > static_cast<std::uint64_t>(
                   std::numeric_limits<unijit::ir::Word>::max())) {
      return invalid("guard input or site is invalid", error, site);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    unijit::ir::Value native;
    if (builder->value_types[input.id] == unijit::ir::ValueType::kWord) {
      native = builder->implementation->guard_word_nonzero(
          unijit::ir::Value{input.id}, static_cast<std::size_t>(site));
    } else if (builder->value_types[input.id] ==
               unijit::ir::ValueType::kFloat64) {
      native = builder->implementation->guard_float64_nonzero(
          unijit::ir::Value{input.id}, static_cast<std::size_t>(site));
    } else {
      return invalid("guard input type is unsupported", error, input.id);
    }
    return finish_value(builder, native, unijit::ir::ValueType::kWord,
                        nullptr, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_safepoint(
    unijit_builder_v1* builder, uint64_t site, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        site > static_cast<std::uint64_t>(
                   std::numeric_limits<unijit::ir::Word>::max())) {
      return invalid("safepoint site is invalid", error, site);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    return finish_value(
        builder,
        builder->implementation->safepoint(static_cast<std::size_t>(site)),
        unijit::ir::ValueType::kWord, nullptr, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_declare_fast_call(
    unijit_builder_v1* builder,
    const unijit_value_type_v1* parameter_types, uint64_t parameter_count,
    unijit_value_type_v1 return_type, unijit_fast_call_slot_v1* slot,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        slot == nullptr || builder->fast_calls.size() >=
                               builder->maximum_fast_calls) {
      return invalid("fast-call declaration or output is invalid", error,
                     builder == nullptr ? 0 : builder->fast_calls.size());
    }
    std::size_t count = 0;
    if (!to_size(parameter_count, &count) ||
        count > builder->maximum_ir_arguments ||
        (count != 0 && parameter_types == nullptr)) {
      return invalid("fast-call parameter list exceeds its bounds", error,
                     parameter_count);
    }
    unijit::ir::ValueType native_return;
    if (!value_type_from_c(return_type, &native_return)) {
      return invalid("fast-call return type is unsupported", error,
                     return_type);
    }
    std::vector<unijit::ir::ValueType> native_parameters;
    native_parameters.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      unijit::ir::ValueType type;
      if (!value_type_from_c(parameter_types[index], &type)) {
        return invalid("fast-call parameter type is unsupported", error,
                       index);
      }
      native_parameters.push_back(type);
    }
    if (builder->fast_calls.size() == builder->fast_calls.capacity()) {
      builder->fast_calls.reserve(builder->fast_calls.size() + 1);
    }
    unijit::ir::FastCallDescriptor descriptor{native_parameters,
                                               native_return};
    unijit::ir::FastCallSlot native;
    try {
      native = builder->implementation->create_fast_call(
          std::move(native_parameters), native_return);
    } catch (...) {
      builder->implementation.reset();
      throw;
    }
    builder->fast_calls.push_back(std::move(descriptor));
    if (!native.valid() || native.id() + 1 != builder->fast_calls.size()) {
      builder->fast_calls.pop_back();
      return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                           "builder produced an inconsistent fast-call slot",
                           0, error);
    }
    slot->id = native.id();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_builder_fast_call(
    unijit_builder_v1* builder, unijit_fast_call_slot_v1 slot,
    const unijit_value_v1* arguments, uint64_t argument_count,
    unijit_value_v1* value, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr || slot.id >= builder->fast_calls.size()) {
      return invalid("fast-call slot or output is invalid", error, slot.id);
    }
    std::size_t count = 0;
    const auto& descriptor = builder->fast_calls[slot.id];
    if (!to_size(argument_count, &count) ||
        count != descriptor.parameter_types.size() ||
        count > builder->maximum_ir_arguments - builder->call_arguments ||
        (count != 0 && arguments == nullptr)) {
      return invalid("fast-call argument list is invalid", error,
                     argument_count);
    }
    std::vector<unijit::ir::Value> native_arguments;
    native_arguments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto validation = require_value(
          *builder, arguments[index], descriptor.parameter_types[index], error);
      if (validation != UNIJIT_STATUS_OK_V1) {
        return validation;
      }
      native_arguments.emplace_back(arguments[index].id);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    unijit::ir::Value native;
    try {
      native = builder->implementation->fast_call(
          unijit::ir::FastCallSlot{slot.id}, std::move(native_arguments));
    } catch (...) {
      builder->implementation.reset();
      throw;
    }
    builder->call_arguments += count;
    return finish_value(builder, native, descriptor.return_type, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_declare_patch_cell(
    unijit_builder_v1* builder, unijit_word_v1 initial_value,
    unijit_patch_cell_kind_v1 kind, unijit_patch_cell_slot_v1* slot,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        slot == nullptr || builder->patch_cells >= builder->maximum_patch_cells) {
      return invalid("patch-cell declaration or output is invalid", error,
                     builder == nullptr ? 0 : builder->patch_cells);
    }
    bool valid = false;
    const auto native_kind = patch_kind_from_c(kind, &valid);
    if (!valid) {
      return invalid("patch-cell kind is unsupported", error, kind);
    }
    const unijit::ir::PatchCellSlot native =
        builder->implementation->create_patch_cell(initial_value, native_kind);
    if (!native.valid() || native.id() != builder->patch_cells) {
      return publish_error(UNIJIT_STATUS_INTERNAL_V1,
                           "builder produced an inconsistent patch-cell slot",
                           0, error);
    }
    ++builder->patch_cells;
    slot->id = native.id();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_builder_load_patch_cell(
    unijit_builder_v1* builder, unijit_patch_cell_slot_v1 slot,
    unijit_value_v1* value, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value == nullptr || slot.id >= builder->patch_cells) {
      return invalid("patch-cell slot or output is invalid", error, slot.id);
    }
    const auto prepared = prepare_value(builder, error);
    if (prepared != UNIJIT_STATUS_OK_V1) {
      return prepared;
    }
    return finish_value(
        builder,
        builder->implementation->load_patch_cell(
            unijit::ir::PatchCellSlot{slot.id}),
        unijit::ir::ValueType::kWord, value, error);
  });
}

unijit_status_code_v1 unijit_v1_builder_set_return(
    unijit_builder_v1* builder, unijit_value_v1 value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        value.id >= builder->value_types.size()) {
      return invalid("return value does not belong to the active builder",
                     error, value.id);
    }
    return publish_status(
        builder->implementation->set_return(unijit::ir::Value{value.id}),
        error);
  });
}

unijit_status_code_v1 unijit_v1_builder_finish(
    unijit_builder_v1* builder, unijit_function_v1** function,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (require_builder(builder, error) != UNIJIT_STATUS_OK_V1 ||
        function == nullptr) {
      return invalid("builder or function output is null", error);
    }
    *function = nullptr;
    unijit::ir::Function native =
        std::move(*builder->implementation).build();
    builder->implementation.reset();
    const unijit::Status status = unijit::ir::verify(native);
    if (!status.ok()) {
      return publish_status(status, error);
    }
    *function = new unijit_function_v1(std::move(native));
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_function_destroy(unijit_function_v1* function) {
  try {
    delete function;
  } catch (...) {
  }
}

uint64_t unijit_v1_function_parameter_count(
    const unijit_function_v1* function) {
  return function == nullptr
             ? 0
             : static_cast<std::uint64_t>(
                   function->implementation.parameter_count());
}

unijit_value_type_v1 unijit_v1_function_parameter_type(
    const unijit_function_v1* function, uint64_t index) {
  if (function == nullptr ||
      index >= function->implementation.parameter_count()) {
    return UINT32_MAX;
  }
  return value_type_to_c(function->implementation.parameter_type(
      static_cast<std::size_t>(index)));
}

unijit_value_type_v1 unijit_v1_function_return_type(
    const unijit_function_v1* function) {
  return function == nullptr
             ? UINT32_MAX
             : value_type_to_c(function->implementation.return_type());
}

unijit_status_code_v1 unijit_v1_compiler_create(
    const unijit_compiler_options_v1* options, unijit_compiler_v1** compiler,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (compiler == nullptr) {
      return invalid("compiler output is null", error);
    }
    *compiler = nullptr;
    unijit::jit::CompilationOptions native_options;
    if (options != nullptr && !options_from_c(*options, &native_options)) {
      return invalid("compiler options are malformed", error);
    }
    const unijit::Status target_status =
        unijit::jit::validate_target_profile(native_options.target_profile);
    if (!target_status.ok()) {
      return publish_status(target_status, error);
    }
    auto result = std::make_unique<unijit_compiler_v1>();
    result->options = native_options;
    *compiler = result.release();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_compiler_destroy(unijit_compiler_v1* compiler) {
  try {
    delete compiler;
  } catch (...) {
  }
}

unijit_status_code_v1 unijit_v1_compiler_compile(
    const unijit_compiler_v1* compiler, const unijit_function_v1* function,
    unijit_compiled_function_v1** compiled, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (compiler == nullptr || function == nullptr || compiled == nullptr) {
      return invalid("compiler, function, or compiled output is null", error);
    }
    *compiled = nullptr;
    auto result = unijit::jit::Compiler::compile(function->implementation,
                                                  compiler->options);
    if (!result.ok()) {
      return publish_status(result.status, error);
    }
    *compiled = new unijit_compiled_function_v1(
        std::move(result.function));
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_compiled_function_destroy(
    unijit_compiled_function_v1* function) {
  try {
    delete function;
  } catch (...) {
  }
}

uint64_t unijit_v1_compiled_function_parameter_count(
    const unijit_compiled_function_v1* function) {
  return function == nullptr || function->implementation == nullptr
             ? 0
             : static_cast<std::uint64_t>(
                   function->implementation->parameter_count());
}

unijit_value_type_v1 unijit_v1_compiled_function_parameter_type(
    const unijit_compiled_function_v1* function, uint64_t index) {
  if (function == nullptr || function->implementation == nullptr ||
      index >= function->implementation->parameter_count()) {
    return UINT32_MAX;
  }
  return value_type_to_c(function->implementation->parameter_type(
      static_cast<std::size_t>(index)));
}

unijit_value_type_v1 unijit_v1_compiled_function_return_type(
    const unijit_compiled_function_v1* function) {
  return function == nullptr || function->implementation == nullptr
             ? UINT32_MAX
             : value_type_to_c(function->implementation->return_type());
}

uint32_t unijit_v1_compiled_function_requires_context(
    const unijit_compiled_function_v1* function) {
  return function != nullptr && function->implementation != nullptr &&
                 function->implementation->requires_context()
             ? 1U
             : 0U;
}

unijit_status_code_v1 unijit_v1_compiled_function_stats(
    const unijit_compiled_function_v1* function,
    unijit_compilation_stats_v1* stats, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (function == nullptr || function->implementation == nullptr ||
        stats == nullptr || stats->struct_size < sizeof(*stats)) {
      return invalid("compiled function or stats output is invalid", error);
    }
    const auto& native = function->implementation->stats();
    const std::uint32_t capacity = stats->struct_size;
    std::memset(stats, 0, sizeof(*stats));
    stats->struct_size = capacity;
    stats->code_size = native.code_size;
    stats->executable_mapping_size = native.executable_mapping_size;
    stats->spill_slots = native.spill_slots;
    stats->frame_slots = native.frame_slots;
    stats->trusted_objects = native.trusted_objects;
    stats->patch_cells = native.patch_cells;
    stats->fast_calls = native.fast_calls;
    stats->input_ir_nodes = native.input_ir_nodes;
    stats->optimized_ir_nodes = native.optimized_ir_nodes;
    stats->stack_map_count = native.stack_map_count;
    stats->stack_map_value_count = native.stack_map_value_count;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_compiled_function_invoke(
    const unijit_compiled_function_v1* function, const unijit_word_v1* args,
    uint64_t arg_count, unijit_execution_context_v1* context,
    unijit_word_v1* result, unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t count = 0;
    if (function == nullptr || function->implementation == nullptr ||
        result == nullptr || !to_size(arg_count, &count) ||
        (count != 0 && args == nullptr)) {
      return invalid("compiled invocation arguments are invalid", error,
                     arg_count);
    }
    auto evaluation = function->implementation->invoke(
        args, count, context == nullptr ? nullptr : &context->implementation);
    if (!evaluation.ok()) {
      return publish_status(evaluation.status, error);
    }
    *result = evaluation.value;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_execution_context_create(
    unijit_execution_context_v1** context, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (context == nullptr) {
      return invalid("execution-context output is null", error);
    }
    *context = nullptr;
    *context = new unijit_execution_context_v1();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_execution_context_destroy(
    unijit_execution_context_v1* context) {
  try {
    delete context;
  } catch (...) {
  }
}

void unijit_v1_execution_context_request_interrupt(
    unijit_execution_context_v1* context) {
  if (context != nullptr) {
    context->implementation.request_interrupt();
  }
}

void unijit_v1_execution_context_clear_interrupt(
    unijit_execution_context_v1* context) {
  if (context != nullptr) {
    context->implementation.clear_interrupt();
  }
}

uint32_t unijit_v1_execution_context_interrupt_requested(
    const unijit_execution_context_v1* context) {
  return context != nullptr && context->implementation.interrupt_requested()
             ? 1U
             : 0U;
}

uint64_t unijit_v1_execution_context_exit_reason(
    const unijit_execution_context_v1* context) {
  return context == nullptr
             ? 0
             : static_cast<std::uint64_t>(context->implementation.exit_reason());
}

uint64_t unijit_v1_execution_context_exit_site(
    const unijit_execution_context_v1* context) {
  return context == nullptr
             ? 0
             : static_cast<std::uint64_t>(context->implementation.exit_site());
}

unijit_word_v1 unijit_v1_execution_context_exit_value(
    const unijit_execution_context_v1* context) {
  return context == nullptr ? 0 : context->implementation.exit_value();
}

uint64_t unijit_v1_execution_context_safepoint_polls(
    const unijit_execution_context_v1* context) {
  return context == nullptr ? 0 : context->implementation.safepoint_polls();
}

unijit_status_code_v1 unijit_v1_code_cache_create(
    const unijit_code_cache_limits_v1* limits,
    const unijit_target_profile_v1* target_profile,
    unijit_code_cache_v1** cache, unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (cache == nullptr) {
      return invalid("code-cache output is null", error);
    }
    *cache = nullptr;
    unijit::jit::CodeCacheLimits native_limits;
    if (limits != nullptr && !cache_limits_from_c(*limits, &native_limits)) {
      return invalid("code-cache limits are malformed", error);
    }
    unijit::jit::TargetProfile native_target =
        unijit::jit::baseline_target_profile();
    if (target_profile != nullptr &&
        !target_from_c(*target_profile, &native_target)) {
      return invalid("code-cache target profile is malformed", error);
    }
    const unijit::Status target_status =
        unijit::jit::validate_target_profile(native_target);
    if (!target_status.ok()) {
      return publish_status(target_status, error);
    }
    *cache = new unijit_code_cache_v1(native_limits, native_target);
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_code_cache_destroy(unijit_code_cache_v1* cache) {
  try {
    delete cache;
  } catch (...) {
  }
}

unijit_status_code_v1 unijit_v1_code_cache_publish(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, unijit_compiled_function_v1** compiled,
    unijit_code_handle_v1** handle, uint32_t* cached, uint32_t* reused,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t size = 0;
    if (cache == nullptr || compiled == nullptr || *compiled == nullptr ||
        (*compiled)->implementation == nullptr || handle == nullptr ||
        cached == nullptr || reused == nullptr || !to_size(key_size, &size) ||
        (size != 0 && key == nullptr)) {
      return invalid("code-cache publication arguments are invalid", error,
                     key_size);
    }
    *handle = nullptr;
    *cached = 0;
    *reused = 0;
    std::unique_ptr<unijit_compiled_function_v1> owner(*compiled);
    *compiled = nullptr;
    auto publication = cache->implementation.publish(
        key_view(key, size), fingerprint, std::move(owner->implementation));
    if (!publication.ok()) {
      return publish_status(publication.status, error);
    }
    auto output =
        std::make_unique<unijit_code_handle_v1>(std::move(publication.handle));
    *cached = publication.cached ? 1U : 0U;
    *reused = publication.reused ? 1U : 0U;
    *handle = output.release();
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_code_cache_find(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, unijit_code_handle_v1** handle,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t size = 0;
    if (cache == nullptr || handle == nullptr || !to_size(key_size, &size) ||
        (size != 0 && key == nullptr)) {
      return invalid("code-cache lookup arguments are invalid", error,
                     key_size);
    }
    *handle = nullptr;
    auto native = cache->implementation.find(key_view(key, size), fingerprint);
    if (!native.valid()) {
      return publish_error(UNIJIT_STATUS_UNAVAILABLE_V1,
                           "code-cache entry was not found", fingerprint,
                           error);
    }
    *handle = new unijit_code_handle_v1(std::move(native));
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_code_cache_invalidate(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, uint32_t* invalidated, unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t size = 0;
    if (cache == nullptr || invalidated == nullptr ||
        !to_size(key_size, &size) || (size != 0 && key == nullptr)) {
      return invalid("code-cache invalidation arguments are invalid", error,
                     key_size);
    }
    *invalidated =
        cache->implementation.invalidate(key_view(key, size), fingerprint)
            ? 1U
            : 0U;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_code_cache_clear(unijit_code_cache_v1* cache) {
  if (cache != nullptr) {
    try {
      cache->implementation.clear();
    } catch (...) {
    }
  }
}

unijit_status_code_v1 unijit_v1_code_cache_stats(
    const unijit_code_cache_v1* cache, unijit_code_cache_stats_v1* stats,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    if (cache == nullptr || stats == nullptr ||
        stats->struct_size < sizeof(*stats)) {
      return invalid("code-cache or stats output is invalid", error);
    }
    const auto native = cache->implementation.stats();
    const std::uint32_t capacity = stats->struct_size;
    std::memset(stats, 0, sizeof(*stats));
    stats->struct_size = capacity;
    stats->lookups = native.lookups;
    stats->hits = native.hits;
    stats->misses = native.misses;
    stats->publications = native.publications;
    stats->publication_reuses = native.publication_reuses;
    stats->uncached_publications = native.uncached_publications;
    stats->replacements = native.replacements;
    stats->invalidations = native.invalidations;
    stats->assumption_invalidations = native.assumption_invalidations;
    stats->evictions = native.evictions;
    stats->clears = native.clears;
    stats->resident_entries = native.resident_entries;
    stats->resident_code_bytes = native.resident_code_bytes;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

void unijit_v1_code_handle_destroy(unijit_code_handle_v1* handle) {
  try {
    delete handle;
  } catch (...) {
  }
}

uint64_t unijit_v1_code_handle_generation(
    const unijit_code_handle_v1* handle) {
  return handle == nullptr ? 0 : handle->implementation.generation();
}

unijit_status_code_v1 unijit_v1_code_handle_invoke(
    const unijit_code_handle_v1* handle, const unijit_word_v1* args,
    uint64_t arg_count, unijit_execution_context_v1* context,
    unijit_word_v1* result, unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t count = 0;
    if (handle == nullptr || !handle->implementation.valid() ||
        result == nullptr || !to_size(arg_count, &count) ||
        (count != 0 && args == nullptr)) {
      return invalid("code-handle invocation arguments are invalid", error,
                     arg_count);
    }
    auto evaluation = handle->implementation.invoke(
        args, count, context == nullptr ? nullptr : &context->implementation);
    if (!evaluation.ok()) {
      return publish_status(evaluation.status, error);
    }
    *result = evaluation.value;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_code_handle_bind_fast_call(
    const unijit_code_handle_v1* caller, uint64_t index,
    const unijit_code_handle_v1* target, unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t native_index = 0;
    if (caller == nullptr || target == nullptr ||
        !caller->implementation.valid() || !target->implementation.valid() ||
        !to_size(index, &native_index)) {
      return invalid("fast-call binding arguments are invalid", error, index);
    }
    return publish_status(caller->implementation.bind_fast_call(
                              native_index, target->implementation),
                          error);
  });
}

unijit_status_code_v1 unijit_v1_code_handle_clear_fast_call(
    const unijit_code_handle_v1* caller, uint64_t index,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t native_index = 0;
    if (caller == nullptr || !caller->implementation.valid() ||
        !to_size(index, &native_index)) {
      return invalid("fast-call clearing arguments are invalid", error, index);
    }
    return publish_status(caller->implementation.clear_fast_call(native_index),
                          error);
  });
}

uint32_t unijit_v1_code_handle_fast_call_bound(
    const unijit_code_handle_v1* caller, uint64_t index) {
  std::size_t native_index = 0;
  return caller != nullptr && caller->implementation.valid() &&
                 to_size(index, &native_index) &&
                 caller->implementation.fast_call_bound(native_index)
             ? 1U
             : 0U;
}

unijit_status_code_v1 unijit_v1_code_handle_read_patch_cell(
    const unijit_code_handle_v1* handle, uint64_t index,
    unijit_word_v1* value, unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t native_index = 0;
    if (handle == nullptr || !handle->implementation.valid() ||
        value == nullptr || !to_size(index, &native_index)) {
      return invalid("patch-cell read arguments are invalid", error, index);
    }
    const auto result = handle->implementation.read_patch_cell(native_index);
    if (!result.ok()) {
      return publish_status(result.status, error);
    }
    *value = result.value;
    return static_cast<unijit_status_code_v1>(UNIJIT_STATUS_OK_V1);
  });
}

unijit_status_code_v1 unijit_v1_code_handle_publish_patch_cell(
    const unijit_code_handle_v1* handle, uint64_t index, unijit_word_v1 value,
    unijit_error_v1** error) {
  return guarded(error, [&]() {
    std::size_t native_index = 0;
    if (handle == nullptr || !handle->implementation.valid() ||
        !to_size(index, &native_index)) {
      return invalid("patch-cell publication arguments are invalid", error,
                     index);
    }
    return publish_status(
        handle->implementation.publish_patch_cell(native_index, value), error);
  });
}

}  // extern "C"
