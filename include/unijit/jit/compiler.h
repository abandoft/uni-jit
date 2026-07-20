#ifndef UNIJIT_JIT_COMPILER_H
#define UNIJIT_JIT_COMPILER_H

#include <cstddef>
#include <memory>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/runtime/execution_context.h"
#include "unijit/status.h"

namespace unijit::jit {

using NativeEntry = ir::Word (*)(const ir::Word*, runtime::ExecutionContext*);

struct CompilationStats final {
  std::size_t code_size{0};
  std::size_t executable_mapping_size{0};
  std::size_t spill_slots{0};
  std::size_t input_ir_nodes{0};
  std::size_t optimized_ir_nodes{0};
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
  bool requires_context() const noexcept { return requires_context_; }

  const CompilationStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  friend class Compiler;

  CompiledFunction(std::unique_ptr<Impl> impl, std::size_t parameter_count,
                   CompilationStats stats, bool requires_context) noexcept;

  std::unique_ptr<Impl> impl_;
  std::size_t parameter_count_{0};
  CompilationStats stats_;
  bool requires_context_{false};
};

struct CompilationResult final {
  Status status;
  std::unique_ptr<CompiledFunction> function;

  bool ok() const noexcept { return status.ok() && function != nullptr; }
};

class Compiler final {
 public:
  static CompilationResult compile(const ir::Function& function);
  static CompilationResult compile(const ir::ControlFlowFunction& function);
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_COMPILER_H
