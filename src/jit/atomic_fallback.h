#ifndef UNIJIT_SRC_JIT_ATOMIC_FALLBACK_H
#define UNIJIT_SRC_JIT_ATOMIC_FALLBACK_H

#include <cstddef>
#include <cstdint>

#include "unijit/ir/function.h"

namespace unijit::jit::detail {

enum class AtomicFallbackOperation : std::uint64_t {
  kExchange = 0,
  kCompareExchange,
  kFetchAdd,
  kFetchAnd,
  kFetchOr,
  kFetchXor,
};

ir::Word execute_atomic_fallback(const ir::Word* arguments,
                                 std::size_t count) noexcept;

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_ATOMIC_FALLBACK_H
