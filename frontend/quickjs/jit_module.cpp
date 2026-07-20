#include "unijit_quickjs.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

#include "source_translator.h"
#include "unijit/ir/function.h"

namespace {

using unijit::frontend::quickjs::TranslationResult;
using unijit::ir::Word;

constexpr std::size_t kMaximumParameters = 64;
JSClassID compiled_function_class_id = JS_INVALID_CLASS_ID;
std::once_flag compiled_function_class_once;

struct OwnedFunction final {
  std::size_t parameter_count{0};
  std::unique_ptr<unijit::jit::CompiledFunction> function;
};

void finalize_compiled_function(JSRuntime*, JSValue value) {
  auto* owned = static_cast<OwnedFunction*>(
      JS_GetOpaque(value, compiled_function_class_id));
  delete owned;
}

bool ensure_compiled_function_class(JSContext* context) {
  std::call_once(compiled_function_class_once, [] {
    JS_NewClassID(&compiled_function_class_id);
  });
  JSRuntime* runtime = JS_GetRuntime(context);
  if (JS_IsRegisteredClass(runtime, compiled_function_class_id) != 0) {
    return true;
  }
  JSClassDef definition{};
  definition.class_name = "UniJITCompiledFunction";
  definition.finalizer = finalize_compiled_function;
  return JS_NewClass(runtime, compiled_function_class_id, &definition) == 0;
}

JSValue invoke_compiled_function(JSContext* context, JSValueConst, int argc,
                                 JSValueConst* arguments, int,
                                 JSValue* function_data) {
  auto* owned = static_cast<OwnedFunction*>(
      JS_GetOpaque(function_data[0], compiled_function_class_id));
  if (owned == nullptr || owned->function == nullptr) {
    return JS_ThrowTypeError(context, "invalid UniJIT compiled function");
  }
  if (argc < static_cast<int>(owned->parameter_count)) {
    return JS_ThrowTypeError(context, "compiled function requires %zu arguments",
                             owned->parameter_count);
  }

  std::array<Word, kMaximumParameters> native_arguments{};
  for (std::size_t index = 0; index < owned->parameter_count; ++index) {
    if (JS_IsNumber(arguments[index]) == 0) {
      return JS_ThrowTypeError(context, "argument %zu must be a Number",
                               index + 1);
    }
    double number = 0.0;
    if (JS_ToFloat64(context, &number, arguments[index]) != 0) {
      return JS_EXCEPTION;
    }
    native_arguments[index] = unijit::ir::pack_float64(number);
  }

  const Word result = owned->function->native_entry()(native_arguments.data(),
                                                       nullptr);
  return JS_NewFloat64(context, unijit::ir::unpack_float64(result));
}

JSValue get_function_to_string(JSContext* context) {
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) {
    return global;
  }
  JSValue constructor = JS_GetPropertyStr(context, global, "Function");
  JS_FreeValue(context, global);
  if (JS_IsException(constructor)) {
    return constructor;
  }
  JSValue prototype = JS_GetPropertyStr(context, constructor, "prototype");
  JS_FreeValue(context, constructor);
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue to_string = JS_GetPropertyStr(context, prototype, "toString");
  JS_FreeValue(context, prototype);
  if (JS_IsException(to_string)) {
    return to_string;
  }
  if (JS_IsFunction(context, to_string) == 0) {
    JS_FreeValue(context, to_string);
    return JS_ThrowTypeError(context,
                             "Function.prototype.toString is not callable");
  }
  return to_string;
}

JSValue compile_with_to_string(JSContext* context, JSValueConst function_value,
                               JSValueConst to_string) {
  if (JS_IsFunction(context, function_value) == 0) {
    return JS_ThrowTypeError(context, "unijit.compile expects a function");
  }

  JSValue source_value =
      JS_Call(context, to_string, function_value, 0, nullptr);
  if (JS_IsException(source_value)) {
    return source_value;
  }

  std::size_t source_size = 0;
  const char* source = JS_ToCStringLen(context, &source_size, source_value);
  JS_FreeValue(context, source_value);
  if (source == nullptr) {
    return JS_EXCEPTION;
  }

  TranslationResult translation =
      unijit::frontend::quickjs::translate_numeric_function(
          std::string_view(source, source_size));
  JS_FreeCString(context, source);
  if (!translation.ok()) {
    return JS_ThrowTypeError(context, "UniJIT rejected source at byte %zu: %s",
                             translation.status.location(),
                             translation.status.message().c_str());
  }
  if (translation.parameter_count > kMaximumParameters) {
    return JS_ThrowTypeError(context, "compiled function has too many arguments");
  }
  if (!ensure_compiled_function_class(context)) {
    return JS_ThrowOutOfMemory(context);
  }

  JSValue holder =
      JS_NewObjectClass(context, static_cast<int>(compiled_function_class_id));
  if (JS_IsException(holder)) {
    return holder;
  }
  auto* owned = new (std::nothrow) OwnedFunction{
      translation.parameter_count, std::move(translation.function)};
  if (owned == nullptr) {
    JS_FreeValue(context, holder);
    return JS_ThrowOutOfMemory(context);
  }
  JS_SetOpaque(holder, owned);

  JSValue data[] = {holder};
  JSValue compiled = JS_NewCFunctionData(
      context, invoke_compiled_function,
      static_cast<int>(translation.parameter_count), 0, 1, data);
  JS_FreeValue(context, holder);
  return compiled;
}

JSValue compile_from_javascript(JSContext* context, JSValueConst, int argc,
                                JSValueConst* arguments, int,
                                JSValue* function_data) {
  if (argc < 1) {
    return JS_ThrowTypeError(context, "unijit.compile expects a function");
  }
  return compile_with_to_string(context, arguments[0], function_data[0]);
}

}  // namespace

extern "C" JSValue unijit_quickjs_compile(JSContext* context,
                                           JSValueConst function_value) {
  if (context == nullptr) {
    return JS_EXCEPTION;
  }
  JSValue to_string = get_function_to_string(context);
  if (JS_IsException(to_string)) {
    return to_string;
  }
  JSValue result =
      compile_with_to_string(context, function_value, to_string);
  JS_FreeValue(context, to_string);
  return result;
}

extern "C" int unijit_quickjs_install(JSContext* context) {
  if (context == nullptr) {
    return -1;
  }
  JSValue global = JS_GetGlobalObject(context);
  if (JS_IsException(global)) {
    return -1;
  }
  JSValue module = JS_NewObject(context);
  if (JS_IsException(module)) {
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue to_string = get_function_to_string(context);
  if (JS_IsException(to_string)) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  JSValue data[] = {to_string};
  JSValue compile = JS_NewCFunctionData(context, compile_from_javascript, 1, 0,
                                        1, data);
  JS_FreeValue(context, to_string);
  if (JS_IsException(compile)) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  if (JS_SetPropertyStr(context, module, "compile", compile) < 0) {
    JS_FreeValue(context, module);
    JS_FreeValue(context, global);
    return -1;
  }
  if (JS_SetPropertyStr(context, global, "unijit", module) < 0) {
    JS_FreeValue(context, global);
    return -1;
  }
  JS_FreeValue(context, global);
  return 0;
}
