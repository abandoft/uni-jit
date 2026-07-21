#include "unijit/jit/compiler.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "jit/executable_memory.h"
#include "jit/stack_map_requirements.h"
#include "unijit/ir/optimizer.h"
#include "ir/object_access.h"

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

struct OptimizationExitStatePreparation final {
  Status status;
  std::vector<ir::OptimizationExitState> exit_states;
};

struct CaptureBinding final {
  std::size_t site{0};
  ir::Value source;
  ir::Value lowered;
};

struct StackMapRequirementPreparation final {
  Status status;
  detail::StackMapRequirements requirements;
  std::vector<CaptureBinding> bindings;
};

bool is_nonzero_guard(ir::Opcode opcode) noexcept {
  return opcode == ir::Opcode::kGuardWordNonzero ||
         opcode == ir::Opcode::kGuardFloatNonzero;
}

bool is_memory(ir::Opcode opcode) noexcept {
  return opcode == ir::Opcode::kLoadWord || opcode == ir::Opcode::kStoreWord ||
         opcode == ir::Opcode::kLoadFloat ||
         opcode == ir::Opcode::kStoreFloat ||
         opcode == ir::Opcode::kLoadVector ||
         opcode == ir::Opcode::kStoreVector ||
         opcode == ir::Opcode::kAtomicLoad ||
         opcode == ir::Opcode::kAtomicStore ||
         opcode == ir::Opcode::kAtomicExchange ||
         opcode == ir::Opcode::kAtomicCompareExchange ||
         opcode == ir::Opcode::kAtomicFetchAdd ||
         opcode == ir::Opcode::kAtomicFetchAnd ||
         opcode == ir::Opcode::kAtomicFetchOr ||
         opcode == ir::Opcode::kAtomicFetchXor;
}

bool is_nonzero_guard(ir::ControlOpcode opcode) noexcept {
  return opcode == ir::ControlOpcode::kGuardWordNonzero ||
         opcode == ir::ControlOpcode::kGuardFloatNonzero;
}

bool is_memory(ir::ControlOpcode opcode) noexcept {
  return opcode == ir::ControlOpcode::kLoadWord ||
         opcode == ir::ControlOpcode::kStoreWord ||
         opcode == ir::ControlOpcode::kLoadFloat ||
         opcode == ir::ControlOpcode::kStoreFloat ||
         opcode == ir::ControlOpcode::kLoadVector ||
         opcode == ir::ControlOpcode::kStoreVector ||
         opcode == ir::ControlOpcode::kAtomicLoad ||
         opcode == ir::ControlOpcode::kAtomicStore ||
         opcode == ir::ControlOpcode::kAtomicExchange ||
         opcode == ir::ControlOpcode::kAtomicCompareExchange ||
         opcode == ir::ControlOpcode::kAtomicFetchAdd ||
         opcode == ir::ControlOpcode::kAtomicFetchAnd ||
         opcode == ir::ControlOpcode::kAtomicFetchOr ||
         opcode == ir::ControlOpcode::kAtomicFetchXor;
}

bool has_guard_site(const ir::Function& function, std::size_t site) noexcept {
  return std::any_of(function.nodes().begin(), function.nodes().end(),
                     [site](const ir::Node& node) {
                       return is_nonzero_guard(node.opcode) &&
                              static_cast<std::size_t>(node.immediate) == site;
                     });
}

bool has_guard_site(const ir::ControlFlowFunction& function,
                    std::size_t site) noexcept {
  return std::any_of(function.nodes().begin(), function.nodes().end(),
                     [site](const ir::ControlNode& node) {
                       return is_nonzero_guard(node.opcode) &&
                              static_cast<std::size_t>(node.immediate) == site;
                     });
}

bool has_runtime_exit_site(const ir::Function& function,
                           std::size_t site) noexcept {
  return std::any_of(function.nodes().begin(), function.nodes().end(),
                     [site](const ir::Node& node) {
                       return (is_nonzero_guard(node.opcode) ||
                               is_memory(node.opcode) ||
                               node.opcode == ir::Opcode::kSafepoint) &&
                              static_cast<std::size_t>(node.immediate) == site;
                     });
}

bool has_runtime_exit_site(const ir::ControlFlowFunction& function,
                           std::size_t site) noexcept {
  return std::any_of(function.nodes().begin(), function.nodes().end(),
                     [site](const ir::ControlNode& node) {
                       return (is_nonzero_guard(node.opcode) ||
                               is_memory(node.opcode) ||
                               node.opcode ==
                                   ir::ControlOpcode::kSafepoint) &&
                              static_cast<std::size_t>(node.immediate) == site;
                     });
}

ir::Value guard_value(const ir::Function& function,
                      std::size_t site) noexcept {
  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::Node& node = function.nodes()[index];
    if (is_nonzero_guard(node.opcode) &&
        static_cast<std::size_t>(node.immediate) == site) {
      return ir::Value{static_cast<std::uint32_t>(index)};
    }
  }
  return {};
}

ir::Value guard_value(const ir::ControlFlowFunction& function,
                      std::size_t site) noexcept {
  for (std::size_t index = 0; index < function.nodes().size(); ++index) {
    const ir::ControlNode& node = function.nodes()[index];
    if (is_nonzero_guard(node.opcode) &&
        static_cast<std::size_t>(node.immediate) == site) {
      return ir::Value{static_cast<std::uint32_t>(index)};
    }
  }
  return {};
}

bool control_value_available(const ir::ControlFlowFunction& function,
                             ir::Value value, ir::Value exit) {
  if (!value.valid() || value.id() >= function.nodes().size() ||
      !exit.valid() || exit.id() >= function.nodes().size()) {
    return false;
  }

  const std::size_t block_count = function.blocks().size();
  const std::size_t no_owner = block_count;
  std::vector<std::size_t> owners(function.nodes().size(), no_owner);
  std::vector<std::size_t> positions(function.nodes().size(), 0);
  std::vector<std::vector<std::size_t>> predecessors(block_count);
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    const ir::BasicBlock& block = function.blocks()[block_index];
    for (std::size_t position = 0; position < block.instructions.size();
         ++position) {
      const ir::Value instruction = block.instructions[position];
      owners[instruction.id()] = block_index;
      positions[instruction.id()] = position;
    }
    const auto note_edge = [&](const ir::ControlEdge& edge) {
      predecessors[edge.target.id()].push_back(block_index);
    };
    if (block.terminator.opcode == ir::TerminatorOpcode::kJump) {
      note_edge(block.terminator.true_edge);
    } else if (block.terminator.opcode == ir::TerminatorOpcode::kBranch) {
      note_edge(block.terminator.true_edge);
      note_edge(block.terminator.false_edge);
    }
  }

  const std::size_t value_block = owners[value.id()];
  const std::size_t exit_block = owners[exit.id()];
  if (value_block == no_owner || exit_block == no_owner) {
    return false;
  }
  if (value_block == exit_block) {
    return positions[value.id()] < positions[exit.id()];
  }

  std::vector<std::vector<bool>> dominates(
      block_count, std::vector<bool>(block_count, true));
  std::fill(dominates[0].begin(), dominates[0].end(), false);
  dominates[0][0] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t block_index = 1; block_index < block_count;
         ++block_index) {
      std::vector<bool> next(block_count, true);
      for (const std::size_t predecessor : predecessors[block_index]) {
        for (std::size_t candidate = 0; candidate < block_count;
             ++candidate) {
          next[candidate] =
              next[candidate] && dominates[predecessor][candidate];
        }
      }
      next[block_index] = true;
      if (next != dominates[block_index]) {
        dominates[block_index] = std::move(next);
        changed = true;
      }
    }
  }
  return dominates[exit_block][value_block];
}

OptimizationExitStatePreparation prepare_optimization_exit_states(
    const ir::Function& function,
    const runtime::DeoptimizationTable& requested) {
  try {
    std::vector<ir::OptimizationExitState> exit_states;
    for (const runtime::DeoptimizationRecord& record : requested.records()) {
      ir::OptimizationExitState exit_state;
      exit_state.exit = guard_value(function, record.site);
      for (const runtime::RecoveryOperation& operation : record.recovery) {
        if (operation.source != runtime::RecoverySource::kCapturedValue) {
          continue;
        }
        if (!exit_state.exit.valid()) {
          return {{StatusCode::kInvalidArgument,
                   "captured recovery values require a native guard site",
                   record.site},
                  {}};
        }
        if (!operation.source_value.valid() ||
            operation.source_value.id() >= exit_state.exit.id()) {
          return {{StatusCode::kInvalidArgument,
                   "captured recovery value is unavailable at its guard",
                   record.site},
                  {}};
        }
        if (function.value_type(operation.source_value) != operation.type) {
          return {{StatusCode::kInvalidArgument,
                   "captured recovery value type does not match SSA",
                   record.site},
                  {}};
        }
        if (std::find(exit_state.live_values.begin(),
                      exit_state.live_values.end(), operation.source_value) ==
            exit_state.live_values.end()) {
          exit_state.live_values.push_back(operation.source_value);
        }
      }
      if (!exit_state.live_values.empty()) {
        exit_states.push_back(std::move(exit_state));
      }
    }
    return {Status::ok_status(), std::move(exit_states)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare optimizer exit state"},
            {}};
  }
}

OptimizationExitStatePreparation prepare_optimization_exit_states(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& requested) {
  try {
    std::vector<ir::OptimizationExitState> exit_states;
    for (const runtime::DeoptimizationRecord& record : requested.records()) {
      ir::OptimizationExitState exit_state;
      exit_state.exit = guard_value(function, record.site);
      for (const runtime::RecoveryOperation& operation : record.recovery) {
        if (operation.source != runtime::RecoverySource::kCapturedValue) {
          continue;
        }
        if (!exit_state.exit.valid()) {
          return {{StatusCode::kInvalidArgument,
                   "CFG captured recovery values require a native guard site",
                   record.site},
                  {}};
        }
        if (!control_value_available(function, operation.source_value,
                                     exit_state.exit)) {
          return {{StatusCode::kInvalidArgument,
                   "CFG captured recovery value does not dominate its guard",
                   record.site},
                  {}};
        }
        if (function.value_type(operation.source_value) != operation.type) {
          return {{StatusCode::kInvalidArgument,
                   "CFG captured recovery value type does not match SSA",
                   record.site},
                  {}};
        }
        if (std::find(exit_state.live_values.begin(),
                      exit_state.live_values.end(), operation.source_value) ==
            exit_state.live_values.end()) {
          exit_state.live_values.push_back(operation.source_value);
        }
      }
      if (!exit_state.live_values.empty()) {
        exit_states.push_back(std::move(exit_state));
      }
    }
    return {Status::ok_status(), std::move(exit_states)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare CFG optimizer exit state"},
            {}};
  }
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
    if (has_runtime_exit_site(input, dependency.site)) {
      return {{StatusCode::kInvalidArgument,
               "assumption and runtime exit sites must be distinct",
               dependency.site},
              {}};
    }
  }

  runtime::DeoptimizationTable result;
  try {
    for (const ir::Node& node : optimized.nodes()) {
      if (!is_nonzero_guard(node.opcode)) {
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
          input.parameter_count(),
          node.opcode == ir::Opcode::kGuardWordNonzero
              ? ir::ValueType::kWord
              : ir::ValueType::kFloat64));
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
    const ir::ControlFlowFunction& input,
    const ir::ControlFlowFunction& lowered,
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
               "CFG deoptimization metadata does not identify an exit site",
               record.site},
              {}};
    }
  }
  for (const runtime::AssumptionDependency& dependency :
       assumptions.dependencies()) {
    if (has_runtime_exit_site(input, dependency.site)) {
      return {{StatusCode::kInvalidArgument,
               "assumption and CFG runtime exit sites must be distinct",
               dependency.site},
              {}};
    }
  }

  runtime::DeoptimizationTable result;
  try {
    for (const ir::ControlNode& node : lowered.nodes()) {
      if (!is_nonzero_guard(node.opcode)) {
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
          input.parameter_count(),
          node.opcode == ir::ControlOpcode::kGuardWordNonzero
              ? ir::ValueType::kWord
              : ir::ValueType::kFloat64));
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
             "unable to prepare CFG deoptimization metadata"},
            {}};
  }
}

StackMapRequirementPreparation prepare_stack_map_requirements(
    const ir::Function& input, const ir::Function& lowered,
    const runtime::DeoptimizationTable& deoptimization,
    const ir::OptimizationResult* optimization) {
  try {
    StackMapRequirementPreparation result;
    result.status = Status::ok_status();
    for (const runtime::DeoptimizationRecord& record :
         deoptimization.records()) {
      const ir::Value lowered_exit = guard_value(lowered, record.site);
      detail::StackMapRequirement requirement;
      requirement.site = record.site;
      for (const runtime::RecoveryOperation& operation : record.recovery) {
        if (operation.source != runtime::RecoverySource::kCapturedValue) {
          continue;
        }
        if (!lowered_exit.valid()) {
          return {{StatusCode::kInvalidArgument,
                   "captured recovery value has no compiled guard",
                   record.site},
                  {}, {}};
        }
        const ir::Value lowered_value =
            optimization == nullptr
                ? operation.source_value
                : optimization->map(operation.source_value);
        if (!lowered_value.valid() || lowered_value.id() >= lowered_exit.id()) {
          return {{StatusCode::kCodeGenerationFailed,
                   "optimizer did not retain a captured recovery value",
                   record.site},
                  {}, {}};
        }
        if (input.value_type(operation.source_value) != operation.type ||
            lowered.value_type(lowered_value) != operation.type) {
          return {{StatusCode::kCodeGenerationFailed,
                   "captured recovery value changed type during optimization",
                   record.site},
                  {}, {}};
        }
        if (std::find(requirement.values.begin(), requirement.values.end(),
                      lowered_value) == requirement.values.end()) {
          requirement.values.push_back(lowered_value);
        }
        result.bindings.push_back(
            {record.site, operation.source_value, lowered_value});
      }
      if (!requirement.values.empty()) {
        result.requirements.push_back(std::move(requirement));
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare straight-line stack-map requirements"},
            {}, {}};
  }
}

StackMapRequirementPreparation prepare_stack_map_requirements(
    const ir::ControlFlowFunction& input,
    const ir::ControlFlowFunction& lowered,
    const runtime::DeoptimizationTable& deoptimization,
    const ir::ControlFlowOptimizationResult* optimization) {
  try {
    StackMapRequirementPreparation result;
    result.status = Status::ok_status();
    for (const runtime::DeoptimizationRecord& record :
         deoptimization.records()) {
      const ir::Value input_exit = guard_value(input, record.site);
      const ir::Value lowered_exit = guard_value(lowered, record.site);
      detail::StackMapRequirement requirement;
      requirement.site = record.site;
      for (const runtime::RecoveryOperation& operation : record.recovery) {
        if (operation.source != runtime::RecoverySource::kCapturedValue) {
          continue;
        }
        if (!input_exit.valid() || !control_value_available(
                                       input, operation.source_value,
                                       input_exit)) {
          return {{StatusCode::kInvalidArgument,
                   "CFG captured recovery value does not dominate its guard",
                   record.site},
                  {}, {}};
        }
        if (input.value_type(operation.source_value) != operation.type) {
          return {{StatusCode::kInvalidArgument,
                   "CFG captured recovery value type does not match SSA",
                   record.site},
                  {}, {}};
        }
        const ir::Value lowered_value =
            optimization == nullptr
                ? operation.source_value
                : optimization->map(operation.source_value);
        if (!lowered_exit.valid() ||
            !control_value_available(lowered, lowered_value, lowered_exit)) {
          return {{StatusCode::kCodeGenerationFailed,
                   "CFG optimizer did not retain a captured recovery value",
                   record.site},
                  {}, {}};
        }
        if (lowered.value_type(lowered_value) != operation.type) {
          return {{StatusCode::kCodeGenerationFailed,
                   "CFG captured recovery value changed type during optimization",
                   record.site},
                  {}, {}};
        }
        if (std::find(requirement.values.begin(), requirement.values.end(),
                      lowered_value) == requirement.values.end()) {
          requirement.values.push_back(lowered_value);
        }
        result.bindings.push_back(
            {record.site, operation.source_value, lowered_value});
      }
      if (!requirement.values.empty()) {
        result.requirements.push_back(std::move(requirement));
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to prepare CFG stack-map requirements"},
            {}, {}};
  }
}

Status resolve_deoptimization_captures(
    runtime::DeoptimizationTable* deoptimization,
    const std::vector<CaptureBinding>& bindings,
    const std::vector<StackMapRecord>& stack_maps) {
  if (bindings.empty()) {
    return Status::ok_status();
  }
  try {
    runtime::DeoptimizationTable resolved;
    for (const runtime::DeoptimizationRecord& source_record :
         deoptimization->records()) {
      runtime::DeoptimizationRecord record = source_record;
      for (runtime::RecoveryOperation& operation : record.recovery) {
        if (operation.source != runtime::RecoverySource::kCapturedValue) {
          continue;
        }
        const auto binding = std::find_if(
            bindings.begin(), bindings.end(), [&](const auto& candidate) {
              return candidate.site == record.site &&
                     candidate.source == operation.source_value;
            });
        const auto stack_map = std::find_if(
            stack_maps.begin(), stack_maps.end(), [&](const auto& candidate) {
              return candidate.site == record.site;
            });
        if (binding == bindings.end() || stack_map == stack_maps.end()) {
          return {StatusCode::kCodeGenerationFailed,
                  "captured recovery value has no native stack map",
                  record.site};
        }
        const auto stack_value = std::find_if(
            stack_map->live_values.begin(), stack_map->live_values.end(),
            [&](const auto& candidate) {
              return candidate.value == binding->lowered;
            });
        if (stack_value == stack_map->live_values.end() ||
            stack_value->type != operation.type) {
          return {StatusCode::kCodeGenerationFailed,
                  "captured recovery value is missing from its native stack map",
                  record.site};
        }
        operation.capture_index = static_cast<std::size_t>(
            stack_value - stack_map->live_values.begin());
      }
      const Status addition = resolved.add(record);
      if (!addition.ok()) {
        return addition;
      }
    }
    *deoptimization = std::move(resolved);
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to resolve captured deoptimization values"};
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

template <typename FunctionType>
std::vector<ir::ValueType> copy_parameter_types(
    const FunctionType& function) {
  std::vector<ir::ValueType> result;
  result.reserve(function.parameter_count());
  for (std::size_t index = 0; index < function.parameter_count(); ++index) {
    result.push_back(function.parameter_type(index));
  }
  return result;
}

std::vector<bool> trusted_object_writes(const ir::Function& function) {
  std::vector<bool> result(function.trusted_objects().size(), false);
  for (const ir::Node& node : function.nodes()) {
    if (node.opcode == ir::Opcode::kStoreObject &&
        node.trusted_object < result.size()) {
      result[node.trusted_object] = true;
    }
  }
  return result;
}

std::vector<bool> trusted_object_writes(
    const ir::ControlFlowFunction& function) {
  std::vector<bool> result(function.trusted_objects().size(), false);
  for (const ir::ControlNode& node : function.nodes()) {
    if (node.opcode == ir::ControlOpcode::kStoreObject &&
        node.trusted_object < result.size()) {
      result[node.trusted_object] = true;
    }
  }
  return result;
}

std::size_t stack_map_value_count(
    const std::vector<StackMapRecord>& records) noexcept {
  std::size_t result = 0;
  for (const StackMapRecord& record : records) {
    result += record.live_values.size();
  }
  return result;
}

Status validate_compilation_limits(const CompilationLimits& limits) {
  if (limits.maximum_parameters == 0 || limits.maximum_ir_nodes == 0 ||
      limits.maximum_cfg_blocks == 0 || limits.maximum_ir_arguments == 0 ||
      limits.maximum_memory_regions == 0 ||
      limits.maximum_memory_accesses == 0 ||
      limits.maximum_atomic_accesses == 0 ||
      limits.maximum_vector_constants == 0 ||
      limits.maximum_vector_shuffles == 0 ||
      limits.maximum_vector_selects == 0 || limits.maximum_frame_slots == 0 ||
      limits.maximum_trusted_objects == 0 || limits.maximum_stack_maps == 0 ||
      limits.maximum_metadata_values == 0 || limits.maximum_code_bytes == 0) {
    return {StatusCode::kInvalidArgument,
            "compilation resource limits must all be positive"};
  }
  return Status::ok_status();
}

Status validate_compilation_target(const TargetProfile& profile) {
  const Status validation = validate_target_profile(profile);
  if (!validation.ok()) {
    return validation;
  }
  const TargetProfile baseline = baseline_target_profile();
  if (profile.architecture != baseline.architecture ||
      profile.abi != baseline.abi ||
      profile.endianness != baseline.endianness) {
    return {StatusCode::kUnsupportedArchitecture,
            "requested target does not match the configured native backend"};
  }
  if (!target_profile_contains(host_target_profile(), profile)) {
    return {StatusCode::kUnsupportedArchitecture,
            "host does not provide every requested target feature"};
  }
  return Status::ok_status();
}

Status add_bounded(std::size_t value, std::size_t limit,
                   std::size_t* total, const char* message,
                   std::size_t location = 0) {
  if (*total > limit || value > limit - *total) {
    return {StatusCode::kResourceExhausted, message, location};
  }
  *total += value;
  return Status::ok_status();
}

Status validate_requested_metadata(
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions, std::size_t parameter_count,
    const CompilationLimits& limits) {
  if (deoptimization_table.size() > limits.maximum_stack_maps) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the exit-record limit"};
  }
  std::size_t records = deoptimization_table.size();
  for (const runtime::AssumptionDependency& dependency :
       assumptions.dependencies()) {
    if (deoptimization_table.find(dependency.site) != nullptr) {
      continue;
    }
    if (records == limits.maximum_stack_maps) {
      return {StatusCode::kResourceExhausted,
              "compilation exceeds the exit-record limit",
              dependency.site};
    }
    ++records;
  }
  std::size_t values = 0;
  for (const runtime::DeoptimizationRecord& record :
       deoptimization_table.records()) {
    const Status addition = add_bounded(
        record.recovery.size(), limits.maximum_metadata_values, &values,
        "compilation exceeds the deoptimization value limit", record.site);
    if (!addition.ok()) {
      return addition;
    }
  }
  for (const runtime::AssumptionDependency& dependency :
       assumptions.dependencies()) {
    if (deoptimization_table.find(dependency.site) != nullptr) {
      continue;
    }
    const Status addition = add_bounded(
        parameter_count, limits.maximum_metadata_values, &values,
        "compilation exceeds the deoptimization value limit",
        dependency.site);
    if (!addition.ok()) {
      return addition;
    }
  }
  return Status::ok_status();
}

Status validate_function_limits(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions,
    const CompilationLimits& limits) {
  const Status configuration = validate_compilation_limits(limits);
  if (!configuration.ok()) {
    return configuration;
  }
  if (function.parameter_count() > limits.maximum_parameters) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the parameter limit",
            function.parameter_count()};
  }
  if (function.nodes().size() > limits.maximum_ir_nodes) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the IR node limit",
            function.nodes().size()};
  }
  if (function.call_arguments().size() > limits.maximum_ir_arguments) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the IR argument limit",
            function.call_arguments().size()};
  }
  if (function.memory_region_count() > limits.maximum_memory_regions) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the memory region limit",
            function.memory_region_count()};
  }
  if (function.memory_accesses().size() >
      limits.maximum_memory_accesses) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the memory access limit",
            function.memory_accesses().size()};
  }
  if (function.atomic_accesses().size() > limits.maximum_atomic_accesses) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the atomic access limit",
            function.atomic_accesses().size()};
  }
  if (function.vector_constants().size() >
      limits.maximum_vector_constants) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the vector constant limit",
            function.vector_constants().size()};
  }
  if (function.vector_shuffles().size() > limits.maximum_vector_shuffles) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the vector shuffle limit",
            function.vector_shuffles().size()};
  }
  if (function.vector_select_arguments().size() >
      limits.maximum_vector_selects) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the vector select limit",
            function.vector_select_arguments().size()};
  }
  if (function.frame_slots().size() > limits.maximum_frame_slots) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the frame slot limit",
            function.frame_slots().size()};
  }
  if (function.trusted_objects().size() >
      limits.maximum_trusted_objects) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the trusted object limit",
            function.trusted_objects().size()};
  }
  return validate_requested_metadata(deoptimization_table, assumptions,
                                     function.parameter_count(), limits);
}

Status validate_function_limits(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions,
    const CompilationLimits& limits) {
  const Status configuration = validate_compilation_limits(limits);
  if (!configuration.ok()) {
    return configuration;
  }
  if (function.parameter_count() > limits.maximum_parameters) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the parameter limit",
            function.parameter_count()};
  }
  if (function.nodes().size() > limits.maximum_ir_nodes) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the IR node limit",
            function.nodes().size()};
  }
  if (function.blocks().size() > limits.maximum_cfg_blocks) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG block limit",
            function.blocks().size()};
  }
  if (function.memory_region_count() > limits.maximum_memory_regions) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG memory region limit",
            function.memory_region_count()};
  }
  if (function.memory_accesses().size() >
      limits.maximum_memory_accesses) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG memory access limit",
            function.memory_accesses().size()};
  }
  if (function.atomic_accesses().size() > limits.maximum_atomic_accesses) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG atomic access limit",
            function.atomic_accesses().size()};
  }
  if (function.vector_constants().size() >
      limits.maximum_vector_constants) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG vector constant limit",
            function.vector_constants().size()};
  }
  if (function.vector_shuffles().size() > limits.maximum_vector_shuffles) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG vector shuffle limit",
            function.vector_shuffles().size()};
  }
  if (function.vector_select_arguments().size() >
      limits.maximum_vector_selects) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG vector select limit",
            function.vector_select_arguments().size()};
  }
  if (function.frame_slots().size() > limits.maximum_frame_slots) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG frame slot limit",
            function.frame_slots().size()};
  }
  if (function.trusted_objects().size() >
      limits.maximum_trusted_objects) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG trusted object limit",
            function.trusted_objects().size()};
  }
  std::size_t arguments = function.call_arguments().size();
  if (arguments > limits.maximum_ir_arguments) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the CFG argument limit", arguments};
  }
  for (std::size_t index = 0; index < function.blocks().size(); ++index) {
    const ir::ControlTerminator& terminator =
        function.blocks()[index].terminator;
    if (terminator.opcode == ir::TerminatorOpcode::kJump ||
        terminator.opcode == ir::TerminatorOpcode::kBranch) {
      const Status true_edge = add_bounded(
          terminator.true_edge.arguments.size(), limits.maximum_ir_arguments,
          &arguments, "compilation exceeds the CFG argument limit", index);
      if (!true_edge.ok()) {
        return true_edge;
      }
    }
    if (terminator.opcode == ir::TerminatorOpcode::kBranch) {
      const Status false_edge = add_bounded(
          terminator.false_edge.arguments.size(),
          limits.maximum_ir_arguments, &arguments,
          "compilation exceeds the CFG argument limit", index);
      if (!false_edge.ok()) {
        return false_edge;
      }
    }
  }
  return validate_requested_metadata(deoptimization_table, assumptions,
                                     function.parameter_count(), limits);
}

Status validate_stack_map_limits(
    const detail::StackMapRequirements& requirements,
    const CompilationLimits& limits) {
  if (requirements.size() > limits.maximum_stack_maps) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the stack-map limit",
            requirements.size()};
  }
  std::size_t values = 0;
  for (const detail::StackMapRequirement& requirement : requirements) {
    const Status addition = add_bounded(
        requirement.values.size(), limits.maximum_metadata_values, &values,
        "compilation exceeds the stack-map value limit", requirement.site);
    if (!addition.ok()) {
      return addition;
    }
  }
  return Status::ok_status();
}

template <typename LoweringResult>
Status validate_lowering_limits(const LoweringResult& lowering,
                                const CompilationLimits& limits) {
  if (lowering.code.size() > limits.maximum_code_bytes) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the native code byte limit",
            lowering.code.size()};
  }
  if (lowering.stack_maps.size() > limits.maximum_stack_maps) {
    return {StatusCode::kResourceExhausted,
            "compilation exceeds the emitted stack-map limit",
            lowering.stack_maps.size()};
  }
  std::size_t values = 0;
  for (const StackMapRecord& record : lowering.stack_maps) {
    const Status addition = add_bounded(
        record.live_values.size(), limits.maximum_metadata_values, &values,
        "compilation exceeds the emitted stack-map value limit", record.site);
    if (!addition.ok()) {
      return addition;
    }
  }
  return Status::ok_status();
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

CompiledFunction::CompiledFunction(
    std::unique_ptr<Impl> impl, std::vector<ir::ValueType> parameter_types,
    ir::ValueType return_type, TargetProfile target_profile,
    CompilationStats stats, CapabilityReport capabilities,
    bool requires_context,
    std::vector<ir::TrustedObjectDescriptor> trusted_objects,
    std::vector<bool> trusted_object_writable,
    runtime::DeoptimizationTable deoptimization_table,
    runtime::AssumptionSet assumptions, StackMapTable stack_maps) noexcept
    : impl_(std::move(impl)), parameter_count_(parameter_types.size()),
      parameter_types_(std::move(parameter_types)), return_type_(return_type),
      target_profile_(target_profile),
      host_compatible_(
          target_profile_contains(host_target_profile(), target_profile)),
      stats_(stats), capabilities_(std::move(capabilities)),
      requires_context_(requires_context),
      trusted_objects_(std::move(trusted_objects)),
      trusted_object_writable_(std::move(trusted_object_writable)),
      deoptimization_table_(std::move(deoptimization_table)),
      assumptions_(std::move(assumptions)), stack_maps_(std::move(stack_maps)) {
}

CompiledFunction::~CompiledFunction() = default;
CompiledFunction::CompiledFunction(CompiledFunction&&) noexcept = default;
CompiledFunction& CompiledFunction::operator=(CompiledFunction&&) noexcept =
    default;

NativeEntry CompiledFunction::native_entry() const noexcept {
  return impl_ == nullptr || !host_compatible_ || !trusted_objects_.empty()
             ? nullptr
             : impl_->entry();
}

StackMapCaptureResult CompiledFunction::reconstruct_stack_map(
    const runtime::ExecutionContext& context) const {
  if (context.exit_reason() == runtime::ExitReason::kNone) {
    return {{StatusCode::kInvalidArgument,
             "execution context has no captured runtime exit"},
            {}};
  }
  const StackMapRecord* record = stack_map(context.exit_site());
  if (record == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "execution context does not identify a compiled stack map",
             context.exit_site()},
            {}};
  }
  const runtime::ExitReason expected_reason =
      record->kind == StackMapKind::kSafepoint
          ? runtime::ExitReason::kSafepoint
          : runtime::ExitReason::kRuntime;
  if (context.exit_reason() != expected_reason) {
    return {{StatusCode::kInvalidArgument,
             "execution context exit kind does not match its stack map",
             record->site},
            {}};
  }
  if (context.captured_value_count() != record->live_values.size()) {
    return {{StatusCode::kInvalidArgument,
             "execution context has incomplete stack-map values",
             record->site},
            {}};
  }

  try {
    CapturedStackMap capture;
    capture.site = record->site;
    capture.kind = record->kind;
    capture.values.reserve(record->live_values.size());
    for (std::size_t index = 0; index < record->live_values.size(); ++index) {
      const StackMapValue& value = record->live_values[index];
      capture.values.push_back(
          {value.value, value.type, context.captured_values()[index]});
    }
    return {Status::ok_status(), std::move(capture)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to reconstruct captured stack-map values",
             record->site},
            {}};
  }
}

runtime::ReconstructionResult CompiledFunction::reconstruct_deoptimization(
    std::size_t site, const ir::Word* args, std::size_t arg_count,
    const runtime::ExecutionContext& context) const {
  const runtime::DeoptimizationRecord* record =
      deoptimization_record(site);
  if (record != nullptr &&
      std::any_of(record->recovery.begin(), record->recovery.end(),
                  [](const runtime::RecoveryOperation& operation) {
                    return operation.source ==
                           runtime::RecoverySource::kCapturedValue;
                  })) {
    const StackMapRecord* map = stack_map(site);
    if (map == nullptr ||
        context.captured_value_count() != map->live_values.size()) {
      return {{StatusCode::kInvalidArgument,
               "execution context has incomplete deoptimization captures",
               site},
              {}};
    }
  }
  return deoptimization_table_.reconstruct(site, args, arg_count, context);
}

runtime::MaterializationResult CompiledFunction::materialize_deoptimization(
    std::size_t site, const ir::Word* args, std::size_t arg_count,
    const runtime::ExecutionContext& context,
    const runtime::MaterializationPlan& plan,
    const runtime::MaterializationCallbacks& callbacks) const {
  runtime::ReconstructionResult reconstruction =
      reconstruct_deoptimization(site, args, arg_count, context);
  if (!reconstruction.ok()) {
    return {reconstruction.status, {}};
  }
  return runtime::materialize_frame(reconstruction.frame, plan, callbacks);
}

runtime::OsrEntryResult CompiledFunction::enter_osr(
    const runtime::OsrFrame& frame, const runtime::OsrEntryPlan& plan,
    runtime::ExecutionContext* context) const {
  runtime::OsrArguments arguments = plan.marshal(frame, parameter_types_);
  if (!arguments.ok()) {
    return {arguments, {arguments.status, 0}};
  }
  return {arguments, invoke(arguments.data(), arguments.count, context)};
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
  const NativeEntry entry =
      impl_ == nullptr || !host_compatible_ ? nullptr : impl_->entry();
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
  const Status object_status = ir::detail::validate_trusted_object_bindings(
      trusted_objects_, trusted_object_writable_, active_context);
  if (!object_status.ok()) {
    return {object_status, 0};
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
                 runtime::AssumptionSet{}, CompilationOptions{});
}

CompilationResult Compiler::compile(const ir::Function& function,
                                    CompilationOptions options) {
  return compile(function, runtime::DeoptimizationTable{},
                 runtime::AssumptionSet{}, options);
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table) {
  return compile(function, deoptimization_table, runtime::AssumptionSet{},
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, runtime::DeoptimizationTable{}, assumptions,
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, deoptimization_table, assumptions,
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::Function& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions,
    CompilationOptions options) {
  const Status target_status =
      validate_compilation_target(options.target_profile);
  if (!target_status.ok()) {
    return {target_status, nullptr};
  }
  const Status limit_status = validate_function_limits(
      function, deoptimization_table, assumptions, options.limits);
  if (!limit_status.ok()) {
    return {limit_status, nullptr};
  }
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }
  const OptimizationExitStatePreparation exit_state =
      prepare_optimization_exit_states(function, deoptimization_table);
  if (!exit_state.status.ok()) {
    return {exit_state.status, nullptr};
  }

  std::optional<ir::OptimizationResult> optimization;
  const ir::Function* lowered = &function;
  switch (options.optimization_level) {
    case OptimizationLevel::kBaseline:
      break;
    case OptimizationLevel::kOptimized:
      optimization.emplace(
          ir::Optimizer::run(function, exit_state.exit_states));
      if (!optimization->ok()) {
        return {optimization->status, nullptr};
      }
      lowered = &optimization->function;
      break;
    default:
      return {{StatusCode::kInvalidArgument,
               "unknown compiler optimization level"},
              nullptr};
  }
  CapabilityReport capabilities =
      preflight_capabilities(*lowered, options.target_profile);
  if (!capabilities.ok()) {
    return {capabilities.status, nullptr};
  }

  DeoptimizationPreparation deoptimization =
      prepare_deoptimization(function, *lowered, deoptimization_table,
                             assumptions);
  if (!deoptimization.status.ok()) {
    return {deoptimization.status, nullptr};
  }
  StackMapRequirementPreparation stack_map_requirements =
      prepare_stack_map_requirements(
          function, *lowered, deoptimization.table,
          optimization.has_value() ? &*optimization : nullptr);
  if (!stack_map_requirements.status.ok()) {
    return {stack_map_requirements.status, nullptr};
  }
  const Status stack_map_limit = validate_stack_map_limits(
      stack_map_requirements.requirements, options.limits);
  if (!stack_map_limit.ok()) {
    return {stack_map_limit, nullptr};
  }

#if defined(UNIJIT_TARGET_AARCH64)
  detail::aarch64::LoweringResult lowering = detail::aarch64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#elif defined(UNIJIT_TARGET_X86_64)
  detail::x86_64::LoweringResult lowering = detail::x86_64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#elif defined(UNIJIT_TARGET_RISCV64)
  detail::riscv64::LoweringResult lowering = detail::riscv64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#endif

#if defined(UNIJIT_TARGET_AARCH64) || defined(UNIJIT_TARGET_X86_64) || \
    defined(UNIJIT_TARGET_RISCV64)
  if (!lowering.status.ok()) {
    return {lowering.status, nullptr};
  }
  const Status lowering_limit =
      validate_lowering_limits(lowering, options.limits);
  if (!lowering_limit.ok()) {
    return {lowering_limit, nullptr};
  }
  const Status capture_resolution = resolve_deoptimization_captures(
      &deoptimization.table, stack_map_requirements.bindings,
      lowering.stack_maps);
  if (!capture_resolution.ok()) {
    return {capture_resolution, nullptr};
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
                           lowering.spill_slots, function.frame_slots().size(),
                           function.trusted_objects().size(),
                           function.nodes().size(),
                           lowered->nodes().size(), lowering.stack_maps.size(),
                           stack_map_value_count(lowering.stack_maps)};
    capabilities.requires_execution_context =
        capabilities.requires_execution_context || !assumptions.empty() ||
        !function.trusted_objects().empty();
    const bool requires_context = capabilities.requires_execution_context;
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), copy_parameter_types(function),
        function.return_type(), options.target_profile, stats,
        std::move(capabilities), requires_context, function.trusted_objects(),
        trusted_object_writes(function), std::move(deoptimization.table),
        assumptions, StackMapTable(std::move(lowering.stack_maps))));
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
                 runtime::AssumptionSet{}, CompilationOptions{});
}

CompilationResult Compiler::compile(const ir::ControlFlowFunction& function,
                                    CompilationOptions options) {
  return compile(function, runtime::DeoptimizationTable{},
                 runtime::AssumptionSet{}, options);
}

CompilationResult Compiler::compile(const ir::ControlFlowFunction& function,
                                    CompilationLimits limits) {
  return compile(function, runtime::DeoptimizationTable{},
                 runtime::AssumptionSet{},
                 CompilationOptions{OptimizationLevel::kOptimized, limits});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table) {
  return compile(function, deoptimization_table, runtime::AssumptionSet{},
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    CompilationOptions options) {
  return compile(function, deoptimization_table, runtime::AssumptionSet{},
                 options);
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, runtime::DeoptimizationTable{}, assumptions,
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::AssumptionSet& assumptions, CompilationOptions options) {
  return compile(function, runtime::DeoptimizationTable{}, assumptions,
                 options);
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions) {
  return compile(function, deoptimization_table, assumptions,
                 CompilationOptions{});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions, CompilationLimits limits) {
  return compile(
      function, deoptimization_table, assumptions,
      CompilationOptions{OptimizationLevel::kOptimized, limits});
}

CompilationResult Compiler::compile(
    const ir::ControlFlowFunction& function,
    const runtime::DeoptimizationTable& deoptimization_table,
    const runtime::AssumptionSet& assumptions, CompilationOptions options) {
  const Status target_status =
      validate_compilation_target(options.target_profile);
  if (!target_status.ok()) {
    return {target_status, nullptr};
  }
  const Status limit_status = validate_function_limits(
      function, deoptimization_table, assumptions, options.limits);
  if (!limit_status.ok()) {
    return {limit_status, nullptr};
  }
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }
  const OptimizationExitStatePreparation exit_state =
      prepare_optimization_exit_states(function, deoptimization_table);
  if (!exit_state.status.ok()) {
    return {exit_state.status, nullptr};
  }

  std::optional<ir::ControlFlowOptimizationResult> optimization;
  const ir::ControlFlowFunction* lowered = &function;
  switch (options.optimization_level) {
    case OptimizationLevel::kBaseline:
      break;
    case OptimizationLevel::kOptimized:
      optimization.emplace(
          ir::Optimizer::run(function, exit_state.exit_states));
      if (!optimization->ok()) {
        return {optimization->status, nullptr};
      }
      lowered = &optimization->function;
      break;
    default:
      return {{StatusCode::kInvalidArgument,
               "unknown compiler optimization level"},
              nullptr};
  }
  CapabilityReport capabilities =
      preflight_capabilities(*lowered, options.target_profile);
  if (!capabilities.ok()) {
    return {capabilities.status, nullptr};
  }

  DeoptimizationPreparation deoptimization =
      prepare_deoptimization(function, *lowered, deoptimization_table,
                             assumptions);
  if (!deoptimization.status.ok()) {
    return {deoptimization.status, nullptr};
  }
  StackMapRequirementPreparation stack_map_requirements =
      prepare_stack_map_requirements(
          function, *lowered, deoptimization.table,
          optimization.has_value() ? &*optimization : nullptr);
  if (!stack_map_requirements.status.ok()) {
    return {stack_map_requirements.status, nullptr};
  }
  const Status stack_map_limit = validate_stack_map_limits(
      stack_map_requirements.requirements, options.limits);
  if (!stack_map_limit.ok()) {
    return {stack_map_limit, nullptr};
  }

#if defined(UNIJIT_TARGET_AARCH64)
  detail::aarch64::LoweringResult lowering = detail::aarch64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#elif defined(UNIJIT_TARGET_X86_64)
  detail::x86_64::LoweringResult lowering = detail::x86_64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#elif defined(UNIJIT_TARGET_RISCV64)
  detail::riscv64::LoweringResult lowering = detail::riscv64::lower(
      *lowered, stack_map_requirements.requirements,
      options.measure_safepoint_polls);
#endif

#if defined(UNIJIT_TARGET_AARCH64) || defined(UNIJIT_TARGET_X86_64) || \
    defined(UNIJIT_TARGET_RISCV64)
  if (!lowering.status.ok()) {
    return {lowering.status, nullptr};
  }
  const Status lowering_limit =
      validate_lowering_limits(lowering, options.limits);
  if (!lowering_limit.ok()) {
    return {lowering_limit, nullptr};
  }
  const Status capture_resolution = resolve_deoptimization_captures(
      &deoptimization.table, stack_map_requirements.bindings,
      lowering.stack_maps);
  if (!capture_resolution.ok()) {
    return {capture_resolution, nullptr};
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
                           lowering.spill_slots, function.frame_slots().size(),
                           function.trusted_objects().size(),
                           function.nodes().size(),
                           lowered->nodes().size(), lowering.stack_maps.size(),
                           stack_map_value_count(lowering.stack_maps)};
    capabilities.requires_execution_context =
        capabilities.requires_execution_context || !assumptions.empty() ||
        !function.trusted_objects().empty();
    const bool requires_context = capabilities.requires_execution_context;
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), copy_parameter_types(function),
        function.return_type(), options.target_profile, stats,
        std::move(capabilities), requires_context, function.trusted_objects(),
        trusted_object_writes(function), std::move(deoptimization.table),
        assumptions, StackMapTable(std::move(lowering.stack_maps))));
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
