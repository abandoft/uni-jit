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

template <typename FunctionType>
Status append_assumption_deoptimization(
    const FunctionType& function,
    const runtime::AssumptionSet& assumptions,
    const runtime::DeoptimizationTable& requested,
    runtime::DeoptimizationTable* result) {
  const runtime::AssumptionDependency* invalid = assumptions.first_invalid();
  if (invalid != nullptr) {
    return {StatusCode::kInvalidArgument,
            "cannot compile against an invalidated assumption",
            invalid->site};
  }
  for (const runtime::AssumptionDependency& dependency :
       assumptions.dependencies()) {
    const runtime::DeoptimizationRecord* supplied =
        requested.find(dependency.site);
    if (supplied != nullptr) {
      if (supplied->reason !=
          runtime::DeoptimizationReason::kAssumptionInvalidated) {
        return {StatusCode::kInvalidArgument,
                "assumption exit metadata has the wrong semantic reason",
                dependency.site};
      }
      const Status addition = result->add(*supplied);
      if (!addition.ok()) {
        return addition;
      }
      continue;
    }

    runtime::DeoptimizationRecord fallback;
    fallback.site = dependency.site;
    fallback.resume_offset = dependency.resume_offset;
    fallback.reason =
        runtime::DeoptimizationReason::kAssumptionInvalidated;
    for (std::size_t index = 0; index < function.parameter_count(); ++index) {
      fallback.recovery.push_back(runtime::RecoveryOperation::argument(
          index, function.parameter_type(index), index));
    }
    const Status addition = result->add(fallback);
    if (!addition.ok()) {
      return addition;
    }
  }
  return Status::ok_status();
}

DeoptimizationPreparation prepare_deoptimization(
    const ir::Function& input, const ir::Function& optimized,
    const runtime::DeoptimizationTable& requested,
    const runtime::AssumptionSet& assumptions) {
  const Status validation = requested.validate(input.parameter_count());
  if (!validation.ok()) {
    return {validation, {}};
  }
  for (const runtime::DeoptimizationRecord& record : requested.records()) {
    if (!has_guard_site(input, record.site) &&
        assumptions.find(record.site) == nullptr) {
      return {{StatusCode::kInvalidArgument,
               "deoptimization metadata does not identify an exit site",
               record.site},
              {}};
    }
  }
  for (const runtime::AssumptionDependency& dependency :
       assumptions.dependencies()) {
    if (has_guard_site(input, dependency.site)) {
      return {{StatusCode::kInvalidArgument,
               "assumption and runtime guard sites must be distinct",
               dependency.site},
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
    const Status assumptions_status = append_assumption_deoptimization(
        input, assumptions, requested, &result);
    if (!assumptions_status.ok()) {
      return {assumptions_status, {}};
    }
    return {Status::ok_status(), std::move(result)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare deoptimization metadata"},
            {}};
  }
}

DeoptimizationPreparation prepare_deoptimization(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& requested,
    const runtime::AssumptionSet& assumptions) {
  const Status validation = requested.validate(function.parameter_count());
  if (!validation.ok()) {
    return {validation, {}};
  }
  for (const runtime::DeoptimizationRecord& record : requested.records()) {
    if (assumptions.find(record.site) == nullptr) {
      return {{StatusCode::kInvalidArgument,
               "CFG deoptimization metadata does not identify an exit site",
               record.site},
              {}};
    }
  }

  runtime::DeoptimizationTable result;
  try {
    const Status assumptions_status = append_assumption_deoptimization(
        function, assumptions, requested, &result);
    if (!assumptions_status.ok()) {
      return {assumptions_status, {}};
    }
    return {Status::ok_status(), std::move(result)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare CFG deoptimization metadata"},
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
                                       deoptimization_table,
                                   runtime::AssumptionSet assumptions) noexcept
    : impl_(std::move(impl)),
      parameter_count_(parameter_count),
      stats_(stats),
      requires_context_(requires_context),
      deoptimization_table_(std::move(deoptimization_table)),
      assumptions_(std::move(assumptions)) {}

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
  runtime::ExecutionContext local_context;
  runtime::ExecutionContext* active_context = context;
  if (active_context == nullptr && requires_context_) {
    active_context = &local_context;
  }
  if (active_context != nullptr) {
    active_context->clear_exit();
    active_context->clear_deoptimization_wakeup();
  }
  if (!assumptions_.empty()) {
    runtime::AssumptionActivation activation =
        assumptions_.activate(active_context);
    if (!activation.status().ok()) {
      return {activation.status(), 0};
    }
    const runtime::AssumptionDependency* invalid =
        activation.invalid_dependency();
    if (invalid != nullptr) {
      active_context->record_exit(runtime::ExitReason::kRuntime,
                                  invalid->site);
      return finish_invocation(0, active_context);
    }

    const ir::Word value = entry(args, active_context);
    invalid = assumptions_.first_invalid();
    if (invalid != nullptr) {
      active_context->clear_deoptimization_wakeup();
      active_context->record_exit(runtime::ExitReason::kRuntime,
                                  invalid->site);
      return finish_invocation(0, active_context);
    }
    return finish_invocation(value, active_context);
  }
  return finish_invocation(entry(args, active_context), active_context);
}

CompilationResult Compiler::compile(const ir::Function& function) {
  return compile(function, runtime::DeoptimizationTable{},
                 runtime::AssumptionSet{});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table) {
  return compile(function, deoptimization_table, runtime::AssumptionSet{});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, runtime::DeoptimizationTable{}, assumptions);
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions) {
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
      prepare_deoptimization(function, optimized, deoptimization_table,
                             assumptions);
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
    const bool requires_context = !assumptions.empty() || std::any_of(
        optimized.nodes().begin(), optimized.nodes().end(),
        [](const ir::Node& node) {
          return node.opcode == ir::Opcode::kGuardFloatNonzero;
        });
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats,
        requires_context, std::move(deoptimization.table), assumptions));
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
  return compile(function, runtime::DeoptimizationTable{},
                 runtime::AssumptionSet{});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, runtime::DeoptimizationTable{}, assumptions);
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions) {
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }

  DeoptimizationPreparation deoptimization =
      prepare_deoptimization(function, deoptimization_table, assumptions);
  if (!deoptimization.status.ok()) {
    return {deoptimization.status, nullptr};
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
    const bool requires_context = !assumptions.empty() || std::any_of(
        function.nodes().begin(), function.nodes().end(),
        [](const ir::ControlNode& node) {
          return node.opcode == ir::ControlOpcode::kSafepoint;
        });
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats,
        requires_context, std::move(deoptimization.table), assumptions));
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
