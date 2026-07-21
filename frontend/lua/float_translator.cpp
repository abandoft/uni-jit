#include "float_translator.h"

#include <cstddef>
#include <cstdio>
#include <new>
#include <utility>
#include <vector>

extern "C" {
#include "lobject.h"
#include "lopcodes.h"
}

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::frontend::lua55::detail {
namespace {

using ir::FunctionBuilder;
using ir::Value;
using ir::ValueType;
using ir::ControlFlowBuilder;
using jit::CompilationResult;
using jit::Compiler;

enum class NumericKind : unsigned char {
  kUndefined,
  kInteger,
  kFloat64,
};

CompilationResult translation_error(std::size_t pc, const char* message) {
  return {{StatusCode::kInvalidArgument, message, pc}, nullptr};
}

jit::CompilationOptions lua_compilation_options(
    jit::OptimizationLevel level, bool numeric_loop = false) noexcept {
  jit::CompilationOptions options{level};
  options.measure_safepoint_polls =
      numeric_loop && level == jit::OptimizationLevel::kBaseline;
  return options;
}

bool valid_register(const std::vector<Value>& registers, int index) {
  return index >= 0 && static_cast<std::size_t>(index) < registers.size() &&
         registers[static_cast<std::size_t>(index)].valid();
}

bool valid_destination(const std::vector<Value>& registers, int index) {
  return index >= 0 && static_cast<std::size_t>(index) < registers.size();
}

bool has_float_operand(NumericKind lhs, NumericKind rhs) {
  return lhs == NumericKind::kFloat64 || rhs == NumericKind::kFloat64;
}

Value append_float_binary(FunctionBuilder* builder, OpCode opcode, Value lhs,
                          Value rhs) {
  if (opcode == OP_ADD || opcode == OP_ADDK || opcode == OP_ADDI) {
    return builder->float64_add(lhs, rhs);
  }
  if (opcode == OP_SUB || opcode == OP_SUBK) {
    return builder->float64_subtract(lhs, rhs);
  }
  if (opcode == OP_MUL || opcode == OP_MULK) {
    return builder->float64_multiply(lhs, rhs);
  }
  return builder->float64_divide(lhs, rhs);
}

Value append_float_binary(ControlFlowBuilder* builder, OpCode opcode,
                          Value lhs, Value rhs) {
  if (opcode == OP_ADD || opcode == OP_ADDK || opcode == OP_ADDI) {
    return builder->float64_add(lhs, rhs);
  }
  if (opcode == OP_SUB || opcode == OP_SUBK) {
    return builder->float64_subtract(lhs, rhs);
  }
  if (opcode == OP_MUL || opcode == OP_MULK) {
    return builder->float64_multiply(lhs, rhs);
  }
  return builder->float64_divide(lhs, rhs);
}

Status bytecode_error(int pc, const char* message) {
  return {StatusCode::kInvalidArgument, message,
          static_cast<std::size_t>(pc)};
}

bool writes_float_destination(OpCode opcode) {
  return opcode == OP_MOVE || opcode == OP_LOADI || opcode == OP_LOADF ||
         opcode == OP_LOADK || opcode == OP_ADDI || opcode == OP_ADDK ||
         opcode == OP_SUBK || opcode == OP_MULK || opcode == OP_DIVK ||
         opcode == OP_ADD || opcode == OP_SUB || opcode == OP_MUL ||
         opcode == OP_DIV || opcode == OP_UNM;
}

template <typename Builder>
Status translate_float_range(const Proto& prototype, Builder* builder,
                             int begin, int end, bool allow_return,
                             int protected_state_base,
                             std::vector<Value>* registers,
                             std::vector<NumericKind>* kinds,
                             bool* returned) {
  for (int pc = begin; pc < end; ++pc) {
    const Instruction instruction = prototype.code[pc];
    const OpCode opcode = GET_OPCODE(instruction);
    const int destination = GETARG_A(instruction);
    if (writes_float_destination(opcode) && protected_state_base >= 0 &&
        destination >= protected_state_base &&
        destination <= protected_state_base + 2) {
      return bytecode_error(
          pc, "Float64 numeric for body modifies an internal loop register");
    }

    switch (opcode) {
      case OP_MOVE: {
        const int source = GETARG_B(instruction);
        if (!valid_destination(*registers, destination) ||
            !valid_register(*registers, source)) {
          return bytecode_error(pc, "invalid register in OP_MOVE");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            (*registers)[static_cast<std::size_t>(source)];
        (*kinds)[static_cast<std::size_t>(destination)] =
            (*kinds)[static_cast<std::size_t>(source)];
        break;
      }

      case OP_LOADI:
      case OP_LOADF:
        if (!valid_destination(*registers, destination)) {
          return bytecode_error(pc, "invalid numeric load destination");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            builder->float64_constant(
                static_cast<double>(GETARG_sBx(instruction)));
        (*kinds)[static_cast<std::size_t>(destination)] =
            opcode == OP_LOADF ? NumericKind::kFloat64
                               : NumericKind::kInteger;
        break;

      case OP_LOADK: {
        const int constant_index = GETARG_Bx(instruction);
        if (!valid_destination(*registers, destination) ||
            constant_index < 0 || constant_index >= prototype.sizek) {
          return bytecode_error(pc, "invalid operand in OP_LOADK");
        }
        const TValue* constant = &prototype.k[constant_index];
        double value = 0.0;
        NumericKind kind = NumericKind::kUndefined;
        if (ttisinteger(constant)) {
          value = static_cast<double>(ivalue(constant));
          kind = NumericKind::kInteger;
        } else if (ttisfloat(constant)) {
          value = static_cast<double>(fltvalue(constant));
          kind = NumericKind::kFloat64;
        } else {
          return bytecode_error(pc, "Float64 compilation requires a number");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            builder->float64_constant(value);
        (*kinds)[static_cast<std::size_t>(destination)] = kind;
        break;
      }

      case OP_ADDI: {
        const int source = GETARG_B(instruction);
        if (!valid_destination(*registers, destination) ||
            !valid_register(*registers, source)) {
          return bytecode_error(pc, "invalid register in OP_ADDI");
        }
        if ((*kinds)[static_cast<std::size_t>(source)] !=
            NumericKind::kFloat64) {
          return bytecode_error(
              pc, "Float64 arithmetic requires at least one Float64 operand");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            append_float_binary(
                builder, opcode,
                (*registers)[static_cast<std::size_t>(source)],
                builder->float64_constant(
                    static_cast<double>(GETARG_sC(instruction))));
        (*kinds)[static_cast<std::size_t>(destination)] =
            NumericKind::kFloat64;
        if (pc + 1 >= end ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBINI) {
          return bytecode_error(
              pc, "OP_ADDI is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_ADDK:
      case OP_SUBK:
      case OP_MULK:
      case OP_DIVK: {
        const int source = GETARG_B(instruction);
        const int constant_index = GETARG_C(instruction);
        if (!valid_destination(*registers, destination) ||
            !valid_register(*registers, source) || constant_index < 0 ||
            constant_index >= prototype.sizek) {
          return bytecode_error(pc, "invalid operand in constant arithmetic");
        }
        const TValue* constant = &prototype.k[constant_index];
        double value = 0.0;
        NumericKind constant_kind = NumericKind::kUndefined;
        if (ttisinteger(constant)) {
          value = static_cast<double>(ivalue(constant));
          constant_kind = NumericKind::kInteger;
        } else if (ttisfloat(constant)) {
          value = static_cast<double>(fltvalue(constant));
          constant_kind = NumericKind::kFloat64;
        } else {
          return bytecode_error(
              pc, "Float64 constant arithmetic requires a number");
        }
        const NumericKind source_kind =
            (*kinds)[static_cast<std::size_t>(source)];
        if (opcode != OP_DIVK &&
            !has_float_operand(source_kind, constant_kind)) {
          return bytecode_error(
              pc, "Float64 arithmetic requires at least one Float64 operand");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            append_float_binary(
                builder, opcode,
                (*registers)[static_cast<std::size_t>(source)],
                builder->float64_constant(value));
        (*kinds)[static_cast<std::size_t>(destination)] =
            NumericKind::kFloat64;
        if (pc + 1 >= end ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBINK) {
          return bytecode_error(
              pc, "constant arithmetic is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        const int lhs_index = GETARG_B(instruction);
        const int rhs_index = GETARG_C(instruction);
        if (!valid_destination(*registers, destination) ||
            !valid_register(*registers, lhs_index) ||
            !valid_register(*registers, rhs_index)) {
          return bytecode_error(pc, "invalid register in binary arithmetic");
        }
        const NumericKind lhs_kind =
            (*kinds)[static_cast<std::size_t>(lhs_index)];
        const NumericKind rhs_kind =
            (*kinds)[static_cast<std::size_t>(rhs_index)];
        if (opcode != OP_DIV && !has_float_operand(lhs_kind, rhs_kind)) {
          return bytecode_error(
              pc, "Float64 arithmetic requires at least one Float64 operand");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            append_float_binary(
                builder, opcode,
                (*registers)[static_cast<std::size_t>(lhs_index)],
                (*registers)[static_cast<std::size_t>(rhs_index)]);
        (*kinds)[static_cast<std::size_t>(destination)] =
            NumericKind::kFloat64;
        if (pc + 1 >= end ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBIN) {
          return bytecode_error(
              pc, "binary arithmetic is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_UNM: {
        const int source = GETARG_B(instruction);
        if (!valid_destination(*registers, destination) ||
            !valid_register(*registers, source)) {
          return bytecode_error(pc, "invalid register in Float64 OP_UNM");
        }
        if ((*kinds)[static_cast<std::size_t>(source)] !=
            NumericKind::kFloat64) {
          return bytecode_error(
              pc, "Float64 unary minus requires a Float64 operand");
        }
        (*registers)[static_cast<std::size_t>(destination)] =
            builder->float64_negate(
                (*registers)[static_cast<std::size_t>(source)]);
        (*kinds)[static_cast<std::size_t>(destination)] =
            NumericKind::kFloat64;
        break;
      }

      case OP_RETURN1:
      case OP_RETURN:
        if (!allow_return ||
            (opcode == OP_RETURN &&
             (GETARG_B(instruction) != 2 || GETARG_k(instruction) != 0)) ||
            !valid_register(*registers, destination)) {
          return bytecode_error(
              pc, "only one-value fixed Float64 returns are supported");
        }
        if ((*kinds)[static_cast<std::size_t>(destination)] !=
            NumericKind::kFloat64) {
          return bytecode_error(pc, "compiled result is not Float64");
        }
        if (!builder
                 ->set_return(
                     (*registers)[static_cast<std::size_t>(destination)])
                 .ok()) {
          return bytecode_error(pc,
                                "unable to record the Lua return value");
        }
        *returned = true;
        return Status::ok_status();

      default: {
        char message[96] = {};
        std::snprintf(message, sizeof(message),
                      protected_state_base >= 0
                          ? "unsupported Lua 5.5 opcode %d in Float64 numeric loop"
                          : "unsupported Lua 5.5 opcode %d",
                      static_cast<int>(opcode));
        return bytecode_error(pc, message);
      }
    }
  }
  return Status::ok_status();
}

CompilationResult compile_float64_numeric_for_prototype(
    const Proto& prototype, jit::OptimizationLevel optimization_level) {
  int preparation_pc = -1;
  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    if (GET_OPCODE(prototype.code[pc]) != OP_FORPREP) {
      continue;
    }
    if (preparation_pc >= 0) {
      return translation_error(static_cast<std::size_t>(pc),
                               "only one Float64 numeric for loop is supported");
    }
    preparation_pc = pc;
  }
  if (preparation_pc < 0) {
    return translation_error(0, "Lua function has no Float64 numeric for loop");
  }

  const Instruction preparation = prototype.code[preparation_pc];
  const int loop_pc = preparation_pc + GETARG_Bx(preparation) + 1;
  if (loop_pc <= preparation_pc || loop_pc >= prototype.sizecode ||
      GET_OPCODE(prototype.code[loop_pc]) != OP_FORLOOP ||
      GETARG_Bx(prototype.code[loop_pc]) != loop_pc - preparation_pc) {
    return translation_error(static_cast<std::size_t>(preparation_pc),
                             "Float64 numeric for loop has malformed control flow");
  }

  const int state_base = GETARG_A(preparation);
  if (state_base < 0 || state_base + 2 >= prototype.maxstacksize ||
      GETARG_A(prototype.code[loop_pc]) != state_base) {
    return translation_error(static_cast<std::size_t>(preparation_pc),
                             "Float64 numeric for loop has invalid state registers");
  }

  try {
    ControlFlowBuilder builder(
        std::vector<ValueType>(prototype.numparams, ValueType::kFloat64));
    std::vector<Value> registers(prototype.maxstacksize);
    std::vector<NumericKind> kinds(prototype.maxstacksize,
                                   NumericKind::kUndefined);
    for (std::size_t index = 0; index < prototype.numparams; ++index) {
      registers[index] = builder.parameter(index);
      kinds[index] = NumericKind::kFloat64;
    }

    bool returned = false;
    Status status = translate_float_range(
        prototype, &builder, 0, preparation_pc, false, -1, &registers,
        &kinds, &returned);
    if (!status.ok()) {
      return {status, nullptr};
    }
    if (!valid_register(registers, state_base) ||
        !valid_register(registers, state_base + 1) ||
        !valid_register(registers, state_base + 2) ||
        kinds[static_cast<std::size_t>(state_base)] ==
            NumericKind::kUndefined ||
        kinds[static_cast<std::size_t>(state_base + 1)] ==
            NumericKind::kUndefined ||
        kinds[static_cast<std::size_t>(state_base + 2)] ==
            NumericKind::kUndefined) {
      return translation_error(static_cast<std::size_t>(preparation_pc),
                               "Float64 numeric loop has undefined controls");
    }
    if (kinds[static_cast<std::size_t>(state_base)] !=
            NumericKind::kFloat64 &&
        kinds[static_cast<std::size_t>(state_base + 2)] !=
            NumericKind::kFloat64) {
      return translation_error(
          static_cast<std::size_t>(preparation_pc),
          "compile_float requires a Float64 loop start or step");
    }

    const Value start = registers[static_cast<std::size_t>(state_base)];
    const Value limit = registers[static_cast<std::size_t>(state_base + 1)];
    const Value step = registers[static_cast<std::size_t>(state_base + 2)];
    const std::size_t step_guard_site =
        static_cast<std::size_t>(prototype.sizecode) +
        static_cast<std::size_t>(preparation_pc);
    if (!builder
             .guard_float64_nonzero(step, step_guard_site)
             .valid()) {
      return translation_error(static_cast<std::size_t>(preparation_pc),
                               "unable to guard a Float64 loop step");
    }

    std::vector<bool> needed_registers(prototype.maxstacksize, false);
    const auto note_register_reads = [&](int begin, int end) {
      for (int pc = begin; pc < end; ++pc) {
        const Instruction instruction = prototype.code[pc];
        const OpCode opcode = GET_OPCODE(instruction);
        const auto note = [&](int index) {
          if (index >= 0 && index < prototype.maxstacksize) {
            needed_registers[static_cast<std::size_t>(index)] = true;
          }
        };
        if (opcode == OP_MOVE || opcode == OP_ADDI || opcode == OP_ADDK ||
            opcode == OP_SUBK || opcode == OP_MULK || opcode == OP_DIVK ||
            opcode == OP_UNM) {
          note(GETARG_B(instruction));
        } else if (opcode == OP_ADD || opcode == OP_SUB ||
                   opcode == OP_MUL || opcode == OP_DIV) {
          note(GETARG_B(instruction));
          note(GETARG_C(instruction));
        } else if (opcode == OP_RETURN || opcode == OP_RETURN1) {
          note(GETARG_A(instruction));
        }
      }
    };
    note_register_reads(preparation_pc + 1, loop_pc);
    note_register_reads(loop_pc + 1, prototype.sizecode);
    for (int offset = 0; offset < 3; ++offset) {
      needed_registers[static_cast<std::size_t>(state_base + offset)] = true;
    }

    std::vector<std::size_t> carried_registers;
    std::vector<NumericKind> carried_kinds;
    for (std::size_t index = 0; index < registers.size(); ++index) {
      if (!registers[index].valid() || !needed_registers[index]) {
        continue;
      }
      carried_registers.push_back(index);
      carried_kinds.push_back(
          index >= static_cast<std::size_t>(state_base) &&
                  index <= static_cast<std::size_t>(state_base + 2)
              ? NumericKind::kFloat64
              : kinds[index]);
    }

    const std::vector<ValueType> block_types(carried_registers.size(),
                                              ValueType::kFloat64);
    const ir::Block positive_entry = builder.create_block(0);
    const ir::Block nonpositive_entry = builder.create_block(0);
    constexpr std::size_t kOptimizedUnrollFactor = 8;
    const bool unroll_loop =
        optimization_level != jit::OptimizationLevel::kBaseline;
    std::vector<ValueType> unrolled_types = block_types;
    unrolled_types.insert(unrolled_types.end(),
                          kOptimizedUnrollFactor - 1,
                          ValueType::kFloat64);
    const ir::Block positive_dispatch = builder.create_block(block_types);
    const ir::Block nonpositive_dispatch = builder.create_block(block_types);
    const ir::Block positive_unrolled =
        unroll_loop ? builder.create_block(unrolled_types) : ir::Block{};
    const ir::Block nonpositive_unrolled =
        unroll_loop ? builder.create_block(unrolled_types) : ir::Block{};
    const ir::Block positive_scalar = builder.create_block(block_types);
    const ir::Block nonpositive_scalar = builder.create_block(block_types);
    const ir::Block exit = builder.create_block(block_types);
    if (!positive_entry.valid() || !nonpositive_entry.valid() ||
        !positive_dispatch.valid() || !nonpositive_dispatch.valid() ||
        (unroll_loop &&
         (!positive_unrolled.valid() || !nonpositive_unrolled.valid())) ||
        !positive_scalar.valid() || !nonpositive_scalar.valid() ||
        !exit.valid()) {
      return {{StatusCode::kResourceExhausted,
               "unable to allocate Float64 numeric-loop blocks"},
              nullptr};
    }

    std::vector<Value> initial_arguments;
    std::vector<Value> skipped_arguments;
    initial_arguments.reserve(carried_registers.size());
    skipped_arguments.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      skipped_arguments.push_back(registers[lua_register]);
      if (lua_register == static_cast<std::size_t>(state_base)) {
        initial_arguments.push_back(limit);
      } else if (lua_register == static_cast<std::size_t>(state_base + 1)) {
        initial_arguments.push_back(step);
      } else if (lua_register == static_cast<std::size_t>(state_base + 2)) {
        initial_arguments.push_back(start);
      } else {
        initial_arguments.push_back(registers[lua_register]);
      }
    }

    const Value zero_float = builder.float64_constant(0.0);
    const Value zero_word = builder.constant(0);
    const Value step_positive = builder.float64_less_than(zero_float, step);
    status = builder.branch(step_positive, positive_entry, {},
                            nonpositive_entry, {});
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(positive_entry);
    if (!status.ok()) {
      return {status, nullptr};
    }
    const Value positive_enters = builder.equal(
        builder.float64_less_than(limit, start), zero_word);
    status = builder.branch(positive_enters, positive_dispatch,
                            initial_arguments, exit,
                            skipped_arguments);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(nonpositive_entry);
    if (!status.ok()) {
      return {status, nullptr};
    }
    const Value nonpositive_enters = builder.equal(
        builder.float64_less_than(start, limit), zero_word);
    status = builder.branch(nonpositive_enters, nonpositive_dispatch,
                            initial_arguments, exit,
                            skipped_arguments);
    if (!status.ok()) {
      return {status, nullptr};
    }

    const auto block_registers = [&](ir::Block block) {
      std::vector<Value> values(prototype.maxstacksize);
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        values[carried_registers[index]] =
            builder.block_parameter(block, index);
      }
      return values;
    };
    const auto block_kinds = [&]() {
      std::vector<NumericKind> values(prototype.maxstacksize,
                                      NumericKind::kUndefined);
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        values[carried_registers[index]] = carried_kinds[index];
      }
      return values;
    };
    const auto collect_carried =
        [&](const std::vector<Value>& values,
            const std::vector<NumericKind>& value_kinds,
            std::vector<Value>* completed) -> Status {
      completed->clear();
      completed->reserve(carried_registers.size());
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        const std::size_t lua_register = carried_registers[index];
        if (!values[lua_register].valid()) {
          return bytecode_error(
              loop_pc,
              "Float64 numeric loop has an undefined carried register");
        }
        if (value_kinds[lua_register] != carried_kinds[index]) {
          return bytecode_error(
              loop_pc,
              "Float64 numeric loop changes a carried Lua numeric tag");
        }
        completed->push_back(values[lua_register]);
      }
      return Status::ok_status();
    };

    const auto block_arguments = [&](ir::Block block) {
      std::vector<Value> arguments;
      arguments.reserve(carried_registers.size());
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        arguments.push_back(builder.block_parameter(block, index));
      }
      return arguments;
    };
    const auto next_arguments = [&](const std::vector<Value>& completed,
                                    Value next_index) {
      std::vector<Value> arguments = completed;
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        if (carried_registers[index] ==
            static_cast<std::size_t>(state_base + 2)) {
          arguments[index] = next_index;
        }
      }
      return arguments;
    };
    const auto emit_dispatch = [&](ir::Block dispatch, ir::Block unrolled,
                                   ir::Block scalar,
                                   bool positive,
                                   std::size_t safepoint_site) -> Status {
      Status dispatch_status = builder.set_insertion_block(dispatch);
      if (!dispatch_status.ok()) {
        return dispatch_status;
      }
      if (!builder.safepoint(safepoint_site).valid()) {
        return {StatusCode::kInvalidArgument,
                "unable to insert a Float64 loop safepoint"};
      }
      std::vector<Value> arguments = block_arguments(dispatch);
      if (!unroll_loop) {
        return builder.jump(scalar, arguments);
      }
      const std::vector<Value> scalar_arguments = arguments;
      const std::vector<Value> dispatch_registers = block_registers(dispatch);
      const Value dispatch_limit =
          dispatch_registers[static_cast<std::size_t>(state_base)];
      const Value dispatch_step =
          dispatch_registers[static_cast<std::size_t>(state_base + 1)];
      Value last_index =
          dispatch_registers[static_cast<std::size_t>(state_base + 2)];
      for (std::size_t index = 1; index < kOptimizedUnrollFactor; ++index) {
        last_index = builder.float64_add(last_index, dispatch_step);
        arguments.push_back(last_index);
      }
      const Value has_full_group =
          positive
              ? builder.float64_less_equal(last_index, dispatch_limit)
              : builder.float64_less_equal(dispatch_limit, last_index);
      return builder.branch(has_full_group, unrolled, arguments, scalar,
                            scalar_arguments);
    };
    status = emit_dispatch(positive_dispatch, positive_unrolled,
                           positive_scalar, true,
                           static_cast<std::size_t>(preparation_pc));
    if (!status.ok()) {
      return {status, nullptr};
    }
    status = emit_dispatch(nonpositive_dispatch, nonpositive_unrolled,
                           nonpositive_scalar, false,
                           2 * static_cast<std::size_t>(prototype.sizecode) +
                               static_cast<std::size_t>(preparation_pc));
    if (!status.ok()) {
      return {status, nullptr};
    }

    const auto emit_iteration = [&](ir::Block iteration,
                                    ir::Block dispatch,
                                    bool positive,
                                    std::size_t count) -> Status {
      Status iteration_status = builder.set_insertion_block(iteration);
      if (!iteration_status.ok()) {
        return iteration_status;
      }
      std::vector<Value> iteration_registers = block_registers(iteration);
      std::vector<NumericKind> iteration_kinds = block_kinds();
      for (std::size_t index = 0; index < count; ++index) {
        returned = false;
        iteration_status = translate_float_range(
            prototype, &builder, preparation_pc + 1, loop_pc, false,
            state_base, &iteration_registers, &iteration_kinds, &returned);
        if (!iteration_status.ok()) {
          return iteration_status;
        }
        if (index + 1 < count) {
          iteration_registers[static_cast<std::size_t>(state_base + 2)] =
              builder.block_parameter(iteration,
                                      carried_registers.size() + index);
        }
      }

      std::vector<Value> completed;
      iteration_status =
          collect_carried(iteration_registers, iteration_kinds, &completed);
      if (!iteration_status.ok()) {
        return iteration_status;
      }
      const Value current_index =
          iteration_registers[static_cast<std::size_t>(state_base + 2)];
      const Value loop_limit =
          iteration_registers[static_cast<std::size_t>(state_base)];
      const Value loop_step =
          iteration_registers[static_cast<std::size_t>(state_base + 1)];
      const Value next_index = builder.float64_add(current_index, loop_step);
      const Value continues =
          positive ? builder.float64_less_equal(next_index, loop_limit)
                   : builder.float64_less_equal(loop_limit, next_index);
      return builder.branch(continues, dispatch,
                            next_arguments(completed, next_index), exit,
                            completed);
    };
    if (unroll_loop) {
      status = emit_iteration(positive_unrolled, positive_dispatch, true,
                              kOptimizedUnrollFactor);
      if (!status.ok()) {
        return {status, nullptr};
      }
      status = emit_iteration(nonpositive_unrolled, nonpositive_dispatch,
                              false, kOptimizedUnrollFactor);
      if (!status.ok()) {
        return {status, nullptr};
      }
    }
    status = emit_iteration(positive_scalar, positive_dispatch, true, 1);
    if (!status.ok()) {
      return {status, nullptr};
    }
    status = emit_iteration(nonpositive_scalar, nonpositive_dispatch, false,
                            1);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(exit);
    if (!status.ok()) {
      return {status, nullptr};
    }
    std::vector<Value> exit_registers = block_registers(exit);
    std::vector<NumericKind> exit_kinds = block_kinds();
    for (int offset = 0; offset < 3; ++offset) {
      exit_kinds[static_cast<std::size_t>(state_base + offset)] =
          NumericKind::kUndefined;
    }
    returned = false;
    status = translate_float_range(
        prototype, &builder, loop_pc + 1, prototype.sizecode, true, -1,
        &exit_registers, &exit_kinds, &returned);
    if (!status.ok()) {
      return {status, nullptr};
    }
    if (!returned) {
      return translation_error(static_cast<std::size_t>(prototype.sizecode),
                               "Float64 numeric loop has no supported return");
    }
    return Compiler::compile(
        std::move(builder).build(),
        lua_compilation_options(optimization_level, true));
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua Float64 numeric-loop translation state"},
            nullptr};
  }
}

}  // namespace

CompilationResult compile_float64_prototype(
    const Proto& prototype, jit::OptimizationLevel optimization_level) {
  if (isvararg(&prototype)) {
    return translation_error(0, "vararg Lua functions are not supported");
  }
  if (prototype.numparams > prototype.maxstacksize) {
    return translation_error(0, "Lua prototype has an invalid stack layout");
  }

  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    if (GET_OPCODE(prototype.code[pc]) == OP_FORPREP) {
      return compile_float64_numeric_for_prototype(prototype,
                                                   optimization_level);
    }
  }

  try {
    FunctionBuilder builder(
        std::vector<ValueType>(prototype.numparams, ValueType::kFloat64));
    std::vector<Value> registers(prototype.maxstacksize);
    std::vector<NumericKind> kinds(prototype.maxstacksize,
                                   NumericKind::kUndefined);
    for (std::size_t index = 0; index < prototype.numparams; ++index) {
      registers[index] = builder.parameter(index);
      kinds[index] = NumericKind::kFloat64;
    }

    bool has_return = false;
    const Status status = translate_float_range(
        prototype, &builder, 0, prototype.sizecode, true, -1, &registers,
        &kinds, &has_return);
    if (!status.ok()) {
      return {status, nullptr};
    }
    if (!has_return) {
      return translation_error(static_cast<std::size_t>(prototype.sizecode),
                               "Lua function has no supported return");
    }
    return Compiler::compile(std::move(builder).build(),
                             lua_compilation_options(optimization_level));
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua Float64 translation state"},
            nullptr};
  }
}

}  // namespace unijit::frontend::lua55::detail
