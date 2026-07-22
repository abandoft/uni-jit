#ifndef UNIJIT_EMBEDDING_H
#define UNIJIT_EMBEDDING_H

/*
 * Stable C17 embedding boundary for 64-bit UniJIT hosts.
 *
 * Every owning object is opaque and must be released by its matching destroy
 * function. An error output may be NULL; otherwise it must point to NULL on
 * entry and the returned error, if any, is owned by the caller. No exception,
 * C++ layout, process-local callback, or executable address crosses this ABI.
 * See doc/EMBEDDING_C_API.md for lifecycle and concurrency rules.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(UNIJIT_BUILDING_LIBRARY)
#define UNIJIT_C_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define UNIJIT_C_API __attribute__((visibility("default")))
#else
#define UNIJIT_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UNIJIT_C_ABI_VERSION_MAJOR 1U
#define UNIJIT_C_ABI_VERSION_MINOR 0U
#define UNIJIT_C_ABI_VERSION \
  ((UNIJIT_C_ABI_VERSION_MAJOR << 16U) | UNIJIT_C_ABI_VERSION_MINOR)
#define UNIJIT_V1_INVALID_VALUE_ID UINT32_MAX

typedef int64_t unijit_word_v1;
typedef uint32_t unijit_status_code_v1;
typedef uint32_t unijit_value_type_v1;
typedef uint32_t unijit_binary_operation_v1;
typedef uint32_t unijit_unary_operation_v1;
typedef uint32_t unijit_optimization_level_v1;
typedef uint32_t unijit_target_architecture_v1;
typedef uint32_t unijit_target_abi_v1;
typedef uint32_t unijit_target_endianness_v1;
typedef uint32_t unijit_vector_width_policy_v1;
typedef uint32_t unijit_patch_cell_kind_v1;

enum {
  UNIJIT_STATUS_OK_V1 = 0,
  UNIJIT_STATUS_INVALID_ARGUMENT_V1 = 1,
  UNIJIT_STATUS_INVALID_IR_V1 = 2,
  UNIJIT_STATUS_UNSUPPORTED_ARCHITECTURE_V1 = 3,
  UNIJIT_STATUS_RESOURCE_EXHAUSTED_V1 = 4,
  UNIJIT_STATUS_CODE_GENERATION_FAILED_V1 = 5,
  UNIJIT_STATUS_MEMORY_PROTECTION_FAILED_V1 = 6,
  UNIJIT_STATUS_EXECUTION_INTERRUPTED_V1 = 7,
  UNIJIT_STATUS_RUNTIME_EXIT_V1 = 8,
  UNIJIT_STATUS_CANCELLED_V1 = 9,
  UNIJIT_STATUS_DEADLINE_EXCEEDED_V1 = 10,
  UNIJIT_STATUS_UNAVAILABLE_V1 = 11,
  UNIJIT_STATUS_INTERNAL_V1 = 12
};

enum {
  UNIJIT_VALUE_WORD_V1 = 0,
  UNIJIT_VALUE_FLOAT64_V1 = 1
};

enum {
  UNIJIT_BINARY_WORD_ADD_V1 = 0,
  UNIJIT_BINARY_WORD_SUBTRACT_V1 = 1,
  UNIJIT_BINARY_WORD_MULTIPLY_V1 = 2,
  UNIJIT_BINARY_WORD_AND_V1 = 3,
  UNIJIT_BINARY_WORD_OR_V1 = 4,
  UNIJIT_BINARY_WORD_XOR_V1 = 5,
  UNIJIT_BINARY_WORD_SHIFT_V1 = 6,
  UNIJIT_BINARY_WORD_FLOOR_DIVIDE_V1 = 7,
  UNIJIT_BINARY_WORD_FLOOR_MODULO_V1 = 8,
  UNIJIT_BINARY_WORD_LESS_THAN_V1 = 9,
  UNIJIT_BINARY_WORD_LESS_EQUAL_V1 = 10,
  UNIJIT_BINARY_WORD_EQUAL_V1 = 11,
  UNIJIT_BINARY_WORD_NOT_EQUAL_V1 = 12,
  UNIJIT_BINARY_FLOAT64_ADD_V1 = 16,
  UNIJIT_BINARY_FLOAT64_SUBTRACT_V1 = 17,
  UNIJIT_BINARY_FLOAT64_MULTIPLY_V1 = 18,
  UNIJIT_BINARY_FLOAT64_DIVIDE_V1 = 19,
  UNIJIT_BINARY_FLOAT64_LESS_THAN_V1 = 20,
  UNIJIT_BINARY_FLOAT64_LESS_EQUAL_V1 = 21,
  UNIJIT_BINARY_FLOAT64_EQUAL_V1 = 22,
  UNIJIT_BINARY_FLOAT64_NOT_EQUAL_V1 = 23
};

enum {
  UNIJIT_UNARY_WORD_NEGATE_V1 = 0,
  UNIJIT_UNARY_WORD_NOT_V1 = 1,
  UNIJIT_UNARY_FLOAT64_NEGATE_V1 = 2
};

enum {
  UNIJIT_OPTIMIZATION_BASELINE_V1 = 0,
  UNIJIT_OPTIMIZATION_OPTIMIZED_V1 = 1
};

enum {
  UNIJIT_TARGET_UNKNOWN_V1 = 0,
  UNIJIT_TARGET_AARCH64_V1 = 1,
  UNIJIT_TARGET_X86_64_V1 = 2,
  UNIJIT_TARGET_RISCV64_V1 = 3
};

enum {
  UNIJIT_ABI_UNKNOWN_V1 = 0,
  UNIJIT_ABI_AAPCS64_V1 = 1,
  UNIJIT_ABI_SYSTEM_V_V1 = 2,
  UNIJIT_ABI_WINDOWS_X64_V1 = 3,
  UNIJIT_ABI_RISCV_ELF_V1 = 4
};

enum {
  UNIJIT_ENDIAN_UNKNOWN_V1 = 0,
  UNIJIT_ENDIAN_LITTLE_V1 = 1,
  UNIJIT_ENDIAN_BIG_V1 = 2
};

enum {
  UNIJIT_VECTOR_WIDTH_PORTABLE_128_V1 = 0,
  UNIJIT_VECTOR_WIDTH_NATIVE_V1 = 1
};

enum {
  UNIJIT_TARGET_FEATURE_FP64_V1 = UINT64_C(1) << 0U,
  UNIJIT_TARGET_FEATURE_NEON_V1 = UINT64_C(1) << 1U,
  UNIJIT_TARGET_FEATURE_SSE2_V1 = UINT64_C(1) << 2U,
  UNIJIT_TARGET_FEATURE_AVX_V1 = UINT64_C(1) << 3U,
  UNIJIT_TARGET_FEATURE_AVX2_V1 = UINT64_C(1) << 4U,
  UNIJIT_TARGET_FEATURE_FMA_V1 = UINT64_C(1) << 5U,
  UNIJIT_TARGET_FEATURE_RISCV_INTEGER_MULTIPLY_V1 = UINT64_C(1) << 6U,
  UNIJIT_TARGET_FEATURE_RISCV_FLOAT64_V1 = UINT64_C(1) << 7U,
  UNIJIT_TARGET_FEATURE_RISCV_VECTOR_V1 = UINT64_C(1) << 8U,
  UNIJIT_TARGET_FEATURE_AARCH64_LSE_V1 = UINT64_C(1) << 9U,
  UNIJIT_TARGET_FEATURE_RISCV_ATOMIC_V1 = UINT64_C(1) << 10U
};

enum {
  UNIJIT_PATCH_CELL_VALUE_V1 = 0,
  UNIJIT_PATCH_CELL_TARGET_V1 = 1,
  UNIJIT_PATCH_CELL_SHAPE_V1 = 2,
  UNIJIT_PATCH_CELL_GENERATION_V1 = 3,
  UNIJIT_PATCH_CELL_COUNTER_V1 = 4
};

typedef struct unijit_error_v1 unijit_error_v1;
typedef struct unijit_builder_v1 unijit_builder_v1;
typedef struct unijit_function_v1 unijit_function_v1;
typedef struct unijit_compiler_v1 unijit_compiler_v1;
typedef struct unijit_compiled_function_v1 unijit_compiled_function_v1;
typedef struct unijit_execution_context_v1 unijit_execution_context_v1;
typedef struct unijit_code_cache_v1 unijit_code_cache_v1;
typedef struct unijit_code_handle_v1 unijit_code_handle_v1;

typedef struct unijit_value_v1 {
  uint32_t id;
} unijit_value_v1;

typedef struct unijit_fast_call_slot_v1 {
  uint32_t id;
} unijit_fast_call_slot_v1;

typedef struct unijit_patch_cell_slot_v1 {
  uint32_t id;
} unijit_patch_cell_slot_v1;

typedef struct unijit_abi_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t word_bits;
  uint32_t pointer_bits;
  uint32_t little_endian;
  uint32_t reserved0;
  uint64_t reserved[4];
} unijit_abi_info_v1;

typedef struct unijit_target_profile_v1 {
  uint32_t struct_size;
  unijit_target_architecture_v1 architecture;
  unijit_target_abi_v1 abi;
  unijit_target_endianness_v1 endianness;
  uint64_t features;
  unijit_vector_width_policy_v1 vector_width_policy;
  uint32_t maximum_vector_bits;
  uint64_t reserved[2];
} unijit_target_profile_v1;

typedef struct unijit_compilation_limits_v1 {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t maximum_parameters;
  uint64_t maximum_ir_nodes;
  uint64_t maximum_cfg_blocks;
  uint64_t maximum_ir_arguments;
  uint64_t maximum_memory_regions;
  uint64_t maximum_memory_accesses;
  uint64_t maximum_atomic_accesses;
  uint64_t maximum_vector_constants;
  uint64_t maximum_vector_shuffles;
  uint64_t maximum_vector_selects;
  uint64_t maximum_frame_slots;
  uint64_t maximum_trusted_objects;
  uint64_t maximum_patch_cells;
  uint64_t maximum_fast_calls;
  uint64_t maximum_stack_maps;
  uint64_t maximum_metadata_values;
  uint64_t maximum_code_bytes;
  uint64_t reserved[4];
} unijit_compilation_limits_v1;

typedef struct unijit_compiler_options_v1 {
  uint32_t struct_size;
  unijit_optimization_level_v1 optimization_level;
  uint32_t measure_safepoint_polls;
  uint32_t reserved0;
  unijit_compilation_limits_v1 limits;
  unijit_target_profile_v1 target_profile;
  uint64_t reserved[4];
} unijit_compiler_options_v1;

typedef struct unijit_compilation_stats_v1 {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t code_size;
  uint64_t executable_mapping_size;
  uint64_t spill_slots;
  uint64_t frame_slots;
  uint64_t trusted_objects;
  uint64_t patch_cells;
  uint64_t fast_calls;
  uint64_t input_ir_nodes;
  uint64_t optimized_ir_nodes;
  uint64_t stack_map_count;
  uint64_t stack_map_value_count;
  uint64_t reserved[4];
} unijit_compilation_stats_v1;

typedef struct unijit_code_cache_limits_v1 {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t maximum_entries;
  uint64_t maximum_code_bytes;
  uint64_t reserved[4];
} unijit_code_cache_limits_v1;

typedef struct unijit_code_cache_stats_v1 {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t lookups;
  uint64_t hits;
  uint64_t misses;
  uint64_t publications;
  uint64_t publication_reuses;
  uint64_t uncached_publications;
  uint64_t replacements;
  uint64_t invalidations;
  uint64_t assumption_invalidations;
  uint64_t evictions;
  uint64_t clears;
  uint64_t resident_entries;
  uint64_t resident_code_bytes;
  uint64_t reserved[4];
} unijit_code_cache_stats_v1;

UNIJIT_C_API uint32_t unijit_v1_abi_version(void);
UNIJIT_C_API unijit_status_code_v1
unijit_v1_abi_info(unijit_abi_info_v1* info, unijit_error_v1** error);

UNIJIT_C_API unijit_status_code_v1
unijit_v1_error_code(const unijit_error_v1* error);
UNIJIT_C_API uint64_t unijit_v1_error_detail(const unijit_error_v1* error);
UNIJIT_C_API const char* unijit_v1_error_message(const unijit_error_v1* error);
UNIJIT_C_API uint64_t
unijit_v1_error_message_size(const unijit_error_v1* error);
UNIJIT_C_API void unijit_v1_error_destroy(unijit_error_v1* error);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_target_profile_baseline(
    unijit_target_profile_v1* profile, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_target_profile_host(
    unijit_target_profile_v1* profile, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_target_profile_validate(
    const unijit_target_profile_v1* profile, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_target_profile_key(
    const unijit_target_profile_v1* profile, uint64_t* key,
    unijit_error_v1** error);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_compilation_limits_init(
    unijit_compilation_limits_v1* limits, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_compiler_options_init(
    unijit_compiler_options_v1* options, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_limits_init(
    unijit_code_cache_limits_v1* limits, unijit_error_v1** error);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_create(
    const unijit_value_type_v1* parameter_types, uint64_t parameter_count,
    const unijit_compilation_limits_v1* limits, unijit_builder_v1** builder,
    unijit_error_v1** error);
UNIJIT_C_API void unijit_v1_builder_destroy(unijit_builder_v1* builder);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_parameter(
    unijit_builder_v1* builder, uint64_t index, unijit_value_v1* value,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_word_constant(
    unijit_builder_v1* builder, unijit_word_v1 bits,
    unijit_value_v1* value, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_float64_constant_bits(
    unijit_builder_v1* builder, unijit_word_v1 bits,
    unijit_value_v1* value, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_binary(
    unijit_builder_v1* builder, unijit_binary_operation_v1 operation,
    unijit_value_v1 lhs, unijit_value_v1 rhs, unijit_value_v1* value,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_unary(
    unijit_builder_v1* builder, unijit_unary_operation_v1 operation,
    unijit_value_v1 input, unijit_value_v1* value,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_guard_nonzero(
    unijit_builder_v1* builder, unijit_value_v1 input, uint64_t site,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_safepoint(
    unijit_builder_v1* builder, uint64_t site, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_declare_fast_call(
    unijit_builder_v1* builder,
    const unijit_value_type_v1* parameter_types, uint64_t parameter_count,
    unijit_value_type_v1 return_type, unijit_fast_call_slot_v1* slot,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_fast_call(
    unijit_builder_v1* builder, unijit_fast_call_slot_v1 slot,
    const unijit_value_v1* arguments, uint64_t argument_count,
    unijit_value_v1* value, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_declare_patch_cell(
    unijit_builder_v1* builder, unijit_word_v1 initial_value,
    unijit_patch_cell_kind_v1 kind, unijit_patch_cell_slot_v1* slot,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_load_patch_cell(
    unijit_builder_v1* builder, unijit_patch_cell_slot_v1 slot,
    unijit_value_v1* value, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_set_return(
    unijit_builder_v1* builder, unijit_value_v1 value,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_builder_finish(
    unijit_builder_v1* builder, unijit_function_v1** function,
    unijit_error_v1** error);

UNIJIT_C_API void unijit_v1_function_destroy(unijit_function_v1* function);
UNIJIT_C_API uint64_t
unijit_v1_function_parameter_count(const unijit_function_v1* function);
UNIJIT_C_API unijit_value_type_v1 unijit_v1_function_parameter_type(
    const unijit_function_v1* function, uint64_t index);
UNIJIT_C_API unijit_value_type_v1
unijit_v1_function_return_type(const unijit_function_v1* function);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_compiler_create(
    const unijit_compiler_options_v1* options, unijit_compiler_v1** compiler,
    unijit_error_v1** error);
UNIJIT_C_API void unijit_v1_compiler_destroy(unijit_compiler_v1* compiler);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_compiler_compile(
    const unijit_compiler_v1* compiler, const unijit_function_v1* function,
    unijit_compiled_function_v1** compiled, unijit_error_v1** error);

UNIJIT_C_API void
unijit_v1_compiled_function_destroy(unijit_compiled_function_v1* function);
UNIJIT_C_API uint64_t unijit_v1_compiled_function_parameter_count(
    const unijit_compiled_function_v1* function);
UNIJIT_C_API unijit_value_type_v1 unijit_v1_compiled_function_parameter_type(
    const unijit_compiled_function_v1* function, uint64_t index);
UNIJIT_C_API unijit_value_type_v1 unijit_v1_compiled_function_return_type(
    const unijit_compiled_function_v1* function);
UNIJIT_C_API uint32_t unijit_v1_compiled_function_requires_context(
    const unijit_compiled_function_v1* function);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_compiled_function_stats(
    const unijit_compiled_function_v1* function,
    unijit_compilation_stats_v1* stats, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_compiled_function_invoke(
    const unijit_compiled_function_v1* function, const unijit_word_v1* args,
    uint64_t arg_count, unijit_execution_context_v1* context,
    unijit_word_v1* result, unijit_error_v1** error);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_execution_context_create(
    unijit_execution_context_v1** context, unijit_error_v1** error);
UNIJIT_C_API void
unijit_v1_execution_context_destroy(unijit_execution_context_v1* context);
UNIJIT_C_API void unijit_v1_execution_context_request_interrupt(
    unijit_execution_context_v1* context);
UNIJIT_C_API void unijit_v1_execution_context_clear_interrupt(
    unijit_execution_context_v1* context);
UNIJIT_C_API uint32_t unijit_v1_execution_context_interrupt_requested(
    const unijit_execution_context_v1* context);
UNIJIT_C_API uint64_t unijit_v1_execution_context_exit_reason(
    const unijit_execution_context_v1* context);
UNIJIT_C_API uint64_t unijit_v1_execution_context_exit_site(
    const unijit_execution_context_v1* context);
UNIJIT_C_API unijit_word_v1 unijit_v1_execution_context_exit_value(
    const unijit_execution_context_v1* context);
UNIJIT_C_API uint64_t unijit_v1_execution_context_safepoint_polls(
    const unijit_execution_context_v1* context);

UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_create(
    const unijit_code_cache_limits_v1* limits,
    const unijit_target_profile_v1* target_profile,
    unijit_code_cache_v1** cache, unijit_error_v1** error);
UNIJIT_C_API void unijit_v1_code_cache_destroy(unijit_code_cache_v1* cache);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_publish(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, unijit_compiled_function_v1** compiled,
    unijit_code_handle_v1** handle, uint32_t* cached, uint32_t* reused,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_find(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, unijit_code_handle_v1** handle,
    unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_invalidate(
    unijit_code_cache_v1* cache, const char* key, uint64_t key_size,
    uint64_t fingerprint, uint32_t* invalidated, unijit_error_v1** error);
UNIJIT_C_API void unijit_v1_code_cache_clear(unijit_code_cache_v1* cache);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_cache_stats(
    const unijit_code_cache_v1* cache, unijit_code_cache_stats_v1* stats,
    unijit_error_v1** error);

UNIJIT_C_API void
unijit_v1_code_handle_destroy(unijit_code_handle_v1* handle);
UNIJIT_C_API uint64_t
unijit_v1_code_handle_generation(const unijit_code_handle_v1* handle);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_handle_invoke(
    const unijit_code_handle_v1* handle, const unijit_word_v1* args,
    uint64_t arg_count, unijit_execution_context_v1* context,
    unijit_word_v1* result, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_handle_bind_fast_call(
    const unijit_code_handle_v1* caller, uint64_t index,
    const unijit_code_handle_v1* target, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_handle_clear_fast_call(
    const unijit_code_handle_v1* caller, uint64_t index,
    unijit_error_v1** error);
UNIJIT_C_API uint32_t unijit_v1_code_handle_fast_call_bound(
    const unijit_code_handle_v1* caller, uint64_t index);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_handle_read_patch_cell(
    const unijit_code_handle_v1* handle, uint64_t index,
    unijit_word_v1* value, unijit_error_v1** error);
UNIJIT_C_API unijit_status_code_v1 unijit_v1_code_handle_publish_patch_cell(
    const unijit_code_handle_v1* handle, uint64_t index,
    unijit_word_v1 value, unijit_error_v1** error);

#ifdef __cplusplus
}
#endif

#endif
