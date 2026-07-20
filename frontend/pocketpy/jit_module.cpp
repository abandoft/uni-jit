#include "unijit_pocketpy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include <pocketpy.h>

#include "source_translator.h"
#include "unijit/ir/function.h"

namespace {

using unijit::frontend::pocketpy::TranslationResult;
using unijit::ir::Word;

constexpr char kModuleName[] = "unijit";
constexpr char kCompiledFunctionTypeName[] = "_CompiledFunction";
constexpr std::size_t kMaximumParameters = 64;

struct OwnedFunction final {
  std::size_t parameter_count{0};
  std::unique_ptr<unijit::jit::CompiledFunction> function;
};

static_assert(alignof(OwnedFunction) <= alignof(std::uint64_t),
              "PocketPy userdata must provide sufficient alignment");

py_Type compiled_function_type() {
  return py_gettype(kModuleName, py_name(kCompiledFunctionTypeName));
}

void destroy_compiled_function(void *userdata) {
  auto *owned = static_cast<OwnedFunction *>(userdata);
  owned->~OwnedFunction();
}

bool reject_direct_construction(int, py_Ref) {
  return TypeError("UniJIT compiled functions cannot be constructed directly");
}

bool create_compiled_function(std::string_view source) {
  TranslationResult translation =
      unijit::frontend::pocketpy::translate_numeric_function(source);
  if (!translation.ok()) {
    return ValueError("UniJIT rejected source at byte %d: %s",
                      static_cast<int>(translation.status.location()),
                      translation.status.message().c_str());
  }
  if (translation.parameter_count > kMaximumParameters) {
    return ValueError("compiled function has too many arguments");
  }

  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  void *storage =
      py_newobject(py_retval(), type, 0, sizeof(OwnedFunction));
  ::new (storage) OwnedFunction{translation.parameter_count,
                                std::move(translation.function)};
  return true;
}

bool invoke_compiled_function(int argc, py_Ref argv) {
  const py_Type type = compiled_function_type();
  if (type == 0) {
    return RuntimeError("UniJIT is not installed in the current VM");
  }
  if (argc < 1) {
    return TypeError("compiled function call is missing self");
  }
  if (!py_checktype(py_arg(0), type)) {
    return false;
  }
  auto *owned = static_cast<OwnedFunction *>(py_touserdata(py_arg(0)));
  if (owned->function == nullptr) {
    return RuntimeError("invalid UniJIT compiled function");
  }

  const int argument_count = argc - 1;
  if (argument_count != static_cast<int>(owned->parameter_count)) {
    return TypeError("compiled function expects %d arguments, got %d",
                     static_cast<int>(owned->parameter_count), argument_count);
  }

  std::array<Word, kMaximumParameters> native_arguments{};
  for (std::size_t index = 0; index < owned->parameter_count; ++index) {
    double number = 0.0;
    if (!py_castfloat(py_arg(static_cast<int>(index) + 1), &number)) {
      return false;
    }
    native_arguments[index] = unijit::ir::pack_float64(number);
  }

  Word result = 0;
  if (owned->function->requires_context()) {
    const unijit::ir::EvaluationResult invocation = owned->function->invoke(
        native_arguments.data(), owned->parameter_count);
    if (!invocation.ok()) {
      if (invocation.status.code() == unijit::StatusCode::kRuntimeExit) {
        return ZeroDivisionError("float division by zero");
      }
      return RuntimeError("UniJIT invocation failed at site %d: %s",
                          static_cast<int>(invocation.status.location()),
                          invocation.status.message().c_str());
    }
    result = invocation.value;
  } else {
    result =
        owned->function->native_entry()(native_arguments.data(), nullptr);
  }
  py_newfloat(py_retval(), unijit::ir::unpack_float64(result));
  return true;
}

bool compile_from_python(int argc, py_Ref argv) {
  if (argc != 1) {
    return TypeError("unijit.compile() expects 1 argument, got %d", argc);
  }
  if (!py_checkstr(py_arg(0))) {
    return false;
  }
  int source_size = 0;
  const char *source = py_tostrn(py_arg(0), &source_size);
  return create_compiled_function(
      std::string_view(source, static_cast<std::size_t>(source_size)));
}

} // namespace

extern "C" bool unijit_pocketpy_compile(const char *source) {
  if (source == nullptr) {
    return TypeError("unijit_pocketpy_compile() expects non-null source");
  }
  return create_compiled_function(source);
}

extern "C" int unijit_pocketpy_install(void) {
  py_GlobalRef module = py_getmodule(kModuleName);
  if (module != nullptr) {
    const py_Ref compile = py_getdict(module, py_name("compile"));
    return compile != nullptr && compiled_function_type() != 0 ? 0 : -1;
  }

  module = py_newmodule(kModuleName);
  const py_Type type = py_newtype(kCompiledFunctionTypeName, tp_object, module,
                                  destroy_compiled_function);
  py_bindmagic(type, py_name("__new__"), reject_direct_construction);
  py_bindmagic(type, py_name("__call__"), invoke_compiled_function);
  py_tpsetfinal(type);
  py_bindfunc(module, "compile", compile_from_python);
  return 0;
}
