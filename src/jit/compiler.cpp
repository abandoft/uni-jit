#include "unijit/jit/compiler.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "jit/executable_memory.h"
#include "unijit/ir/optimizer.h"

#if defined(UNIJIT_TARGET_AARCH64)
#include "jit/backend/aarch64/lower.h"
#elif defined(UNIJIT_TARGET_X86_64)
#include "jit/backend/x86_64/lower.h"
#elif defined(UNIJIT_TARGET_RISCV64)
#include "jit/backend/riscv64/lower.h"
#endif

namespace unijit::jit {
namespace {

ir::EvaluationResult finish_invocation(
    ir::Word value, runtime::ExecutionContext* context) {
  if (context == nullptr ||
      context->exit_reason() == runtime::ExitReason::kNone) {
    return {Status::ok_status(), value};
  }
  if (context->exit_reason() == runtime::ExitReason::kSafepoint) {
    return {{StatusCode::kExecutionInterrupted,
             "execution interrupted at a safepoint", context->exit_site()},
            value};
  }
  return {{StatusCode::kRuntimeExit,
           "compiled execution requested a runtime exit", context->exit_site()},
          value};
}

}  // namespace

struct CompiledFunction::Impl final {
  detail::ExecutableMemory memory;

  NativeEntry entry() const noexcept {
    static_assert(sizeof(NativeEntry) == sizeof(void*),
                  "native entry and data pointers must have equal size");
    NativeEntry result = nullptr;
    void* address = memory.address();
    std::memcpy(&result, &address, sizeof(result));
    return result;
  }
};

CompiledFunction::CompiledFunction(std::unique_ptr<Impl> impl,
                                   std::size_t parameter_count,
                                   CompilationStats stats,
                                   bool requires_context) noexcept
    : impl_(std::move(impl)),
      parameter_count_(parameter_count),
      stats_(stats),
      requires_context_(requires_context) {}

CompiledFunction::~CompiledFunction() = default;
CompiledFunction::CompiledFunction(CompiledFunction&&) noexcept = default;
CompiledFunction& CompiledFunction::operator=(CompiledFunction&&) noexcept =
    default;

NativeEntry CompiledFunction::native_entry() const noexcept {
  return impl_ == nullptr ? nullptr : impl_->entry();
}

ir::EvaluationResult CompiledFunction::invoke(
    const ir::Word* args, std::size_t arg_count,
    runtime::ExecutionContext* context) const {
  if (arg_count != parameter_count_) {
    return {{StatusCode::kInvalidArgument,
             "argument count does not match compiled function signature"},
            0};
  }
  if (arg_count != 0 && args == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "argument storage is null for a non-empty signature"},
            0};
  }
  const NativeEntry entry = native_entry();
  if (entry == nullptr) {
    return {{StatusCode::kCodeGenerationFailed,
             "compiled function has no published entry point"},
            0};
  }
  if (context == nullptr && requires_context_) {
    runtime::ExecutionContext local_context;
    return finish_invocation(entry(args, &local_context), &local_context);
  }
  if (context != nullptr) {
    context->clear_exit();
  }
  return finish_invocation(entry(args, context), context);
}

CompilationResult Compiler::compile(const ir::Function& function) {
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }

  ir::OptimizationResult optimization = ir::Optimizer::run(function);
  if (!optimization.ok()) {
    return {optimization.status, nullptr};
  }
  const ir::Function& optimized = optimization.function;

#if defined(UNIJIT_TARGET_AARCH64)
  detail::aarch64::LoweringResult lowering = detail::aarch64::lower(optimized);
#elif defined(UNIJIT_TARGET_X86_64)
  detail::x86_64::LoweringResult lowering = detail::x86_64::lower(optimized);
#elif defined(UNIJIT_TARGET_RISCV64)
  detail::riscv64::LoweringResult lowering = detail::riscv64::lower(optimized);
#endif

#if defined(UNIJIT_TARGET_AARCH64) || defined(UNIJIT_TARGET_X86_64) || \
    defined(UNIJIT_TARGET_RISCV64)
  if (!lowering.status.ok()) {
    return {lowering.status, nullptr};
  }

  try {
    auto implementation = std::make_unique<CompiledFunction::Impl>();
    const Status publication = detail::ExecutableMemory::publish(
        lowering.code.data(), lowering.code.size(), &implementation->memory);
    if (!publication.ok()) {
      return {publication, nullptr};
    }

    CompilationStats stats{lowering.code.size(), lowering.spill_slots,
                           function.nodes().size(), optimized.nodes().size()};
    const bool requires_context = std::any_of(
        optimized.nodes().begin(), optimized.nodes().end(),
        [](const ir::Node& node) {
          return node.opcode == ir::Opcode::kGuardFloatNonzero;
        });
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats,
        requires_context));
    return {Status::ok_status(), std::move(compiled)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate compiled-function metadata"},
            nullptr};
  }
#else
  (void)function;
  return {{StatusCode::kUnsupportedArchitecture,
           "UniJIT has no native backend for this architecture yet"},
          nullptr};
#endif
}

CompilationResult Compiler::compile(const ir::ControlFlowFunction& function) {
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }

#if defined(UNIJIT_TARGET_AARCH64)
  detail::aarch64::LoweringResult lowering = detail::aarch64::lower(function);
#elif defined(UNIJIT_TARGET_X86_64)
  detail::x86_64::LoweringResult lowering = detail::x86_64::lower(function);
#elif defined(UNIJIT_TARGET_RISCV64)
  detail::riscv64::LoweringResult lowering = detail::riscv64::lower(function);
#endif

#if defined(UNIJIT_TARGET_AARCH64) || defined(UNIJIT_TARGET_X86_64) || \
    defined(UNIJIT_TARGET_RISCV64)
  if (!lowering.status.ok()) {
    return {lowering.status, nullptr};
  }

  try {
    auto implementation = std::make_unique<CompiledFunction::Impl>();
    const Status publication = detail::ExecutableMemory::publish(
        lowering.code.data(), lowering.code.size(), &implementation->memory);
    if (!publication.ok()) {
      return {publication, nullptr};
    }

    CompilationStats stats{lowering.code.size(), lowering.spill_slots,
                           function.nodes().size(), function.nodes().size()};
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats, false));
    return {Status::ok_status(), std::move(compiled)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate compiled CFG function metadata"},
            nullptr};
  }
#else
  (void)function;
  return {{StatusCode::kUnsupportedArchitecture,
           "UniJIT has no native backend for this architecture yet"},
          nullptr};
#endif
}

}  // namespace unijit::jit
