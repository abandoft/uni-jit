#include <cstdlib>
#include <cstring>
#include <iostream>

#include <quickjs.h>

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
  std::cout << "stock QuickJS embedding smoke test passed\n";
  return EXIT_SUCCESS;
}
