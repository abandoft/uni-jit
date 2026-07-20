#ifndef UNIJIT_SRC_JIT_BACKEND_RISCV64_LOWER_H
#define UNIJIT_SRC_JIT_BACKEND_RISCV64_LOWER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::jit::detail::riscv64 {

struct LoweringResult final {
  Status status;
  std::vector<std::uint8_t> code;
  std::size_t spill_slots{0};
};

LoweringResult lower(const ir::Function& function);

}  // namespace unijit::jit::detail::riscv64

#endif  // UNIJIT_SRC_JIT_BACKEND_RISCV64_LOWER_H
