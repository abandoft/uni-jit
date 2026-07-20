#ifndef UNIJIT_IR_OPTIMIZER_H
#define UNIJIT_IR_OPTIMIZER_H

#include <cstddef>

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::ir {

struct OptimizationStats final {
  std::size_t input_nodes{0};
  std::size_t output_nodes{0};
  std::size_t constants_folded{0};
  std::size_t algebraic_simplifications{0};
};

struct OptimizationResult final {
  Status status;
  Function function;
  OptimizationStats stats;

  bool ok() const noexcept { return status.ok(); }
};

class Optimizer final {
 public:
  static OptimizationResult run(const Function& function);
};

}  // namespace unijit::ir

#endif  // UNIJIT_IR_OPTIMIZER_H
