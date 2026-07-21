#ifndef UNIJIT_IR_OPTIMIZER_H
#define UNIJIT_IR_OPTIMIZER_H

#include <cstddef>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::ir {

struct OptimizationStats final {
  std::size_t input_nodes{0};
  std::size_t output_nodes{0};
  std::size_t constants_folded{0};
  std::size_t algebraic_simplifications{0};
  std::size_t common_subexpressions{0};
  std::size_t branches_folded{0};
};

struct OptimizationExitState final {
  Value exit;
  std::vector<Value> live_values;
};

struct OptimizationResult final {
  Status status;
  Function function;
  OptimizationStats stats;
  std::vector<Value> value_mapping;

  bool ok() const noexcept { return status.ok(); }
  Value map(Value input) const noexcept {
    return input.valid() && input.id() < value_mapping.size()
               ? value_mapping[input.id()]
               : Value{};
  }
};

struct ControlFlowOptimizationResult final {
  Status status;
  ControlFlowFunction function;
  OptimizationStats stats;
  std::vector<Value> value_mapping;

  bool ok() const noexcept { return status.ok(); }
  Value map(Value input) const noexcept {
    return input.valid() && input.id() < value_mapping.size()
               ? value_mapping[input.id()]
               : Value{};
  }
};

class Optimizer final {
 public:
  static OptimizationResult run(
      const Function& function,
      const std::vector<OptimizationExitState>& exit_states = {});
  static ControlFlowOptimizationResult run(
      const ControlFlowFunction& function,
      const std::vector<OptimizationExitState>& exit_states = {});
};

}  // namespace unijit::ir

#endif  // UNIJIT_IR_OPTIMIZER_H
