#ifndef UNIJIT_SRC_IR_ATOMIC_ACCESS_H
#define UNIJIT_SRC_IR_ATOMIC_ACCESS_H

#include <cstddef>

#include "unijit/ir/function.h"
#include "unijit/runtime/execution_context.h"

namespace unijit::ir::detail {

enum class AtomicOperation {
  kLoad,
  kStore,
  kExchange,
  kCompareExchange,
  kFetchAdd,
  kFetchAnd,
  kFetchOr,
  kFetchXor,
};

struct AtomicAccessResult final {
  Status status;
  Word value{0};
  bool success{false};

  bool ok() const noexcept { return status.ok(); }
};

AtomicAccessResult access_bounded_atomic(const AtomicAccessDescriptor &access,
                                         AtomicOperation operation,
                                         Word byte_offset, Word value,
                                         Word desired, std::size_t site,
                                         runtime::ExecutionContext *context);

void execute_atomic_fence(AtomicMemoryOrder order);

} // namespace unijit::ir::detail

#endif // UNIJIT_SRC_IR_ATOMIC_ACCESS_H
