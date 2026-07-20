#ifndef UNIJIT_JIT_COMPILER_H
#define UNIJIT_JIT_COMPILER_H

#include <cstddef>
#include <memory>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/status.h"

namespace unijit::jit {

using NativeEntry = ir::Word (*)(const ir::Word*);

struct CompilationStats final {
  std::size_t code_size{0};
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

  ir::EvaluationResult invoke(const ir::Word* args,
                              std::size_t arg_count) const;

  ir::EvaluationResult invoke(const std::vector<ir::Word>& args) const {
    return invoke(args.data(), args.size());
  }

  NativeEntry native_entry() const noexcept;

  const CompilationStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  friend class Compiler;

  CompiledFunction(std::unique_ptr<Impl> impl, std::size_t parameter_count,
                   CompilationStats stats) noexcept;

  std::unique_ptr<Impl> impl_;
  std::size_t parameter_count_{0};
  CompilationStats stats_;
};

struct CompilationResult final {
  Status status;
  std::unique_ptr<CompiledFunction> function;

  bool ok() const noexcept { return status.ok() && function != nullptr; }
};

class Compiler final {
 public:
  static CompilationResult compile(const ir::Function& function);
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_COMPILER_H
