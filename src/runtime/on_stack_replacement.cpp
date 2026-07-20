#include "unijit/runtime/on_stack_replacement.h"

#include <algorithm>
#include <new>

namespace unijit::runtime {
namespace {

bool valid_value_type(ir::ValueType type) noexcept {
  return type == ir::ValueType::kWord || type == ir::ValueType::kFloat64;
}

}  // namespace

Status OsrFrame::add(std::size_t slot, ir::ValueType type, ir::Word value) {
  if (!valid_value_type(type)) {
    return {StatusCode::kInvalidArgument,
            "OSR frame value has an unsupported type", slot};
  }
  if (values_.size() == kMaximumValues) {
    return {StatusCode::kResourceExhausted,
            "OSR frame exceeds the value limit", slot};
  }
  if (find(slot) != nullptr) {
    return {StatusCode::kInvalidArgument,
            "OSR frame slot is duplicated", slot};
  }
  try {
    values_.push_back({slot, type, value});
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate OSR frame state", slot};
  }
}

const OsrValue* OsrFrame::find(std::size_t slot) const noexcept {
  const auto value =
      std::find_if(values_.begin(), values_.end(), [slot](const OsrValue& item) {
        return item.slot == slot;
      });
  return value == values_.end() ? nullptr : &*value;
}

Status OsrEntryPlan::add_argument(std::size_t slot, ir::ValueType type) {
  if (!valid_value_type(type)) {
    return {StatusCode::kInvalidArgument,
            "OSR argument binding has an unsupported type", slot};
  }
  if (bindings_.size() == kMaximumArguments) {
    return {StatusCode::kResourceExhausted,
            "OSR entry plan exceeds the argument limit", slot};
  }
  if (std::any_of(bindings_.begin(), bindings_.end(),
                  [slot](const OsrBinding& binding) {
                    return binding.slot == slot;
                  })) {
    return {StatusCode::kInvalidArgument,
            "OSR argument binding reuses a logical slot", slot};
  }
  try {
    bindings_.push_back({slot, type});
    return Status::ok_status();
  } catch (const std::bad_alloc&) {
    return {StatusCode::kResourceExhausted,
            "unable to allocate OSR entry metadata", slot};
  }
}

OsrArguments OsrEntryPlan::marshal(
    const OsrFrame& frame,
    const std::vector<ir::ValueType>& parameter_types) const {
  OsrArguments arguments;
  arguments.entry_site = entry_site_;
  arguments.resume_offset = resume_offset_;
  if (frame.entry_site() != entry_site_ ||
      frame.resume_offset() != resume_offset_) {
    arguments.status = {
        StatusCode::kInvalidArgument,
        "OSR frame does not match the entry site and resume offset",
        frame.entry_site()};
    return arguments;
  }
  if (parameter_types.size() != bindings_.size()) {
    arguments.status = {
        StatusCode::kInvalidArgument,
        "OSR plan does not match the compiled function parameter count",
        entry_site_};
    return arguments;
  }
  for (std::size_t index = 0; index < bindings_.size(); ++index) {
    const OsrBinding& binding = bindings_[index];
    if (parameter_types[index] != binding.type) {
      arguments.status = {
          StatusCode::kInvalidArgument,
          "OSR argument binding does not match the compiled signature",
          binding.slot};
      return arguments;
    }
    const OsrValue* value = frame.find(binding.slot);
    if (value == nullptr || value->type != binding.type) {
      arguments.status = {
          StatusCode::kInvalidArgument,
          "OSR argument is missing from the interpreter frame",
          binding.slot};
      return arguments;
    }
    arguments.values[index] = value->value;
  }
  arguments.count = bindings_.size();
  arguments.status = Status::ok_status();
  return arguments;
}

}  // namespace unijit::runtime
