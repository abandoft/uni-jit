#ifndef UNIJIT_SRC_IR_MEMORY_ACCESS_H
#define UNIJIT_SRC_IR_MEMORY_ACCESS_H

#include <cstddef>

#include "unijit/ir/function.h"
#include "unijit/runtime/execution_context.h"

namespace unijit::ir::detail {

struct MemoryAccessResult final {
  Status status;
  Word value{0};

  bool ok() const noexcept { return status.ok(); }
};

struct VectorMemoryAccessResult final {
  Status status;
  Vector128 value;

  bool ok() const noexcept { return status.ok(); }
};

MemoryAccessResult load_bounded_word(
    const MemoryAccessDescriptor& access, Word byte_offset, std::size_t site,
    runtime::ExecutionContext* context) noexcept;

MemoryAccessResult store_bounded_word(
    const MemoryAccessDescriptor& access, Word byte_offset, Word value,
    std::size_t site, runtime::ExecutionContext* context) noexcept;

MemoryAccessResult load_bounded_float(
    const MemoryAccessDescriptor& access, Word byte_offset, std::size_t site,
    runtime::ExecutionContext* context) noexcept;

MemoryAccessResult store_bounded_float(
    const MemoryAccessDescriptor& access, Word byte_offset, Word value,
    std::size_t site, runtime::ExecutionContext* context) noexcept;

VectorMemoryAccessResult
load_bounded_vector(const MemoryAccessDescriptor &access, ValueType type,
                    Word byte_offset, std::size_t site,
                    runtime::ExecutionContext *context) noexcept;

VectorMemoryAccessResult
store_bounded_vector(const MemoryAccessDescriptor &access, ValueType type,
                     Word byte_offset, const Vector128 &value, std::size_t site,
                     runtime::ExecutionContext *context) noexcept;

}  // namespace unijit::ir::detail

#endif  // UNIJIT_SRC_IR_MEMORY_ACCESS_H
