#include <array>
#include <cstdlib>
#include <iostream>

#include <pocketpy.h>

#include "source_translator.h"
#include "unijit/ir/function.h"
#include "unijit_pocketpy.h"

int main() {
  const auto translation =
      unijit::frontend::pocketpy::translate_numeric_function(
          "def affine(a, b):\n"
          "    return (a + 2.5) * (b - -3)\n");
  if (!translation.ok() || translation.parameter_count != 2) {
    std::cerr << "PocketPy numeric source did not compile: "
              << translation.status.message() << '\n';
    return EXIT_FAILURE;
  }

  const std::array<unijit::ir::Word, 2> arguments = {
      unijit::ir::pack_float64(1.5), unijit::ir::pack_float64(4.0)};
  const auto native =
      translation.function->invoke(arguments.data(), arguments.size());
  if (!native.ok() || unijit::ir::unpack_float64(native.value) != 28.0) {
    std::cerr << "PocketPy numeric source produced the wrong native result\n";
    return EXIT_FAILURE;
  }

  constexpr std::array<const char *, 7> kRejectedSources = {
      "lambda a: a + 1",
      "def f(a, a): return a",
      "def f(class): return class",
      "def f(a): return a / 2",
      "def f(a): return external + a",
      "def f(a):\n    b = a\n    return b",
      "def f(a): return a; return 0"};
  for (const char *source : kRejectedSources) {
    if (unijit::frontend::pocketpy::translate_numeric_function(source).ok()) {
      std::cerr << "unsupported PocketPy source was accepted: " << source
                << '\n';
      return EXIT_FAILURE;
    }
  }

  py_initialize();
  if (unijit_pocketpy_install() != 0 || unijit_pocketpy_install() != 0) {
    std::cerr << "unable to install the UniJIT PocketPy module\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  constexpr char kNativeSource[] =
      "import unijit\n"
      "native = unijit.compile(\"def affine(a, b): return (a + 2.5) * "
      "(b - -3)\")\n";
  if (!py_exec(kNativeSource, "<unijit-pocketpy-native>", EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  (void)py_gc_collect();
  if (!py_exec("result = native(1.5, 4)", "<unijit-pocketpy-call>",
               EXEC_MODE, nullptr)) {
    py_printexc();
    py_finalize();
    return EXIT_FAILURE;
  }
  const py_Ref result = py_getglobal(py_name("result"));
  if (result == nullptr || !py_isfloat(result) || py_tofloat(result) != 28.0) {
    std::cerr << "PocketPy did not execute the compiled native callable\n";
    py_finalize();
    return EXIT_FAILURE;
  }

  if (py_exec("native('1.5', 4)", "<unijit-pocketpy-guard>", EVAL_MODE,
              nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy native callable accepted a non-number argument\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("unijit._CompiledFunction()", "<unijit-pocketpy-constructor>",
              EVAL_MODE, nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy exposed unsafe direct native construction\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);

  if (py_exec("native(1.5)", "<unijit-pocketpy-arity>", EVAL_MODE, nullptr) ||
      !py_matchexc(tp_TypeError)) {
    std::cerr << "PocketPy native callable accepted the wrong arity\n";
    py_finalize();
    return EXIT_FAILURE;
  }
  py_clearexc(nullptr);
  py_finalize();

  std::cout << "PocketPy numeric source translator test passed\n";
  return EXIT_SUCCESS;
}
