#ifndef UNIJIT_SRC_JIT_BACKEND_AARCH64_LOWER_H
#define UNIJIT_SRC_JIT_BACKEND_AARCH64_LOWER_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/stack_map.h"
#include "unijit/status.h"

namespace unijit::jit::detail::aarch64 {

struct LoweringResult final {
  LoweringResult(Status result_status, std::vector<std::uint8_t> result_code,
                 std::size_t result_spill_slots,
                 std::vector<StackMapRecord> result_stack_maps = {}) noexcept
      : status(std::move(result_status)),
        code(std::move(result_code)),
        spill_slots(result_spill_slots),
        stack_maps(std::move(result_stack_maps)) {}

  Status status;
  std::vector<std::uint8_t> code;
  std::size_t spill_slots{0};
  std::vector<StackMapRecord> stack_maps;
};

LoweringResult lower(const ir::Function& function);
LoweringResult lower(const ir::ControlFlowFunction& function);

}  // namespace unijit::jit::detail::aarch64

#endif  // UNIJIT_SRC_JIT_BACKEND_AARCH64_LOWER_H
