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

struct DeoptimizationPreparation final {
  Status status;
  runtime::DeoptimizationTable table;
};

bool has_guard_site(const ir::Function& function, std::size_t site) noexcept {
  return std::any_of(function.nodes().begin(), function.nodes().end(),
                     [site](const ir::Node& node) {
                       return node.opcode == ir::Opcode::kGuardFloatNonzero &&
                              static_cast<std::size_t>(node.immediate) == site;
                     });
}

DeoptimizationPreparation prepare_deoptimization(
    const ir::Function& input, const ir::Function& optimized,
    const runtime::DeoptimizationTable& requested) {
  const Status validation = requested.validate(input.parameter_count());
  if (!validation.ok()) {
    return {validation, {}};
  }
  for (const runtime::DeoptimizationRecord& record : requested.records()) {
    if (!has_guard_site(input, record.site)) {
      return {{StatusCode::kInvalidArgument,
               "deoptimization metadata does not identify a runtime guard",
               record.site},
              {}};
    }
  }

  runtime::DeoptimizationTable result;
  try {
    for (const ir::Node& node : optimized.nodes()) {
      if (node.opcode != ir::Opcode::kGuardFloatNonzero) {
        continue;
      }
      const std::size_t site = static_cast<std::size_t>(node.immediate);
      if (result.find(site) != nullptr) {
        continue;
      }
      const runtime::DeoptimizationRecord* supplied = requested.find(site);
      if (supplied != nullptr) {
        const Status addition = result.add(*supplied);
        if (!addition.ok()) {
          return {addition, {}};
        }
        continue;
      }

      runtime::DeoptimizationRecord fallback;
      fallback.site = site;
      fallback.resume_offset = site;
      fallback.reason = runtime::DeoptimizationReason::kGuardFailed;
      fallback.recovery.push_back(runtime::RecoveryOperation::exit_value(
          input.parameter_count(), ir::ValueType::kFloat64));
      const Status addition = result.add(fallback);
      if (!addition.ok()) {
        return {addition, {}};
      }
    }
    return {Status::ok_status(), std::move(result)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare deoptimization metadata"},
            {}};
  }
}

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
                                   bool requires_context,
                                   runtime::DeoptimizationTable
                                       deoptimization_table) noexcept
    : impl_(std::move(impl)),
      parameter_count_(parameter_count),
      stats_(stats),
      requires_context_(requires_context),
      deoptimization_table_(std::move(deoptimization_table)) {}

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
  return compile(function, {});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table) {
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }

  ir::OptimizationResult optimization = ir::Optimizer::run(function);
  if (!optimization.ok()) {
    return {optimization.status, nullptr};
  }
  const ir::Function& optimized = optimization.function;

  DeoptimizationPreparation deoptimization =
      prepare_deoptimization(function, optimized, deoptimization_table);
  if (!deoptimization.status.ok()) {
    return {deoptimization.status, nullptr};
  }

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

    CompilationStats stats{lowering.code.size(),
                           implementation->memory.mapping_size(),
                           lowering.spill_slots, function.nodes().size(),
                           optimized.nodes().size()};
    const bool requires_context = std::any_of(
        optimized.nodes().begin(), optimized.nodes().end(),
        [](const ir::Node& node) {
          return node.opcode == ir::Opcode::kGuardFloatNonzero;
        });
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats,
        requires_context, std::move(deoptimization.table)));
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

    CompilationStats stats{lowering.code.size(),
                           implementation->memory.mapping_size(),
                           lowering.spill_slots, function.nodes().size(),
                           function.nodes().size()};
    const bool requires_context = std::any_of(
        function.nodes().begin(), function.nodes().end(),
        [](const ir::ControlNode& node) {
          return node.opcode == ir::ControlOpcode::kSafepoint;
        });
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats,
        requires_context, {}));
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
