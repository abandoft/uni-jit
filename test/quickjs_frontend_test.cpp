#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <quickjs.h>

#include "source_translator.h"
#include "unijit/ir/function.h"

int main() {
  JSRuntime* runtime = JS_NewRuntime();
  if (runtime == nullptr) {
    std::cerr << "unable to create the stock QuickJS runtime\n";
    return EXIT_FAILURE;
  }
  JSContext* context = JS_NewContext(runtime);
  if (context == nullptr) {
    std::cerr << "unable to create the stock QuickJS context\n";
    JS_FreeRuntime(runtime);
    return EXIT_FAILURE;
  }

  constexpr char kSource[] = "(19 + 23)";
  JSValue result = JS_Eval(context, kSource, std::strlen(kSource),
                           "<unijit-quickjs-smoke>", JS_EVAL_TYPE_GLOBAL);
  double number = 0.0;
  const bool passed = !JS_IsException(result) &&
                      JS_ToFloat64(context, &number, result) == 0 &&
                      number == 42.0;
  JS_FreeValue(context, result);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);

  if (!passed) {
    std::cerr << "stock QuickJS embedding smoke test failed\n";
    return EXIT_FAILURE;
  }

  const auto translation =
      unijit::frontend::quickjs::translate_numeric_function(
          "function affine(a, b) { return (a + 2.5) * (b - -3); }");
  if (!translation.ok() || translation.parameter_count != 2) {
    std::cerr << "QuickJS numeric source did not compile: "
              << translation.status.message() << '\n';
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> arguments = {
      unijit::ir::pack_float64(1.5), unijit::ir::pack_float64(4.0)};
  const auto native = translation.function->invoke(arguments.data(),
                                                    arguments.size());
  if (!native.ok() || unijit::ir::unpack_float64(native.value) != 28.0) {
    std::cerr << "QuickJS numeric source produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char*, 4> kRejectedSources = {
      "function(a, a) { return a; }",
      "function(a) { return a / 2; }",
      "function(a) { return external + a; }",
      "function(a) { const b = a; return b; }"};
  for (const char* source : kRejectedSources) {
    if (unijit::frontend::quickjs::translate_numeric_function(source).ok()) {
      std::cerr << "unsupported QuickJS source was accepted: " << source
                << '\n';
      return EXIT_FAILURE;
    }
  }
  std::cout << "stock QuickJS embedding smoke test passed\n";
  return EXIT_SUCCESS;
}
