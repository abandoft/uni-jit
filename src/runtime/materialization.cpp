#include "unijit/runtime/materialization.h"

#include <algorithm>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace unijit::runtime {
namespace {

bool valid_value_type(ir::ValueType type) noexcept {
  return type == ir::ValueType::kWord || type == ir::ValueType::kFloat64;
}

bool valid_source(MaterializationSource source) noexcept {
  return source == MaterializationSource::kRecoveredSlot ||
         source == MaterializationSource::kConstant ||
         source == MaterializationSource::kObject;
}

Status validate_callbacks(const MaterializationCallbacks& callbacks) noexcept {
  if (callbacks.begin == nullptr || callbacks.allocate == nullptr ||
      callbacks.store_primitive == nullptr ||
      callbacks.store_object == nullptr ||
      callbacks.store_frame_primitive == nullptr ||
      callbacks.store_frame_object == nullptr || callbacks.commit == nullptr ||
      callbacks.rollback == nullptr) {
    return {StatusCode::kInvalidArgument,
            "object materialization callbacks are incomplete"};
  }
  return Status::ok_status();
}

MaterializationResult rollback_failure(
    const MaterializationCallbacks& callbacks, Status status) noexcept {
  callbacks.rollback(callbacks.state);
  return {std::move(status), {}};
}

}  // namespace

MaterializationInput MaterializationInput::recovered(
    std::size_t slot, ir::ValueType type) noexcept {
  MaterializationInput input;
  input.source = MaterializationSource::kRecoveredSlot;
  input.type = type;
  input.recovered_slot = slot;
  return input;
}

MaterializationInput MaterializationInput::constant_value(
    ir::ValueType type, ir::Word value) noexcept {
  MaterializationInput input;
  input.source = MaterializationSource::kConstant;
  input.type = type;
  input.constant = value;
  return input;
}

MaterializationInput MaterializationInput::object(
    std::size_t object_id) noexcept {
  MaterializationInput input;
  input.source = MaterializationSource::kObject;
  input.object_id = object_id;
  return input;
}

Status MaterializationPlan::add(const ObjectRecipe& recipe) {
  if (recipes_.size() == kMaximumObjects) {
    return {StatusCode::kResourceExhausted,
            "object materialization plan exceeds the object limit",
            recipe.id};
  }
  if (std::any_of(recipes_.begin(), recipes_.end(),
                  [&](const ObjectRecipe& existing) {
                    return existing.id == recipe.id;
                  })) {
    return {StatusCode::kInvalidArgument,
            "object materialization recipe id is duplicated", recipe.id};
  }
  if (std::any_of(recipes_.begin(), recipes_.end(),
                  [&](const ObjectRecipe& existing) {
                    return existing.destination_slot == recipe.destination_slot;
                  })) {
    return {StatusCode::kInvalidArgument,
            "object materialization destination slot is duplicated",
            recipe.destination_slot};
  }
  if (recipe.fields.size() > kMaximumFields) {
    return {StatusCode::kResourceExhausted,
            "object materialization recipe exceeds the field limit",
            recipe.id};
  }
  std::size_t field_count = recipe.fields.size();
  for (const ObjectRecipe& existing : recipes_) {
    if (field_count > kMaximumFields - existing.fields.size()) {
      return {StatusCode::kResourceExhausted,
              "object materialization plan exceeds the field limit",
              recipe.id};
    }
    field_count += existing.fields.size();
  }
  for (std::size_t index = 0; index < recipe.fields.size(); ++index) {
    const MaterializationInput& input = recipe.fields[index];
    if (!valid_source(input.source) ||
        (input.source != MaterializationSource::kObject &&
         !valid_value_type(input.type))) {
      return {StatusCode::kInvalidArgument,
              "object materialization field is malformed", index};
    }
  }

  try {
    recipes_.push_back(recipe);
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate object materialization metadata", recipe.id};
  }
}

Status MaterializationPlan::validate() const {
  try {
    std::unordered_set<std::size_t> ids;
    ids.reserve(recipes_.size());
    std::unordered_set<std::size_t> destinations;
    destinations.reserve(recipes_.size());
    std::size_t fields = 0;
    for (const ObjectRecipe& recipe : recipes_) {
      if (!ids.insert(recipe.id).second) {
        return {StatusCode::kInvalidArgument,
                "object materialization recipe id is duplicated", recipe.id};
      }
      if (!destinations.insert(recipe.destination_slot).second) {
        return {StatusCode::kInvalidArgument,
                "object materialization destination slot is duplicated",
                recipe.destination_slot};
      }
      if (recipe.fields.size() > kMaximumFields ||
          fields > kMaximumFields - recipe.fields.size()) {
        return {StatusCode::kResourceExhausted,
                "object materialization plan exceeds the field limit",
                recipe.id};
      }
      fields += recipe.fields.size();
      for (std::size_t index = 0; index < recipe.fields.size(); ++index) {
        const MaterializationInput& input = recipe.fields[index];
        if (!valid_source(input.source) ||
            (input.source != MaterializationSource::kObject &&
             !valid_value_type(input.type))) {
          return {StatusCode::kInvalidArgument,
                  "object materialization field is malformed", index};
        }
      }
    }
    for (const ObjectRecipe& recipe : recipes_) {
      for (const MaterializationInput& input : recipe.fields) {
        if (input.source == MaterializationSource::kObject &&
            ids.find(input.object_id) == ids.end()) {
          return {StatusCode::kInvalidArgument,
                  "object materialization references an unknown object",
                  input.object_id};
        }
      }
    }
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to validate object materialization metadata"};
  }
}

const MaterializedValue* MaterializedFrame::find(
    std::size_t slot) const noexcept {
  const auto value =
      std::find_if(values.begin(), values.end(), [slot](const auto& candidate) {
        return candidate.slot == slot;
      });
  return value == values.end() ? nullptr : &*value;
}

MaterializationResult materialize_frame(
    const ReconstructedFrame& frame, const MaterializationPlan& plan,
    const MaterializationCallbacks& callbacks) {
  if (plan.site() != frame.site ||
      plan.resume_offset() != frame.resume_offset) {
    return {{StatusCode::kInvalidArgument,
             "object materialization plan does not match the reconstructed frame",
             frame.site},
            {}};
  }
  const Status plan_status = plan.validate();
  if (!plan_status.ok()) {
    return {plan_status, {}};
  }
  const Status callback_status = validate_callbacks(callbacks);
  if (!callback_status.ok()) {
    return {callback_status, {}};
  }

  try {
    std::unordered_map<std::size_t, std::size_t> object_indices;
    object_indices.reserve(plan.size());
    for (std::size_t index = 0; index < plan.recipes().size(); ++index) {
      object_indices.emplace(plan.recipes()[index].id, index);
    }

    std::unordered_set<std::size_t> frame_slots;
    frame_slots.reserve(frame.values.size() + plan.size());
    MaterializedFrame output;
    output.reason = frame.reason;
    output.site = frame.site;
    output.resume_offset = frame.resume_offset;
    output.values.reserve(frame.values.size() + plan.size());
    for (const RecoveredValue& value : frame.values) {
      if (!valid_value_type(value.type) ||
          !frame_slots.insert(value.slot).second) {
        return {{StatusCode::kInvalidArgument,
                 "reconstructed frame has malformed primitive slots",
                 value.slot},
                {}};
      }
      output.values.push_back(
          {value.slot,
           value.type == ir::ValueType::kFloat64
               ? MaterializedValueKind::kFloat64
               : MaterializedValueKind::kWord,
           value.value, 0});
    }
    for (const ObjectRecipe& recipe : plan.recipes()) {
      if (!frame_slots.insert(recipe.destination_slot).second) {
        return {{StatusCode::kInvalidArgument,
                 "object destination conflicts with a reconstructed slot",
                 recipe.destination_slot},
                {}};
      }
      for (const MaterializationInput& input : recipe.fields) {
        if (input.source != MaterializationSource::kRecoveredSlot) {
          continue;
        }
        const RecoveredValue* recovered = frame.find(input.recovered_slot);
        if (recovered == nullptr || recovered->type != input.type) {
          return {{StatusCode::kInvalidArgument,
                   "object field references an unavailable recovered slot",
                   input.recovered_slot},
                  {}};
        }
      }
    }

    std::vector<ObjectHandle> handles(plan.size(), 0);
    const std::size_t frame_value_count = frame.values.size() + plan.size();
    const Status begin = callbacks.begin(
        callbacks.state, frame.reason, frame.site, frame.resume_offset,
        plan.size(), frame_value_count);
    if (!begin.ok()) {
      return rollback_failure(callbacks, begin);
    }
    for (std::size_t index = 0; index < plan.recipes().size(); ++index) {
      const ObjectRecipe& recipe = plan.recipes()[index];
      const Status allocation = callbacks.allocate(
          callbacks.state, recipe.id, recipe.kind, recipe.fields.size(),
          &handles[index]);
      if (!allocation.ok()) {
        return rollback_failure(callbacks, allocation);
      }
    }
    for (std::size_t index = 0; index < plan.recipes().size(); ++index) {
      const ObjectRecipe& recipe = plan.recipes()[index];
      for (std::size_t field_index = 0;
           field_index < recipe.fields.size(); ++field_index) {
        const MaterializationInput& input = recipe.fields[field_index];
        Status stored;
        if (input.source == MaterializationSource::kObject) {
          stored = callbacks.store_object(
              callbacks.state, handles[index], field_index,
              handles[object_indices.find(input.object_id)->second]);
        } else {
          const ir::Word value =
              input.source == MaterializationSource::kConstant
                  ? input.constant
                  : frame.find(input.recovered_slot)->value;
          stored = callbacks.store_primitive(callbacks.state, handles[index],
                                             field_index, input.type, value);
        }
        if (!stored.ok()) {
          return rollback_failure(callbacks, stored);
        }
      }
    }
    for (std::size_t index = 0; index < plan.recipes().size(); ++index) {
      output.values.push_back(
          {plan.recipes()[index].destination_slot,
           MaterializedValueKind::kObject, 0, handles[index]});
    }
    for (const MaterializedValue& value : output.values) {
      Status installed;
      if (value.kind == MaterializedValueKind::kObject) {
        installed = callbacks.store_frame_object(
            callbacks.state, value.slot, value.object);
      } else {
        installed = callbacks.store_frame_primitive(
            callbacks.state, value.slot,
            value.kind == MaterializedValueKind::kFloat64
                ? ir::ValueType::kFloat64
                : ir::ValueType::kWord,
            value.value);
      }
      if (!installed.ok()) {
        return rollback_failure(callbacks, installed);
      }
    }
    const Status committed = callbacks.commit(callbacks.state);
    if (!committed.ok()) {
      return rollback_failure(callbacks, committed);
    }
    return {Status::ok_status(), std::move(output)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate object materialization state"},
            {}};
  }
}

}  // namespace unijit::runtime
