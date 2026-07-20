#ifndef UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H
#define UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::jit::detail {

struct ValueLocation final {
  static constexpr std::size_t kNone =
      std::numeric_limits<std::size_t>::max();

  std::size_t register_index{kNone};
  std::size_t spill_slot{kNone};

  bool in_register() const noexcept { return register_index != kNone; }
};

struct RegisterAllocation final {
  Status status;
  std::vector<ValueLocation> locations;
  std::size_t spill_slots{0};
};

struct ControlFlowRegisterAllocation final {
  Status status;
  std::vector<std::size_t> register_indices;
  std::vector<std::size_t> owner_blocks;
  std::vector<bool> requires_stack;
};

enum class ControlFlowMoveSource : std::uint8_t {
  kRegister,
  kStack,
  kTemporary,
};

struct ControlFlowRegisterMove final {
  ControlFlowMoveSource source_kind{ControlFlowMoveSource::kRegister};
  std::size_t source_index{0};
  std::size_t destination_index{0};
};

struct ControlFlowEdgeMoves final {
  bool uses_registers{false};
  std::vector<ControlFlowRegisterMove> moves;
};

RegisterAllocation allocate_linear_scan(const ir::Function& function,
                                        std::size_t register_count,
                                        std::size_t maximum_spill_slots);

ControlFlowRegisterAllocation allocate_control_flow_registers(
    const ir::ControlFlowFunction& function, std::size_t register_count);

ControlFlowEdgeMoves plan_control_flow_edge_moves(
    const ir::ControlFlowFunction& function, const ir::ControlEdge& edge,
    const ControlFlowRegisterAllocation& allocation,
    std::size_t current_block);

}  // namespace unijit::jit::detail

#endif  // UNIJIT_SRC_JIT_REGISTER_ALLOCATOR_H
