#include <unijit/embedding.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void require_ok(unijit_status_code_v1 status,
                       unijit_error_v1* error) {
  if (status != UNIJIT_STATUS_OK_V1) {
    fprintf(stderr, "installed C ABI failed: %s\n",
            unijit_v1_error_message(error));
    unijit_v1_error_destroy(error);
    exit(EXIT_FAILURE);
  }
}

#define REQUIRE_OK(expression)                    \
  do {                                            \
    require_ok((expression), error);              \
    error = NULL;                                 \
  } while (0)

int main(void) {
  const unijit_value_type_v1 parameters[2] = {
      UNIJIT_VALUE_WORD_V1, UNIJIT_VALUE_WORD_V1};
  const unijit_word_v1 arguments[2] = {20, 22};
  unijit_error_v1* error = NULL;
  unijit_builder_v1* builder = NULL;
  unijit_function_v1* function = NULL;
  unijit_compiler_v1* compiler = NULL;
  unijit_compiled_function_v1* compiled = NULL;
  unijit_value_v1 lhs;
  unijit_value_v1 rhs;
  unijit_value_v1 sum;
  unijit_word_v1 result = 0;

  if (unijit_v1_abi_version() != UNIJIT_C_ABI_VERSION) {
    return EXIT_FAILURE;
  }
  REQUIRE_OK(
      unijit_v1_builder_create(parameters, 2, NULL, &builder, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 0, &lhs, &error));
  REQUIRE_OK(unijit_v1_builder_parameter(builder, 1, &rhs, &error));
  REQUIRE_OK(unijit_v1_builder_binary(builder, UNIJIT_BINARY_WORD_ADD_V1,
                                      lhs, rhs, &sum, &error));
  REQUIRE_OK(unijit_v1_builder_set_return(builder, sum, &error));
  REQUIRE_OK(unijit_v1_builder_finish(builder, &function, &error));
  REQUIRE_OK(unijit_v1_compiler_create(NULL, &compiler, &error));
  REQUIRE_OK(
      unijit_v1_compiler_compile(compiler, function, &compiled, &error));
  REQUIRE_OK(unijit_v1_compiled_function_invoke(
      compiled, arguments, 2, NULL, &result, &error));

  unijit_v1_compiled_function_destroy(compiled);
  unijit_v1_compiler_destroy(compiler);
  unijit_v1_function_destroy(function);
  unijit_v1_builder_destroy(builder);
  if (result != 42) {
    return EXIT_FAILURE;
  }
  puts("installed pure C17 embedding consumer passed");
  return EXIT_SUCCESS;
}
