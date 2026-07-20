#ifndef UNIJIT_JIT_COMPILER_H
#define UNIJIT_JIT_COMPILER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/jit/stack_map.h"
#include "unijit/runtime/assumption.h"
#include "unijit/runtime/deoptimization.h"
#include "unijit/runtime/execution_context.h"
#include "unijit/runtime/materialization.h"
#include "unijit/runtime/on_stack_replacement.h"
#include "unijit/status.h"

namespace unijit::jit {

using NativeEntry = ir::Word (*)(const ir::Word*, runtime::ExecutionContext*);

struct CompilationStats final {
  std::size_t code_size{0};
  std::size_t executable_mapping_size{0};
  std::size_t spill_slots{0};
  std::size_t input_ir_nodes{0};
  std::size_t optimized_ir_nodes{0};
  std::size_t stack_map_count{0};
  std::size_t stack_map_value_count{0};
};

enum class OptimizationLevel : std::uint8_t {
  kBaseline = 0,
  kOptimized,
};

struct CompilationLimits final {
  std::size_t maximum_parameters{256};
  std::size_t maximum_ir_nodes{64U * 1024U};
  std::size_t maximum_cfg_blocks{1024};
  std::size_t maximum_ir_arguments{256U * 1024U};
  std::size_t maximum_stack_maps{4096};
  std::size_t maximum_metadata_values{256U * 1024U};
  std::size_t maximum_code_bytes{16U * 1024U * 1024U};
};

struct CompilationOptions final {
  CompilationOptions() noexcept = default;
  explicit CompilationOptions(
      OptimizationLevel level,
      CompilationLimits configured_limits = {}) noexcept
      : optimization_level(level), limits(configured_limits) {}

  OptimizationLevel optimization_level{OptimizationLevel::kOptimized};
  CompilationLimits limits;
};

class CompiledFunction final {
 public:
  ~CompiledFunction();
  CompiledFunction(CompiledFunction&&) noexcept;
  CompiledFunction& operator=(CompiledFunction&&) noexcept;

  CompiledFunction(const CompiledFunction&) = delete;
  CompiledFunction& operator=(const CompiledFunction&) = delete;

  ir::EvaluationResult invoke(
      const ir::Word* args, std::size_t arg_count,
      runtime::ExecutionContext* context = nullptr) const;

  ir::EvaluationResult invoke(
      const std::vector<ir::Word>& args,
      runtime::ExecutionContext* context = nullptr) const {
    return invoke(args.data(), args.size(), context);
  }

  NativeEntry native_entry() const noexcept;
  std::size_t parameter_count() const noexcept { return parameter_count_; }
  ir::ValueType parameter_type(std::size_t index) const noexcept {
    return index < parameter_types_.size() ? parameter_types_[index]
                                           : ir::ValueType::kWord;
  }
  ir::ValueType return_type() const noexcept { return return_type_; }
  bool requires_context() const noexcept { return requires_context_; }
  const runtime::AssumptionSet& assumptions() const noexcept {
    return assumptions_;
  }
  const StackMapTable& stack_maps() const noexcept { return stack_maps_; }
  const StackMapRecord* stack_map(std::size_t site) const noexcept {
    return stack_maps_.find(site);
  }
  StackMapCaptureResult reconstruct_stack_map(
      const runtime::ExecutionContext& context) const;

  const runtime::DeoptimizationTable& deoptimization_table() const noexcept {
    return deoptimization_table_;
  }
  const runtime::DeoptimizationRecord* deoptimization_record(
      std::size_t site) const noexcept {
    return deoptimization_table_.find(site);
  }
  runtime::ReconstructionResult reconstruct_deoptimization(
      std::size_t site, const ir::Word* args, std::size_t arg_count,
      const runtime::ExecutionContext& context) const;
  runtime::MaterializationResult materialize_deoptimization(
      std::size_t site, const ir::Word* args, std::size_t arg_count,
      const runtime::ExecutionContext& context,
      const runtime::MaterializationPlan& plan,
      const runtime::MaterializationCallbacks& callbacks) const;
  runtime::OsrEntryResult enter_osr(
      const runtime::OsrFrame& frame, const runtime::OsrEntryPlan& plan,
      runtime::ExecutionContext* context = nullptr) const;

  const CompilationStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  friend class Compiler;

  CompiledFunction(std::unique_ptr<Impl> impl,
                   std::vector<ir::ValueType> parameter_types,
                   ir::ValueType return_type, CompilationStats stats,
                   bool requires_context,
                   runtime::DeoptimizationTable deoptimization_table,
                   runtime::AssumptionSet assumptions,
                   StackMapTable stack_maps) noexcept;

  std::unique_ptr<Impl> impl_;
  std::size_t parameter_count_{0};
  std::vector<ir::ValueType> parameter_types_;
  ir::ValueType return_type_{ir::ValueType::kWord};
  CompilationStats stats_;
  bool requires_context_{false};
  runtime::DeoptimizationTable deoptimization_table_;
  runtime::AssumptionSet assumptions_;
  StackMapTable stack_maps_;
};

struct CompilationResult final {
  Status status;
  std::unique_ptr<CompiledFunction> function;

  bool ok() const noexcept { return status.ok() && function != nullptr; }
};

class Compiler final {
 public:
  static CompilationResult compile(const ir::Function& function);
  static CompilationResult compile(const ir::Function& function,
                                   CompilationOptions options);
  static CompilationResult compile(
      const ir::Function& function,
      const runtime::DeoptimizationTable& deoptimization_table);
  static CompilationResult compile(
      const ir::Function& function,
      const runtime::AssumptionSet& assumptions);
  static CompilationResult compile(
      const ir::Function& function,
      const runtime::DeoptimizationTable& deoptimization_table,
      const runtime::AssumptionSet& assumptions);
  static CompilationResult compile(
      const ir::Function& function,
      const runtime::DeoptimizationTable& deoptimization_table,
      const runtime::AssumptionSet& assumptions,
      CompilationOptions options);
  static CompilationResult compile(const ir::ControlFlowFunction& function);
  static CompilationResult compile(const ir::ControlFlowFunction& function,
                                   CompilationLimits limits);
  static CompilationResult compile(
      const ir::ControlFlowFunction& function,
      const runtime::DeoptimizationTable& deoptimization_table);
  static CompilationResult compile(
      const ir::ControlFlowFunction& function,
      const runtime::AssumptionSet& assumptions);
  static CompilationResult compile(
      const ir::ControlFlowFunction& function,
      const runtime::DeoptimizationTable& deoptimization_table,
      const runtime::AssumptionSet& assumptions);
  static CompilationResult compile(
      const ir::ControlFlowFunction& function,
      const runtime::DeoptimizationTable& deoptimization_table,
      const runtime::AssumptionSet& assumptions, CompilationLimits limits);
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_COMPILER_H
