#include <unijit/embedding.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(unijit_word_v1) == 8, "C ABI words must be 64-bit");
_Static_assert(sizeof(unijit_value_v1) == 4, "value tokens must stay stable");
_Static_assert(sizeof(unijit_fast_call_slot_v1) == 4,
               "fast-call tokens must stay stable");
_Static_assert(sizeof(unijit_patch_cell_slot_v1) == 4,
               "patch-cell tokens must stay stable");
_Static_assert(sizeof(unijit_abi_info_v1) == 56,
               "ABI-info layout must stay stable");
_Static_assert(sizeof(unijit_target_profile_v1) == 48,
               "target-profile layout must stay stable");
_Static_assert(sizeof(unijit_compilation_limits_v1) == 176,
               "compilation-limits layout must stay stable");
_Static_assert(sizeof(unijit_compiler_options_v1) == 272,
               "compiler-options layout must stay stable");
_Static_assert(sizeof(unijit_compilation_stats_v1) == 128,
               "compilation-stats layout must stay stable");
_Static_assert(sizeof(unijit_code_cache_limits_v1) == 56,
               "code-cache-limits layout must stay stable");
_Static_assert(sizeof(unijit_code_cache_stats_v1) == 144,
               "code-cache-stats layout must stay stable");
_Static_assert(offsetof(unijit_compiler_options_v1, limits) == 16,
               "nested limits offset must stay stable");
_Static_assert(offsetof(unijit_compiler_options_v1, target_profile) == 192,
               "target-profile offset must stay stable");

static void fail(const char* message, int line) {
  fprintf(stderr, "embedding C ABI test failed at line %d: %s\n", line,
          message);
  exit(EXIT_FAILURE);
}

#define REQUIRE(condition, message)       \
  do {                                    \
    if (!(condition)) {                   \
      fail((message), __LINE__);          \
    }                                     \
  } while (0)

static void require_ok(unijit_status_code_v1 status, unijit_error_v1* error,
                       int line) {
  if (status != UNIJIT_STATUS_OK_V1) {
    const char* message = unijit_v1_error_message(error);
    fprintf(stderr,
            "embedding C ABI call failed at line %d: status=%" PRIu32
            " detail=%" PRIu64 " message=%s\n",
            line, status, unijit_v1_error_detail(error),
            message == NULL ? "<missing>" : message);
    unijit_v1_error_destroy(error);
    exit(EXIT_FAILURE);
  }
  REQUIRE(error == NULL, "successful calls must clear diagnostic output");
}

#define REQUIRE_OK(expression)                         \
  do {                                                 \
    unijit_status_code_v1 status_ = (expression);      \
    require_ok(status_, error, __LINE__);              \
  } while (0)

static void require_status(unijit_status_code_v1 expected,
                           unijit_status_code_v1 actual,
                           unijit_error_v1* error, int line) {
  if (actual != expected) {
    fprintf(stderr,
            "embedding C ABI status mismatch at line %d: expected=%" PRIu32
            " actual=%" PRIu32 "\n",
            line, expected, actual);
    unijit_v1_error_destroy(error);
    exit(EXIT_FAILURE);
  }
  REQUIRE(error != NULL, "failed calls must provide a diagnostic");
  REQUIRE(unijit_v1_error_code(error) == expected,
          "diagnostic code must match returned status");
  REQUIRE(unijit_v1_error_message(error) != NULL,
          "diagnostic message must be present");
  REQUIRE(unijit_v1_error_message_size(error) > 0,
          "diagnostic message must not be empty");
  REQUIRE(unijit_v1_error_message_size(error) <= 1024,
          "diagnostics must remain bounded");
  unijit_v1_error_destroy(error);
}

#define REQUIRE_STATUS(expected, expression)                 \
  do {                                                       \
    unijit_status_code_v1 status_ = (expression);            \
    require_status((expected), status_, error, __LINE__);    \
    error = NULL;                                            \
  } while (0)

static unijit_compiled_function_v1* compile_function(
    unijit_compiler_v1* compiler, unijit_function_v1* function) {
  unijit_error_v1* error = NULL;
  unijit_compiled_function_v1* compiled = NULL;
  REQUIRE_OK(unijit_v1_compiler_compile(compiler, function, &compiled, &error));
  REQUIRE(compiled != NULL, "compiler must return an owning handle");
  return compiled;
}

static unijit_function_v1* build_binary(unijit_value_type_v1 type,
                                        unijit_binary_operation_v1 operation) {
  unijit_error_v1* error = NULL;
  const unijit_value_type_v1 parameters[2] = {type, type};
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_value_v1 lhs;
  unijit_value_v1 rhs;
  unijit_value_v1 result;
  REQUIRE_OK(unijit_v1_builder_create(parameters, 2, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 0, &lhs, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 1, &rhs, &error));
  REQUIRE_OK(unijit_v1_builder_binary(builder, operation, lhs, rhs, &result,
                                      &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, result, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  unijit_v1_builder_destroy(builder);
  return function;
}

static unijit_function_v1* build_safepoint(void) {
  unijit_error_v1* error = NULL;
  const unijit_value_type_v1 parameter = UNIJIT_VALUE_WORD_V1;
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_value_v1 value;
  REQUIRE_OK(unijit_v1_builder_create(&parameter, 1, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 0, &value, &error));
  REQUIRE_OK(unijit_v1_builder_safepoint(builder, 77, &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, value, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  unijit_v1_builder_destroy(builder);
  return function;
}

static unijit_function_v1* build_patch_cell(void) {
  unijit_error_v1* error = NULL;
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_patch_cell_slot_v1 slot;
  unijit_value_v1 value;
  REQUIRE_OK(unijit_v1_builder_create(NULL, 0, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_declare_patch_cell(
      builder, 41, UNIJIT_PATCH_CELL_VALUE_V1, &slot, &error));
  REQUIRE(slot.id == 0, "first patch-cell slot must be zero");
  REQUIRE_OK(unijit_v1_builder_load_patch_cell(builder, slot, &value, &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, value, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  unijit_v1_builder_destroy(builder);
  return function;
}

static unijit_function_v1* build_fast_target(void) {
  unijit_error_v1* error = NULL;
  const unijit_value_type_v1 parameter = UNIJIT_VALUE_WORD_V1;
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_value_v1 input;
  unijit_value_v1 five;
  unijit_value_v1 result;
  REQUIRE_OK(unijit_v1_builder_create(&parameter, 1, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 0, &input, &error));
  REQUIRE_OK(unijit_v1_builder_word_constant(builder, 5, &five, &error));
  REQUIRE_OK(unijit_v1_builder_binary(builder, UNIJIT_BINARY_WORD_ADD_V1,
                                      input, five, &result, &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, result, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  unijit_v1_builder_destroy(builder);
  return function;
}

static unijit_function_v1* build_fast_caller(void) {
  unijit_error_v1* error = NULL;
  const unijit_value_type_v1 parameter = UNIJIT_VALUE_WORD_V1;
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_fast_call_slot_v1 slot;
  unijit_value_v1 input;
  unijit_value_v1 result;
  REQUIRE_OK(unijit_v1_builder_create(&parameter, 1, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 0, &input, &error));
  REQUIRE_OK(unijit_v1_builder_declare_fast_call(
      builder, &parameter, 1, UNIJIT_VALUE_WORD_V1, &slot, &error));
  REQUIRE(slot.id == 0, "first fast-call slot must be zero");
  REQUIRE_OK(
      unijit_v1_builder_fast_call(builder, slot, &input, 1, &result, &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, result, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  unijit_v1_builder_destroy(builder);
  return function;
}

static unijit_code_handle_v1* publish(
    unijit_code_cache_v1* cache, const char* key, uint64_t fingerprint,
    unijit_compiled_function_v1** compiled) {
  unijit_error_v1* error = NULL;
  unijit_code_handle_v1* handle = NULL;
  uint32_t cached = 0;
  uint32_t reused = 0;
  REQUIRE_OK(unijit_v1_code_cache_publish(
      cache, key, (uint64_t)strlen(key), fingerprint, compiled, &handle,
      &cached, &reused, &error));
  REQUIRE(*compiled == NULL, "publication must consume compiled ownership");
  REQUIRE(handle != NULL, "publication must return a code handle");
  REQUIRE(cached == 1, "qualified publications must be cached");
  REQUIRE(reused == 0, "first publication must not be a reuse");
  REQUIRE(unijit_v1_code_handle_generation(handle) > 0,
          "published code must have a generation");
  return handle;
}

static void test_abi_and_diagnostics(void) {
  unijit_error_v1* error = NULL;
  unijit_abi_info_v1 info;
  unijit_target_profile_v1 baseline;
  unijit_target_profile_v1 host;
  unijit_compilation_limits_v1 limits;
  unijit_compiler_options_v1 options;
  unijit_code_cache_limits_v1 cache_limits;
  uint64_t key = 0;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  REQUIRE(unijit_v1_abi_version() == UNIJIT_C_ABI_VERSION,
          "runtime and header ABI versions must agree");
  REQUIRE_OK(unijit_v1_abi_info(&info, &error));
  REQUIRE(info.abi_version == UNIJIT_C_ABI_VERSION,
          "ABI info must report the header version");
  REQUIRE(info.word_bits == 64 && info.pointer_bits == 64,
          "commercial ABI requires 64-bit words and pointers");
  REQUIRE(info.little_endian == 1, "supported execution hosts are little-endian");

  memset(&baseline, 0, sizeof(baseline));
  baseline.struct_size = sizeof(baseline);
  REQUIRE_OK(unijit_v1_target_profile_baseline(&baseline, &error));
  REQUIRE_OK(unijit_v1_target_profile_validate(&baseline, &error));
  REQUIRE_OK(unijit_v1_target_profile_key(&baseline, &key, &error));
  REQUIRE(key != 0, "target compatibility key must be nonzero");
  memset(&host, 0, sizeof(host));
  host.struct_size = sizeof(host);
  REQUIRE_OK(unijit_v1_target_profile_host(&host, &error));
  REQUIRE_OK(unijit_v1_target_profile_validate(&host, &error));

  memset(&limits, 0, sizeof(limits));
  limits.struct_size = sizeof(limits);
  REQUIRE_OK(unijit_v1_compilation_limits_init(&limits, &error));
  REQUIRE(limits.maximum_ir_nodes > 0 && limits.maximum_code_bytes > 0,
          "default compilation budgets must be bounded and usable");
  memset(&options, 0, sizeof(options));
  options.struct_size = sizeof(options);
  REQUIRE_OK(unijit_v1_compiler_options_init(&options, &error));
  REQUIRE(options.limits.struct_size == sizeof(options.limits),
          "nested compiler limits must be initialized");
  memset(&cache_limits, 0, sizeof(cache_limits));
  cache_limits.struct_size = sizeof(cache_limits);
  REQUIRE_OK(unijit_v1_code_cache_limits_init(&cache_limits, &error));
  REQUIRE(cache_limits.maximum_entries > 0 &&
              cache_limits.maximum_code_bytes > 0,
          "default cache budgets must be bounded and usable");

  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info) - 1;
  REQUIRE_STATUS(UNIJIT_STATUS_INVALID_ARGUMENT_V1,
                 unijit_v1_abi_info(&info, &error));
  {
    unijit_error_v1* retained_error = NULL;
    unijit_error_v1* same_error = NULL;
    info.struct_size = sizeof(info) - 1;
    REQUIRE(unijit_v1_abi_info(&info, &retained_error) ==
                UNIJIT_STATUS_INVALID_ARGUMENT_V1 &&
                retained_error != NULL,
            "negative calls must create an owned diagnostic");
    same_error = retained_error;
    info.struct_size = sizeof(info);
    REQUIRE(unijit_v1_abi_info(&info, &retained_error) ==
                UNIJIT_STATUS_INVALID_ARGUMENT_V1 &&
                retained_error == same_error,
            "a non-null error output must be rejected without leaking it");
    unijit_v1_error_destroy(retained_error);
  }
  options.reserved[0] = 1;
  {
    unijit_compiler_v1* compiler = NULL;
    REQUIRE_STATUS(UNIJIT_STATUS_INVALID_ARGUMENT_V1,
                   unijit_v1_compiler_create(&options, &compiler, &error));
    REQUIRE(compiler == NULL, "malformed options must not create a compiler");
  }
}

static void test_scalar_compilation(unijit_compiler_v1* compiler) {
  unijit_error_v1* error = NULL;
  unijit_function_v1* function =
      build_binary(UNIJIT_VALUE_WORD_V1, UNIJIT_BINARY_WORD_ADD_V1);
  unijit_compiled_function_v1* compiled = compile_function(compiler, function);
  const unijit_word_v1 arguments[2] = {19, 23};
  unijit_word_v1 result = 0;
  unijit_compilation_stats_v1 stats;
  REQUIRE(unijit_v1_function_parameter_count(function) == 2,
          "function signature must retain its arity");
  REQUIRE(unijit_v1_function_parameter_type(function, 0) ==
              UNIJIT_VALUE_WORD_V1,
          "function signature must retain parameter types");
  REQUIRE(unijit_v1_compiled_function_parameter_count(compiled) == 2,
          "compiled signature must retain its arity");
  REQUIRE_OK(unijit_v1_compiled_function_invoke(
      compiled, arguments, 2, NULL, &result, &error));
  REQUIRE(result == 42, "word addition must execute through the C ABI");
  REQUIRE_STATUS(UNIJIT_STATUS_INVALID_ARGUMENT_V1,
                 unijit_v1_compiled_function_invoke(
                     compiled, arguments, 1, NULL, &result, &error));
  memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  REQUIRE_OK(unijit_v1_compiled_function_stats(compiled, &stats, &error));
  REQUIRE(stats.code_size > 0 && stats.input_ir_nodes >= 3,
          "compilation statistics must describe emitted code");
  unijit_v1_compiled_function_destroy(compiled);
  unijit_v1_function_destroy(function);

  function = build_binary(UNIJIT_VALUE_FLOAT64_V1,
                          UNIJIT_BINARY_FLOAT64_ADD_V1);
  compiled = compile_function(compiler, function);
  {
    const double lhs = 1.25;
    const double rhs = 2.5;
    double output = 0.0;
    unijit_word_v1 float_arguments[2];
    memcpy(&float_arguments[0], &lhs, sizeof(lhs));
    memcpy(&float_arguments[1], &rhs, sizeof(rhs));
    REQUIRE_OK(unijit_v1_compiled_function_invoke(
        compiled, float_arguments, 2, NULL, &result, &error));
    memcpy(&output, &result, sizeof(output));
    REQUIRE(output == 3.75, "Float64 values must cross the C ABI as value bits");
  }
  unijit_v1_compiled_function_destroy(compiled);
  unijit_v1_function_destroy(function);
}

static void test_safepoint(unijit_compiler_v1* compiler) {
  unijit_error_v1* error = NULL;
  unijit_function_v1* function = build_safepoint();
  unijit_compiled_function_v1* compiled = compile_function(compiler, function);
  unijit_execution_context_v1* context = NULL;
  const unijit_word_v1 argument = 123;
  unijit_word_v1 result = 0;
  REQUIRE(unijit_v1_compiled_function_requires_context(compiled) == 1,
          "safepoint code must require an execution context");
  REQUIRE_OK(unijit_v1_execution_context_create(&context, &error));
  unijit_v1_execution_context_request_interrupt(context);
  REQUIRE(unijit_v1_execution_context_interrupt_requested(context) == 1,
          "interrupt requests must be observable");
  REQUIRE_STATUS(UNIJIT_STATUS_EXECUTION_INTERRUPTED_V1,
                 unijit_v1_compiled_function_invoke(
                     compiled, &argument, 1, context, &result, &error));
  REQUIRE(unijit_v1_execution_context_exit_reason(context) != 0,
          "interrupt exits must retain a reason");
  REQUIRE(unijit_v1_execution_context_exit_site(context) == 77,
          "interrupt exits must retain the safepoint site");
  unijit_v1_execution_context_clear_interrupt(context);
  REQUIRE_OK(unijit_v1_compiled_function_invoke(
      compiled, &argument, 1, context, &result, &error));
  REQUIRE(result == argument, "cleared interrupts must allow execution");
  REQUIRE(unijit_v1_execution_context_safepoint_polls(context) == 1,
          "the latest invocation's measured safepoint polls must cross the ABI");
  unijit_v1_execution_context_destroy(context);
  unijit_v1_compiled_function_destroy(compiled);
  unijit_v1_function_destroy(function);
}

static void test_cache_patch_and_fast_calls(unijit_compiler_v1* compiler) {
  unijit_error_v1* error = NULL;
  unijit_target_profile_v1 target;
  unijit_code_cache_v1* cache = NULL;
  unijit_code_handle_v1* patch = NULL;
  unijit_code_handle_v1* found = NULL;
  unijit_code_handle_v1* target_handle = NULL;
  unijit_code_handle_v1* caller_handle = NULL;
  unijit_compiled_function_v1* compiled = NULL;
  unijit_function_v1* function = NULL;
  unijit_word_v1 result = 0;
  uint32_t invalidated = 0;
  unijit_code_cache_stats_v1 stats;

  memset(&target, 0, sizeof(target));
  target.struct_size = sizeof(target);
  REQUIRE_OK(unijit_v1_target_profile_baseline(&target, &error));
  REQUIRE_OK(unijit_v1_code_cache_create(NULL, &target, &cache, &error));

  function = build_patch_cell();
  compiled = compile_function(compiler, function);
  patch = publish(cache, "patch", UINT64_C(0x5041544348), &compiled);
  unijit_v1_function_destroy(function);
  REQUIRE_OK(unijit_v1_code_handle_invoke(patch, NULL, 0, NULL, &result,
                                          &error));
  REQUIRE(result == 41, "patch-cell initial value must be executable");
  REQUIRE_OK(unijit_v1_code_handle_read_patch_cell(patch, 0, &result, &error));
  REQUIRE(result == 41, "patch-cell reads must cross the C ABI");
  REQUIRE_OK(
      unijit_v1_code_handle_publish_patch_cell(patch, 0, 99, &error));
  REQUIRE_OK(unijit_v1_code_handle_invoke(patch, NULL, 0, NULL, &result,
                                          &error));
  REQUIRE(result == 99, "published patch values must affect retained code");
  REQUIRE_OK(unijit_v1_code_cache_find(
      cache, "patch", 5, UINT64_C(0x5041544348), &found, &error));
  REQUIRE(unijit_v1_code_handle_generation(found) ==
              unijit_v1_code_handle_generation(patch),
          "cache lookup must retain the published generation");
  REQUIRE_OK(unijit_v1_code_cache_invalidate(
      cache, "patch", 5, UINT64_C(0x5041544348), &invalidated, &error));
  REQUIRE(invalidated == 1, "cache invalidation must report removal");
  REQUIRE_OK(unijit_v1_code_handle_invoke(found, NULL, 0, NULL, &result,
                                          &error));
  REQUIRE(result == 99, "retained handles must survive cache invalidation");
  unijit_v1_code_handle_destroy(found);
  unijit_v1_code_handle_destroy(patch);

  function = build_fast_target();
  compiled = compile_function(compiler, function);
  target_handle = publish(cache, "target", UINT64_C(0x544152474554),
                          &compiled);
  unijit_v1_function_destroy(function);
  function = build_fast_caller();
  compiled = compile_function(compiler, function);
  caller_handle = publish(cache, "caller", UINT64_C(0x43414C4C4552),
                          &compiled);
  unijit_v1_function_destroy(function);
  REQUIRE(unijit_v1_code_handle_fast_call_bound(caller_handle, 0) == 0,
          "new fast-call slots must start unbound");
  REQUIRE_OK(unijit_v1_code_handle_bind_fast_call(
      caller_handle, 0, target_handle, &error));
  REQUIRE(unijit_v1_code_handle_fast_call_bound(caller_handle, 0) == 1,
          "bound fast calls must be observable");
  {
    const unijit_word_v1 argument = 37;
    REQUIRE_OK(unijit_v1_code_handle_invoke(
        caller_handle, &argument, 1, NULL, &result, &error));
    REQUIRE(result == 42, "bound fast calls must execute the target generation");
  }
  REQUIRE_OK(unijit_v1_code_cache_invalidate(
      cache, "target", 6, UINT64_C(0x544152474554), &invalidated, &error));
  REQUIRE(invalidated == 1, "target generation must leave the cache");
  unijit_v1_code_handle_destroy(target_handle);
  target_handle = NULL;
  {
    const unijit_word_v1 argument = 8;
    REQUIRE_OK(unijit_v1_code_handle_invoke(
        caller_handle, &argument, 1, NULL, &result, &error));
    REQUIRE(result == 13,
            "caller binding must retain an invalidated target generation");
  }
  REQUIRE_OK(
      unijit_v1_code_handle_clear_fast_call(caller_handle, 0, &error));
  {
    const unijit_word_v1 argument = 8;
    REQUIRE_STATUS(UNIJIT_STATUS_UNAVAILABLE_V1,
                   unijit_v1_code_handle_invoke(
                       caller_handle, &argument, 1, NULL, &result, &error));
  }

  memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  REQUIRE_OK(unijit_v1_code_cache_stats(cache, &stats, &error));
  REQUIRE(stats.publications >= 3 && stats.hits >= 1 &&
              stats.invalidations >= 2,
          "cache statistics must report lifecycle operations");
  unijit_v1_code_handle_destroy(caller_handle);
  unijit_v1_code_cache_destroy(cache);
}

static void test_resource_limit(void) {
  unijit_error_v1* error = NULL;
  unijit_compilation_limits_v1 limits;
  unijit_builder_v1* builder = NULL;
  unijit_value_v1 value;
  memset(&limits, 0, sizeof(limits));
  limits.struct_size = sizeof(limits);
  REQUIRE_OK(unijit_v1_compilation_limits_init(&limits, &error));
  limits.maximum_ir_nodes = 1;
  REQUIRE_OK(unijit_v1_builder_create(NULL, 0, &limits, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_word_constant(builder, 1, &value, &error));
  REQUIRE_STATUS(UNIJIT_STATUS_RESOURCE_EXHAUSTED_V1,
                 unijit_v1_builder_word_constant(builder, 2, &value, &error));
  unijit_v1_builder_destroy(builder);
}

int main(void) {
  unijit_error_v1* error = NULL;
  unijit_compiler_options_v1 options;
  unijit_compiler_v1* compiler = NULL;
  test_abi_and_diagnostics();
  memset(&options, 0, sizeof(options));
  options.struct_size = sizeof(options);
  REQUIRE_OK(unijit_v1_compiler_options_init(&options, &error));
  options.measure_safepoint_polls = 1;
  REQUIRE_OK(unijit_v1_compiler_create(&options, &compiler, &error));
  test_scalar_compilation(compiler);
  test_safepoint(compiler);
  test_cache_patch_and_fast_calls(compiler);
  test_resource_limit();
  unijit_v1_compiler_destroy(compiler);
  puts("embedding C ABI qualification passed");
  return EXIT_SUCCESS;
}
