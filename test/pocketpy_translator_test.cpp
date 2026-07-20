#include <array>
#include <cstdlib>
#include <iostream>

#include "source_translator.h"
#include "unijit/ir/function.h"

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

  std::cout << "PocketPy numeric source translator test passed\n";
  return EXIT_SUCCESS;
}
