#ifndef UNIJIT_SRC_IR_OBJECT_ACCESS_H
#define UNIJIT_SRC_IR_OBJECT_ACCESS_H

#include <cstddef>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/runtime/execution_context.h"

namespace unijit::ir::detail {

struct ObjectAccessResult final {
  Status status;
  Word value{0};

  bool ok() const noexcept { return status.ok(); }
};

Status validate_trusted_object_bindings(
    const std::vector<TrustedObjectDescriptor>& descriptors,
    const std::vector<bool>& writable,
    runtime::ExecutionContext* context) noexcept;

ObjectAccessResult load_trusted_object(
    TrustedObjectSlot slot, std::size_t byte_offset,
    runtime::ExecutionContext* context) noexcept;

ObjectAccessResult store_trusted_object(
    TrustedObjectSlot slot, std::size_t byte_offset, Word value,
    runtime::ExecutionContext* context) noexcept;

}  // namespace unijit::ir::detail

#endif  // UNIJIT_SRC_IR_OBJECT_ACCESS_H
