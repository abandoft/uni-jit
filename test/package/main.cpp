#include <array>
#include <utility>

#include "unijit/ir/function.h"
#include "unijit/jit/compiler.h"

int main() {
  unijit::ir::FunctionBuilder builder(2);
  const auto sum = builder.add(builder.parameter(0), builder.parameter(1));
  if (!builder.set_return(sum).ok()) {
    return 1;
  }
  const auto function = std::move(builder).build();
  auto compilation = unijit::jit::Compiler::compile(function);
  if (!compilation.ok()) {
    return 2;
  }
  const std::array<unijit::ir::Word, 2> arguments = {20, 22};
  const auto result = compilation.function->invoke(arguments.data(),
                                                   arguments.size());
  return result.ok() && result.value == 42 ? 0 : 3;
}
