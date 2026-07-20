#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <quickjs.h>

#include "source_translator.h"
#include "unijit_quickjs.h"
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

  if (!passed) {
    std::cerr << "stock QuickJS embedding smoke test failed\n";
    return EXIT_FAILURE;
  }

  const auto translation =
      unijit::frontend::quickjs::translate_numeric_function(
          "function affine(a, b) { return (a + 2.5) * (b - -3) / 2; }");
  if (!translation.ok() || translation.parameter_count != 2) {
    std::cerr << "QuickJS numeric source did not compile: "
              << translation.status.message() << '\n';
    return EXIT_FAILURE;
  }
  const std::array<unijit::ir::Word, 2> arguments = {
      unijit::ir::pack_float64(1.5), unijit::ir::pack_float64(4.0)};
  const auto native = translation.function->invoke(arguments.data(),
                                                    arguments.size());
  if (!native.ok() || unijit::ir::unpack_float64(native.value) != 14.0) {
    std::cerr << "QuickJS numeric source produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char*, 3> kRejectedSources = {
      "function(a, a) { return a; }",
      "function(a) { return external + a; }",
      "function(a) { const b = a; return b; }"};
  for (const char* source : kRejectedSources) {
    if (unijit::frontend::quickjs::translate_numeric_function(source).ok()) {
      std::cerr << "unsupported QuickJS source was accepted: " << source
                << '\n';
      return EXIT_FAILURE;
    }
  }

  if (unijit_quickjs_install(context) != 0) {
    std::cerr << "unable to install the UniJIT QuickJS module\n";
    return EXIT_FAILURE;
  }
  constexpr char kNativeSource[] =
      "function sourceFunction(a, b) {"
      "  return (a + 2.5) * (b - -3) / 2;"
      "}"
      "sourceFunction.toString = () => 'function(a, b) { return 999; }';"
      "const native = unijit.compile(sourceFunction);"
      "native(1.5, 4.0, 99);";
  result = JS_Eval(context, kNativeSource, std::strlen(kNativeSource),
                   "<unijit-quickjs-native>", JS_EVAL_TYPE_GLOBAL);
  number = 0.0;
  if (JS_IsException(result) ||
      JS_ToFloat64(context, &number, result) != 0 || number != 14.0) {
    std::cerr << "QuickJS did not execute the compiled native closure\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);

  constexpr char kGuardSource[] = "native('1.5', 4.0);";
  result = JS_Eval(context, kGuardSource, std::strlen(kGuardSource),
                   "<unijit-quickjs-guard>", JS_EVAL_TYPE_GLOBAL);
  if (!JS_IsException(result)) {
    std::cerr << "QuickJS native closure accepted a non-Number argument\n";
    JS_FreeValue(context, result);
    return EXIT_FAILURE;
  }
  JS_FreeValue(context, result);
  JSValue exception = JS_GetException(context);
  JS_FreeValue(context, exception);

  JS_RunGC(runtime);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  std::cout << "stock QuickJS embedding smoke test passed\n";
  return EXIT_SUCCESS;
}
