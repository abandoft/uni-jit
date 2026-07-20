#include "unijit/ir/optimizer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace unijit::ir {
namespace {

struct PassResult final {
  Function function;
  std::size_t constants_folded{0};
  std::size_t algebraic_simplifications{0};
  bool changed{false};
};

std::uint64_t to_bits(Word value) noexcept {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Word from_bits(std::uint64_t bits) noexcept {
  Word result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

Word fold_binary(Opcode opcode, Word lhs, Word rhs) noexcept {
  const std::uint64_t lhs_bits = to_bits(lhs);
  const std::uint64_t rhs_bits = to_bits(rhs);
  if (opcode == Opcode::kAdd) {
    return from_bits(lhs_bits + rhs_bits);
  }
  if (opcode == Opcode::kSubtract) {
    return from_bits(lhs_bits - rhs_bits);
  }
  return from_bits(lhs_bits * rhs_bits);
}

Word fold_float_binary(Opcode opcode, Word lhs, Word rhs) noexcept {
  const double lhs_value = unpack_float64(lhs);
  const double rhs_value = unpack_float64(rhs);
  if (opcode == Opcode::kFloatAdd) {
    return pack_float64(lhs_value + rhs_value);
  }
  if (opcode == Opcode::kFloatSubtract) {
    return pack_float64(lhs_value - rhs_value);
  }
  if (opcode == Opcode::kFloatMultiply) {
    return pack_float64(lhs_value * rhs_value);
  }
  return pack_float64(lhs_value / rhs_value);
}

bool is_binary(Opcode opcode) noexcept {
  return opcode == Opcode::kAdd || opcode == Opcode::kSubtract ||
         opcode == Opcode::kMultiply || opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide;
}

bool is_float_binary(Opcode opcode) noexcept {
  return opcode == Opcode::kFloatAdd ||
         opcode == Opcode::kFloatSubtract ||
         opcode == Opcode::kFloatMultiply || opcode == Opcode::kFloatDivide;
}

PassResult transform_once(const Function& input) {
  const std::size_t node_count = input.nodes().size();
  std::vector<bool> live(node_count, false);
  live[input.return_value().id()] = true;
  for (std::size_t index = 0; index < node_count; ++index) {
    if (input.nodes()[index].opcode == Opcode::kCall ||
        input.nodes()[index].opcode == Opcode::kGuardFloatNonzero ||
        input.nodes()[index].opcode == Opcode::kSafepoint) {
      live[index] = true;
    }
  }
  for (std::size_t reverse = node_count; reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    if (!live[index]) {
      continue;
    }
    const Node& node = input.nodes()[index];
    if (is_binary(node.opcode)) {
      live[node.lhs.id()] = true;
      live[node.rhs.id()] = true;
    } else if (node.opcode == Opcode::kCall) {
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        live[argument.id()] = true;
      }
    } else if (node.opcode == Opcode::kGuardFloatNonzero) {
      live[node.lhs.id()] = true;
    }
  }

  std::vector<ValueType> parameter_types;
  parameter_types.reserve(input.parameter_count());
  for (std::size_t index = 0; index < input.parameter_count(); ++index) {
    parameter_types.push_back(input.parameter_type(index));
  }
  FunctionBuilder builder(std::move(parameter_types));
  std::vector<Value> mapped(node_count);
  std::vector<bool> known_constant(node_count, false);
  std::vector<Word> constant_value(node_count, 0);
  for (std::size_t index = 0; index < input.parameter_count(); ++index) {
    mapped[index] = builder.parameter(index);
  }

  std::size_t folded = 0;
  std::size_t simplified = 0;
  bool changed = false;
  for (std::size_t index = input.parameter_count(); index < node_count;
       ++index) {
    if (!live[index]) {
      changed = true;
      continue;
    }
    const Node& node = input.nodes()[index];
    if (node.opcode == Opcode::kConstant) {
      if (node.type == ValueType::kFloat64) {
        mapped[index] = builder.float64_constant_bits(node.immediate);
      } else {
        mapped[index] = builder.constant(node.immediate);
      }
      known_constant[index] = true;
      constant_value[index] = node.immediate;
      continue;
    }

    if (node.opcode == Opcode::kCall) {
      std::vector<Value> arguments;
      arguments.reserve(node.argument_count);
      for (std::size_t argument_index = 0;
           argument_index < node.argument_count; ++argument_index) {
        const Value argument = input.call_arguments()[
            static_cast<std::size_t>(node.argument_begin) + argument_index];
        arguments.push_back(mapped[argument.id()]);
      }
      mapped[index] = builder.call(unpack_runtime_helper(node.immediate),
                                   std::move(arguments), node.type);
      continue;
    }

    if (node.opcode == Opcode::kSafepoint) {
      mapped[index] = builder.safepoint(
          static_cast<std::size_t>(node.immediate));
      continue;
    }

    if (node.opcode == Opcode::kGuardFloatNonzero) {
      const std::size_t guarded_id = node.lhs.id();
      if (known_constant[guarded_id] &&
          unpack_float64(constant_value[guarded_id]) != 0.0) {
        mapped[index] = builder.constant(0);
        known_constant[index] = true;
        constant_value[index] = 0;
        ++simplified;
        changed = true;
      } else {
        mapped[index] = builder.guard_float64_nonzero(
            mapped[guarded_id], static_cast<std::size_t>(node.immediate));
      }
      continue;
    }

    const std::size_t lhs_id = node.lhs.id();
    const std::size_t rhs_id = node.rhs.id();
    if (is_float_binary(node.opcode)) {
      if (known_constant[lhs_id] && known_constant[rhs_id]) {
        constant_value[index] = fold_float_binary(
            node.opcode, constant_value[lhs_id], constant_value[rhs_id]);
        mapped[index] = builder.float64_constant_bits(constant_value[index]);
        known_constant[index] = true;
        ++folded;
        changed = true;
      } else if (node.opcode == Opcode::kFloatAdd) {
        mapped[index] = builder.float64_add(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatSubtract) {
        mapped[index] =
            builder.float64_subtract(mapped[lhs_id], mapped[rhs_id]);
      } else if (node.opcode == Opcode::kFloatMultiply) {
        mapped[index] =
            builder.float64_multiply(mapped[lhs_id], mapped[rhs_id]);
      } else {
        mapped[index] =
            builder.float64_divide(mapped[lhs_id], mapped[rhs_id]);
      }
      continue;
    }
    if (known_constant[lhs_id] && known_constant[rhs_id]) {
      constant_value[index] = fold_binary(
          node.opcode, constant_value[lhs_id], constant_value[rhs_id]);
      mapped[index] = builder.constant(constant_value[index]);
      known_constant[index] = true;
      ++folded;
      changed = true;
      continue;
    }

    std::size_t replacement = node_count;
    if (node.opcode == Opcode::kAdd) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = rhs_id;
      }
    } else if (node.opcode == Opcode::kSubtract) {
      if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = lhs_id;
      }
    } else if (node.opcode == Opcode::kMultiply) {
      if (known_constant[lhs_id] && constant_value[lhs_id] == 0) {
        replacement = lhs_id;
      } else if (known_constant[rhs_id] && constant_value[rhs_id] == 0) {
        replacement = rhs_id;
      } else if (known_constant[lhs_id] && constant_value[lhs_id] == 1) {
        replacement = rhs_id;
      } else if (known_constant[rhs_id] && constant_value[rhs_id] == 1) {
        replacement = lhs_id;
      }
    }

    if (replacement != node_count) {
      mapped[index] = mapped[replacement];
      known_constant[index] = known_constant[replacement];
      constant_value[index] = constant_value[replacement];
      ++simplified;
      changed = true;
      continue;
    }

    if (node.opcode == Opcode::kAdd) {
      mapped[index] = builder.add(mapped[lhs_id], mapped[rhs_id]);
    } else if (node.opcode == Opcode::kSubtract) {
      mapped[index] = builder.subtract(mapped[lhs_id], mapped[rhs_id]);
    } else {
      mapped[index] = builder.multiply(mapped[lhs_id], mapped[rhs_id]);
    }
  }

  const Status return_status =
      builder.set_return(mapped[input.return_value().id()]);
  if (!return_status.ok()) {
    return {};
  }
  return {std::move(builder).build(), folded, simplified, changed};
}

}  // namespace

OptimizationResult Optimizer::run(const Function& function) {
  const Status verification = verify(function);
  if (!verification.ok()) {
    return {verification, {}, {}};
  }

  try {
    const std::size_t input_nodes = function.nodes().size();
    PassResult pass = transform_once(function);
    std::size_t folded = pass.constants_folded;
    std::size_t simplified = pass.algebraic_simplifications;

    constexpr std::size_t kMaximumCanonicalizationPasses = 8;
    for (std::size_t iteration = 1;
         pass.changed && iteration < kMaximumCanonicalizationPasses;
         ++iteration) {
      PassResult next = transform_once(pass.function);
      folded += next.constants_folded;
      simplified += next.algebraic_simplifications;
      pass = std::move(next);
    }

    const Status output_verification = verify(pass.function);
    if (!output_verification.ok()) {
      return {{StatusCode::kCodeGenerationFailed,
               "optimizer produced invalid SSA", output_verification.location()},
              {}, {}};
    }
    const std::size_t output_nodes = pass.function.nodes().size();
    return {Status::ok_status(), std::move(pass.function),
            {input_nodes, output_nodes, folded, simplified}};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate SSA optimization state"},
            {}, {}};
  }
}

}  // namespace unijit::ir
