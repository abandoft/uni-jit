#include "unijit_lua.h"

#include "float_translator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
}

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/status.h"

namespace {

using unijit::Status;
using unijit::StatusCode;
using unijit::ir::FunctionBuilder;
using unijit::ir::ControlFlowBuilder;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::CompilationResult;
using unijit::jit::CodeCache;
using unijit::jit::CodeHandle;
using unijit::jit::CompiledFunction;
using unijit::jit::Compiler;

constexpr char kCompiledFunctionMetatable[] = "unijit.lua55.compiled";
constexpr std::size_t kMaximumLuaParameters = 255;

static_assert(sizeof(lua_Integer) == sizeof(Word),
              "Lua and UniJIT integers must have the same width");
static_assert(sizeof(lua_Number) == sizeof(double),
              "Lua and UniJIT Float64 values must have the same width");

enum class NumericMode : unsigned char {
  kInteger,
  kFloat64,
};

struct OwnedFunction final {
  CodeHandle *function{nullptr};
  std::size_t parameter_count{0};
  NumericMode mode{NumericMode::kInteger};
};

constexpr std::uint64_t kIntegerCacheFingerprint = 0x554A4C5541494E54ULL;
constexpr std::uint64_t kFloatCacheFingerprint = 0x554A4C5541464C54ULL;

CodeCache &compiled_function_cache(NumericMode mode) {
  static CodeCache integer_cache;
  static CodeCache float_cache;
  return mode == NumericMode::kInteger ? integer_cache : float_cache;
}

CodeCache &uncached_publication_cache() {
  static CodeCache cache(unijit::jit::CodeCacheLimits{0, 0});
  return cache;
}

template <typename T>
void append_binary(std::string *output, const T &value) {
  output->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

bool prototype_cache_key(const Proto &prototype, NumericMode mode,
                         std::string *output) {
  if (output == nullptr || prototype.sizecode < 0 || prototype.sizek < 0 ||
      prototype.sizeupvalues < 0 || prototype.sizep != 0 ||
      (prototype.sizecode != 0 && prototype.code == nullptr) ||
      (prototype.sizek != 0 && prototype.k == nullptr)) {
    return false;
  }
  try {
    output->clear();
    output->reserve(sizeof(Instruction) *
                        static_cast<std::size_t>(prototype.sizecode) +
                    sizeof(TValue) *
                        static_cast<std::size_t>(prototype.sizek) +
                    32U);
    const auto numeric_mode = static_cast<unsigned char>(mode);
    append_binary(output, numeric_mode);
    append_binary(output, prototype.numparams);
    append_binary(output, prototype.flag);
    append_binary(output, prototype.maxstacksize);
    append_binary(output, prototype.sizeupvalues);
    append_binary(output, prototype.sizek);
    append_binary(output, prototype.sizecode);
    if (prototype.sizecode != 0) {
      output->append(
          reinterpret_cast<const char *>(prototype.code),
          sizeof(Instruction) * static_cast<std::size_t>(prototype.sizecode));
    }
    for (int index = 0; index < prototype.sizek; ++index) {
      const TValue *constant = &prototype.k[index];
      const int tag = rawtt(constant);
      append_binary(output, tag);
      if (ttisinteger(constant)) {
        const lua_Integer value = ivalue(constant);
        append_binary(output, value);
      } else if (ttisfloat(constant)) {
        const lua_Number value = fltvalue(constant);
        append_binary(output, value);
      } else {
        output->clear();
        return false;
      }
    }
    return true;
  } catch (const std::bad_alloc &) {
    output->clear();
    return false;
  }
}

CompilationResult translation_error(std::size_t pc, const char *message) {
  return {{StatusCode::kInvalidArgument, message, pc}, nullptr};
}

bool valid_register(const std::vector<Value> &registers, int index) {
  return index >= 0 && static_cast<std::size_t>(index) < registers.size() &&
         registers[static_cast<std::size_t>(index)].valid();
}

bool valid_destination(const std::vector<Value> &registers, int index) {
  return index >= 0 && static_cast<std::size_t>(index) < registers.size();
}

CompilationResult compile_straight_prototype(const Proto &prototype) {
  if (isvararg(&prototype)) {
    return translation_error(0, "vararg Lua functions are not supported");
  }
  if (prototype.numparams > prototype.maxstacksize) {
    return translation_error(0, "Lua prototype has an invalid stack layout");
  }

  try {
    FunctionBuilder builder(prototype.numparams);
    std::vector<Value> registers(prototype.maxstacksize);
    for (std::size_t index = 0; index < prototype.numparams; ++index) {
      registers[index] = builder.parameter(index);
    }

    bool has_return = false;
    for (int pc = 0; pc < prototype.sizecode; ++pc) {
      const Instruction instruction = prototype.code[pc];
      const OpCode opcode = GET_OPCODE(instruction);
      const int destination = GETARG_A(instruction);

      switch (opcode) {
      case OP_MOVE: {
        const int source = GETARG_B(instruction);
        if (!valid_destination(registers, destination) ||
            !valid_register(registers, source)) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid register in OP_MOVE");
        }
        registers[static_cast<std::size_t>(destination)] =
            registers[static_cast<std::size_t>(source)];
        break;
      }

      case OP_LOADI:
        if (!valid_destination(registers, destination)) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid destination in OP_LOADI");
        }
        registers[static_cast<std::size_t>(destination)] =
            builder.constant(static_cast<Word>(GETARG_sBx(instruction)));
        break;

      case OP_LOADK: {
        const int constant_index = GETARG_Bx(instruction);
        if (!valid_destination(registers, destination) || constant_index < 0 ||
            constant_index >= prototype.sizek) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid operand in OP_LOADK");
        }
        const TValue *constant = &prototype.k[constant_index];
        if (!ttisinteger(constant)) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "only integer constants are supported in OP_LOADK");
        }
        registers[static_cast<std::size_t>(destination)] =
            builder.constant(static_cast<Word>(ivalue(constant)));
        break;
      }

      case OP_ADDI: {
        const int source = GETARG_B(instruction);
        if (!valid_destination(registers, destination) ||
            !valid_register(registers, source)) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid register in OP_ADDI");
        }
        registers[static_cast<std::size_t>(destination)] = builder.add(
            registers[static_cast<std::size_t>(source)],
            builder.constant(static_cast<Word>(GETARG_sC(instruction))));
        if (pc + 1 >= prototype.sizecode ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBINI) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "OP_ADDI is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_ADDK:
      case OP_SUBK:
      case OP_MULK: {
        const int source = GETARG_B(instruction);
        const int constant_index = GETARG_C(instruction);
        if (!valid_destination(registers, destination) ||
            !valid_register(registers, source) || constant_index < 0 ||
            constant_index >= prototype.sizek) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid operand in constant arithmetic");
        }
        const TValue *constant = &prototype.k[constant_index];
        if (!ttisinteger(constant)) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "constant arithmetic requires an integer constant");
        }
        const Value lhs = registers[static_cast<std::size_t>(source)];
        const Value rhs = builder.constant(static_cast<Word>(ivalue(constant)));
        if (opcode == OP_ADDK) {
          registers[static_cast<std::size_t>(destination)] =
              builder.add(lhs, rhs);
        } else if (opcode == OP_SUBK) {
          registers[static_cast<std::size_t>(destination)] =
              builder.subtract(lhs, rhs);
        } else {
          registers[static_cast<std::size_t>(destination)] =
              builder.multiply(lhs, rhs);
        }
        if (pc + 1 >= prototype.sizecode ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBINK) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "constant arithmetic is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_ADD:
      case OP_SUB:
      case OP_MUL: {
        const int lhs_index = GETARG_B(instruction);
        const int rhs_index = GETARG_C(instruction);
        if (!valid_destination(registers, destination) ||
            !valid_register(registers, lhs_index) ||
            !valid_register(registers, rhs_index)) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid register in binary arithmetic");
        }
        const Value lhs = registers[static_cast<std::size_t>(lhs_index)];
        const Value rhs = registers[static_cast<std::size_t>(rhs_index)];
        if (opcode == OP_ADD) {
          registers[static_cast<std::size_t>(destination)] =
              builder.add(lhs, rhs);
        } else if (opcode == OP_SUB) {
          registers[static_cast<std::size_t>(destination)] =
              builder.subtract(lhs, rhs);
        } else {
          registers[static_cast<std::size_t>(destination)] =
              builder.multiply(lhs, rhs);
        }
        if (pc + 1 >= prototype.sizecode ||
            GET_OPCODE(prototype.code[pc + 1]) != OP_MMBIN) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "binary arithmetic is missing its Lua metamethod fallback");
        }
        ++pc;
        break;
      }

      case OP_RETURN1:
        if (!valid_register(registers, destination)) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "invalid return register");
        }
        if (!builder
                 .set_return(registers[static_cast<std::size_t>(destination)])
                 .ok()) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "unable to record the Lua return value");
        }
        has_return = true;
        pc = prototype.sizecode;
        break;

      case OP_RETURN:
        if (GETARG_B(instruction) != 2 || GETARG_k(instruction) != 0 ||
            !valid_register(registers, destination)) {
          return translation_error(
              static_cast<std::size_t>(pc),
              "only one-value fixed Lua returns are supported");
        }
        if (!builder
                 .set_return(registers[static_cast<std::size_t>(destination)])
                 .ok()) {
          return translation_error(static_cast<std::size_t>(pc),
                                   "unable to record the Lua return value");
        }
        has_return = true;
        pc = prototype.sizecode;
        break;

      default: {
        char message[96] = {};
        std::snprintf(message, sizeof(message), "unsupported Lua 5.5 opcode %d",
                      static_cast<int>(opcode));
        return translation_error(static_cast<std::size_t>(pc), message);
      }
      }
    }

    if (!has_return) {
      return translation_error(static_cast<std::size_t>(prototype.sizecode),
                               "Lua function has no supported return");
    }
    return Compiler::compile(std::move(builder).build());
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua frontend translation state"},
            nullptr};
  }
}

CompilationResult compile_numeric_for_prototype(const Proto &prototype) {
  if (isvararg(&prototype)) {
    return translation_error(0, "vararg Lua functions are not supported");
  }
  if (prototype.numparams > prototype.maxstacksize) {
    return translation_error(0, "Lua prototype has an invalid stack layout");
  }

  int preparation_pc = -1;
  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    if (GET_OPCODE(prototype.code[pc]) == OP_FORPREP) {
      if (preparation_pc >= 0) {
        return translation_error(static_cast<std::size_t>(pc),
                                 "only one numeric for loop is supported");
      }
      preparation_pc = pc;
    }
  }
  if (preparation_pc < 0) {
    return translation_error(0, "Lua function has no numeric for loop");
  }

  const Instruction preparation = prototype.code[preparation_pc];
  const int loop_pc =
      preparation_pc + GETARG_Bx(preparation) + 1;
  if (loop_pc <= preparation_pc || loop_pc >= prototype.sizecode ||
      GET_OPCODE(prototype.code[loop_pc]) != OP_FORLOOP ||
      GETARG_Bx(prototype.code[loop_pc]) != loop_pc - preparation_pc) {
    return translation_error(static_cast<std::size_t>(preparation_pc),
                             "numeric for loop has malformed control flow");
  }

  const int state_base = GETARG_A(preparation);
  if (state_base < 0 || state_base + 2 >= prototype.maxstacksize ||
      GETARG_A(prototype.code[loop_pc]) != state_base) {
    return translation_error(static_cast<std::size_t>(preparation_pc),
                             "numeric for loop has invalid state registers");
  }

  try {
    ControlFlowBuilder builder(prototype.numparams);
    std::vector<Value> registers(prototype.maxstacksize);
    std::vector<std::optional<Word>> known(prototype.maxstacksize);
    for (std::size_t index = 0; index < prototype.numparams; ++index) {
      registers[index] = builder.parameter(index);
    }

    const auto bytecode_error = [](int pc, const char *message) {
      return Status{StatusCode::kInvalidArgument, message,
                    static_cast<std::size_t>(pc)};
    };

    const auto translate_range =
        [&](int begin, int end, bool allow_return, int protected_state_base,
            std::vector<Value> *values,
            std::vector<std::optional<Word>> *constants,
            bool *returned) -> Status {
      for (int pc = begin; pc < end; ++pc) {
        const Instruction instruction = prototype.code[pc];
        const OpCode opcode = GET_OPCODE(instruction);
        const int destination = GETARG_A(instruction);
        const bool writes_destination =
            opcode == OP_MOVE || opcode == OP_LOADI || opcode == OP_LOADK ||
            opcode == OP_ADDI || opcode == OP_ADDK || opcode == OP_SUBK ||
            opcode == OP_MULK || opcode == OP_ADD || opcode == OP_SUB ||
            opcode == OP_MUL;
        if (writes_destination && protected_state_base >= 0 &&
            destination >= protected_state_base &&
            destination <= protected_state_base + 2) {
          return bytecode_error(
              pc, "numeric for body modifies an internal loop register");
        }

        switch (opcode) {
          case OP_MOVE: {
            const int source = GETARG_B(instruction);
            if (!valid_destination(*values, destination) ||
                !valid_register(*values, source)) {
              return bytecode_error(pc, "invalid register in OP_MOVE");
            }
            (*values)[static_cast<std::size_t>(destination)] =
                (*values)[static_cast<std::size_t>(source)];
            (*constants)[static_cast<std::size_t>(destination)] =
                (*constants)[static_cast<std::size_t>(source)];
            break;
          }

          case OP_LOADI: {
            if (!valid_destination(*values, destination)) {
              return bytecode_error(pc, "invalid destination in OP_LOADI");
            }
            const Word immediate =
                static_cast<Word>(GETARG_sBx(instruction));
            (*values)[static_cast<std::size_t>(destination)] =
                builder.constant(immediate);
            (*constants)[static_cast<std::size_t>(destination)] = immediate;
            break;
          }

          case OP_LOADK: {
            const int constant_index = GETARG_Bx(instruction);
            if (!valid_destination(*values, destination) ||
                constant_index < 0 || constant_index >= prototype.sizek) {
              return bytecode_error(pc, "invalid operand in OP_LOADK");
            }
            const TValue *constant = &prototype.k[constant_index];
            if (!ttisinteger(constant)) {
              return bytecode_error(
                  pc, "only integer constants are supported in OP_LOADK");
            }
            const Word immediate = static_cast<Word>(ivalue(constant));
            (*values)[static_cast<std::size_t>(destination)] =
                builder.constant(immediate);
            (*constants)[static_cast<std::size_t>(destination)] = immediate;
            break;
          }

          case OP_ADDI: {
            const int source = GETARG_B(instruction);
            if (!valid_destination(*values, destination) ||
                !valid_register(*values, source)) {
              return bytecode_error(pc, "invalid register in OP_ADDI");
            }
            (*values)[static_cast<std::size_t>(destination)] = builder.add(
                (*values)[static_cast<std::size_t>(source)],
                builder.constant(static_cast<Word>(GETARG_sC(instruction))));
            (*constants)[static_cast<std::size_t>(destination)].reset();
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
          case OP_MULK: {
            const int source = GETARG_B(instruction);
            const int constant_index = GETARG_C(instruction);
            if (!valid_destination(*values, destination) ||
                !valid_register(*values, source) || constant_index < 0 ||
                constant_index >= prototype.sizek) {
              return bytecode_error(pc,
                                    "invalid operand in constant arithmetic");
            }
            const TValue *constant = &prototype.k[constant_index];
            if (!ttisinteger(constant)) {
              return bytecode_error(
                  pc, "constant arithmetic requires an integer constant");
            }
            const Value lhs = (*values)[static_cast<std::size_t>(source)];
            const Value rhs =
                builder.constant(static_cast<Word>(ivalue(constant)));
            if (opcode == OP_ADDK) {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.add(lhs, rhs);
            } else if (opcode == OP_SUBK) {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.subtract(lhs, rhs);
            } else {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.multiply(lhs, rhs);
            }
            (*constants)[static_cast<std::size_t>(destination)].reset();
            if (pc + 1 >= end ||
                GET_OPCODE(prototype.code[pc + 1]) != OP_MMBINK) {
              return bytecode_error(
                  pc,
                  "constant arithmetic is missing its Lua metamethod fallback");
            }
            ++pc;
            break;
          }

          case OP_ADD:
          case OP_SUB:
          case OP_MUL: {
            const int lhs_index = GETARG_B(instruction);
            const int rhs_index = GETARG_C(instruction);
            if (!valid_destination(*values, destination) ||
                !valid_register(*values, lhs_index) ||
                !valid_register(*values, rhs_index)) {
              return bytecode_error(pc,
                                    "invalid register in binary arithmetic");
            }
            const Value lhs = (*values)[static_cast<std::size_t>(lhs_index)];
            const Value rhs = (*values)[static_cast<std::size_t>(rhs_index)];
            if (opcode == OP_ADD) {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.add(lhs, rhs);
            } else if (opcode == OP_SUB) {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.subtract(lhs, rhs);
            } else {
              (*values)[static_cast<std::size_t>(destination)] =
                  builder.multiply(lhs, rhs);
            }
            (*constants)[static_cast<std::size_t>(destination)].reset();
            if (pc + 1 >= end ||
                GET_OPCODE(prototype.code[pc + 1]) != OP_MMBIN) {
              return bytecode_error(
                  pc,
                  "binary arithmetic is missing its Lua metamethod fallback");
            }
            ++pc;
            break;
          }

          case OP_RETURN1:
            if (!allow_return || !valid_register(*values, destination)) {
              return bytecode_error(pc, "unsupported return in numeric loop");
            }
            if (!builder
                     .set_return(
                         (*values)[static_cast<std::size_t>(destination)])
                     .ok()) {
              return bytecode_error(pc,
                                    "unable to record the Lua return value");
            }
            *returned = true;
            return Status::ok_status();

          case OP_RETURN:
            if (!allow_return || GETARG_B(instruction) != 2 ||
                GETARG_k(instruction) != 0 ||
                !valid_register(*values, destination)) {
              return bytecode_error(
                  pc, "only one-value fixed Lua returns are supported");
            }
            if (!builder
                     .set_return(
                         (*values)[static_cast<std::size_t>(destination)])
                     .ok()) {
              return bytecode_error(pc,
                                    "unable to record the Lua return value");
            }
            *returned = true;
            return Status::ok_status();

          default: {
            char message[96] = {};
            std::snprintf(message, sizeof(message),
                          "unsupported Lua 5.5 opcode %d in numeric loop",
                          static_cast<int>(opcode));
            return bytecode_error(pc, message);
          }
        }
      }
      return Status::ok_status();
    };

    bool returned = false;
    Status status = translate_range(0, preparation_pc, false, -1, &registers,
                                    &known, &returned);
    if (!status.ok()) {
      return {status, nullptr};
    }

    if (!valid_register(registers, state_base) ||
        !valid_register(registers, state_base + 1) ||
        !valid_register(registers, state_base + 2) ||
        !known[static_cast<std::size_t>(state_base)].has_value() ||
        !known[static_cast<std::size_t>(state_base + 2)].has_value() ||
        known[static_cast<std::size_t>(state_base + 2)].value() != 1) {
      return translation_error(
          static_cast<std::size_t>(preparation_pc),
          "numeric for loop requires an integer constant start and step 1");
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
            opcode == OP_SUBK || opcode == OP_MULK) {
          note(GETARG_B(instruction));
        } else if (opcode == OP_ADD || opcode == OP_SUB || opcode == OP_MUL) {
          note(GETARG_B(instruction));
          note(GETARG_C(instruction));
        } else if (opcode == OP_RETURN || opcode == OP_RETURN1) {
          note(GETARG_A(instruction));
        }
      }
    };
    note_register_reads(preparation_pc + 1, loop_pc);
    note_register_reads(loop_pc + 1, prototype.sizecode);
    needed_registers[static_cast<std::size_t>(state_base + 1)] = true;
    needed_registers[static_cast<std::size_t>(state_base + 2)] = true;

    std::vector<std::size_t> carried_registers;
    for (std::size_t index = 0; index < registers.size(); ++index) {
      if (registers[index].valid() && needed_registers[index]) {
        carried_registers.push_back(index);
      }
    }
    constexpr Word kLoopUnrollFactor = 8;
    const unijit::ir::Block dispatch =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block unroll_check =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block unrolled_loop =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block scalar_loop =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block exit =
        builder.create_block(carried_registers.size());
    if (!dispatch.valid() || !unroll_check.valid() ||
        !unrolled_loop.valid() || !scalar_loop.valid() || !exit.valid()) {
      return {{StatusCode::kResourceExhausted,
               "unable to allocate Lua numeric-loop blocks"},
              nullptr};
    }

    std::vector<Value> initial_loop_arguments;
    std::vector<Value> skipped_loop_arguments;
    initial_loop_arguments.reserve(carried_registers.size());
    skipped_loop_arguments.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      skipped_loop_arguments.push_back(registers[lua_register]);
      initial_loop_arguments.push_back(
          lua_register == static_cast<std::size_t>(state_base + 2)
              ? registers[static_cast<std::size_t>(state_base)]
              : registers[lua_register]);
    }
    const Value enters_loop = builder.less_equal(
        registers[static_cast<std::size_t>(state_base)],
        registers[static_cast<std::size_t>(state_base + 1)]);
    status = builder.branch(enters_loop, dispatch, initial_loop_arguments,
                            exit, skipped_loop_arguments);
    if (!status.ok()) {
      return {status, nullptr};
    }

    const auto block_arguments = [&](unijit::ir::Block block) {
      std::vector<Value> arguments;
      arguments.reserve(carried_registers.size());
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        arguments.push_back(builder.block_parameter(block, index));
      }
      return arguments;
    };
    const auto block_registers = [&](unijit::ir::Block block) {
      std::vector<Value> values(prototype.maxstacksize);
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        values[carried_registers[index]] =
            builder.block_parameter(block, index);
      }
      return values;
    };

    status = builder.set_insertion_block(dispatch);
    if (!status.ok()) {
      return {status, nullptr};
    }
    if (!builder.safepoint(static_cast<std::size_t>(preparation_pc)).valid()) {
      return translation_error(static_cast<std::size_t>(preparation_pc),
                               "unable to insert a numeric-loop safepoint");
    }
    const std::vector<Value> dispatch_arguments = block_arguments(dispatch);
    const std::vector<Value> dispatch_registers = block_registers(dispatch);
    const Value dispatch_index =
        dispatch_registers[static_cast<std::size_t>(state_base + 2)];
    const Value maximum_unrolled_start = builder.constant(
        std::numeric_limits<Word>::max() - (kLoopUnrollFactor - 1));
    const Value unroll_cannot_overflow =
        builder.less_equal(dispatch_index, maximum_unrolled_start);
    status = builder.branch(unroll_cannot_overflow, unroll_check,
                            dispatch_arguments, scalar_loop,
                            dispatch_arguments);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(unroll_check);
    if (!status.ok()) {
      return {status, nullptr};
    }
    const std::vector<Value> check_arguments = block_arguments(unroll_check);
    const std::vector<Value> check_registers =
        block_registers(unroll_check);
    const Value check_index =
        check_registers[static_cast<std::size_t>(state_base + 2)];
    const Value check_limit =
        check_registers[static_cast<std::size_t>(state_base + 1)];
    const Value last_unrolled_index = builder.add(
        check_index, builder.constant(kLoopUnrollFactor - 1));
    const Value has_full_unrolled_group =
        builder.less_equal(last_unrolled_index, check_limit);
    status = builder.branch(has_full_unrolled_group, unrolled_loop,
                            check_arguments, scalar_loop, check_arguments);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(unrolled_loop);
    if (!status.ok()) {
      return {status, nullptr};
    }
    std::vector<Value> unrolled_registers = block_registers(unrolled_loop);
    std::vector<std::optional<Word>> unrolled_known(prototype.maxstacksize);
    for (Word iteration = 0; iteration < kLoopUnrollFactor; ++iteration) {
      status = translate_range(preparation_pc + 1, loop_pc, false, state_base,
                               &unrolled_registers, &unrolled_known,
                               &returned);
      if (!status.ok()) {
        return {status, nullptr};
      }
      if (iteration + 1 < kLoopUnrollFactor) {
        unrolled_registers[static_cast<std::size_t>(state_base + 2)] =
            builder.add(
                unrolled_registers[static_cast<std::size_t>(state_base + 2)],
                registers[static_cast<std::size_t>(state_base + 2)]);
      }
    }
    const Value unrolled_index =
        unrolled_registers[static_cast<std::size_t>(state_base + 2)];
    const Value unrolled_limit =
        unrolled_registers[static_cast<std::size_t>(state_base + 1)];
    if (!unrolled_index.valid() || !unrolled_limit.valid()) {
      return translation_error(static_cast<std::size_t>(loop_pc),
                               "numeric loop lost its induction state");
    }
    const Value continues_unrolled =
        builder.less_than(unrolled_index, unrolled_limit);
    const Value next_unrolled_index = builder.add(
        unrolled_index, registers[static_cast<std::size_t>(state_base + 2)]);
    std::vector<Value> unrolled_backedge;
    std::vector<Value> unrolled_completed;
    unrolled_backedge.reserve(carried_registers.size());
    unrolled_completed.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      const Value value = unrolled_registers[lua_register];
      if (!value.valid()) {
        return translation_error(
            static_cast<std::size_t>(loop_pc),
            "numeric loop has an undefined carried register");
      }
      unrolled_completed.push_back(value);
      unrolled_backedge.push_back(
          lua_register == static_cast<std::size_t>(state_base + 2)
              ? next_unrolled_index
              : value);
    }
    status = builder.branch(continues_unrolled, dispatch, unrolled_backedge,
                            exit, unrolled_completed);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(scalar_loop);
    if (!status.ok()) {
      return {status, nullptr};
    }
    std::vector<Value> scalar_registers = block_registers(scalar_loop);
    std::vector<std::optional<Word>> scalar_known(prototype.maxstacksize);
    status = translate_range(preparation_pc + 1, loop_pc, false, state_base,
                             &scalar_registers, &scalar_known, &returned);
    if (!status.ok()) {
      return {status, nullptr};
    }
    const Value scalar_index =
        scalar_registers[static_cast<std::size_t>(state_base + 2)];
    const Value scalar_limit =
        scalar_registers[static_cast<std::size_t>(state_base + 1)];
    if (!scalar_index.valid() || !scalar_limit.valid()) {
      return translation_error(static_cast<std::size_t>(loop_pc),
                               "numeric loop lost its scalar induction state");
    }
    const Value continues_scalar =
        builder.less_than(scalar_index, scalar_limit);
    const Value next_scalar_index = builder.add(
        scalar_index, registers[static_cast<std::size_t>(state_base + 2)]);
    std::vector<Value> scalar_backedge;
    std::vector<Value> scalar_completed;
    scalar_backedge.reserve(carried_registers.size());
    scalar_completed.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      const Value value = scalar_registers[lua_register];
      if (!value.valid()) {
        return translation_error(
            static_cast<std::size_t>(loop_pc),
            "numeric scalar tail has an undefined carried register");
      }
      scalar_completed.push_back(value);
      scalar_backedge.push_back(
          lua_register == static_cast<std::size_t>(state_base + 2)
              ? next_scalar_index
              : value);
    }
    status = builder.branch(continues_scalar, scalar_loop, scalar_backedge,
                            exit, scalar_completed);
    if (!status.ok()) {
      return {status, nullptr};
    }

    status = builder.set_insertion_block(exit);
    if (!status.ok()) {
      return {status, nullptr};
    }
    std::vector<Value> exit_registers(prototype.maxstacksize);
    std::vector<std::optional<Word>> exit_known(prototype.maxstacksize);
    for (std::size_t index = 0; index < carried_registers.size(); ++index) {
      exit_registers[carried_registers[index]] =
          builder.block_parameter(exit, index);
    }
    status = translate_range(loop_pc + 1, prototype.sizecode, true, -1,
                             &exit_registers, &exit_known, &returned);
    if (!status.ok()) {
      return {status, nullptr};
    }
    if (!returned) {
      return translation_error(static_cast<std::size_t>(prototype.sizecode),
                               "Lua numeric loop has no supported return");
    }
    return Compiler::compile(std::move(builder).build());
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua numeric-loop translation state"},
            nullptr};
  }
}

CompilationResult compile_prototype(const Proto &prototype) {
  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    if (GET_OPCODE(prototype.code[pc]) == OP_FORPREP) {
      return compile_numeric_for_prototype(prototype);
    }
  }
  return compile_straight_prototype(prototype);
}

int destroy_compiled_function(lua_State *state) {
  auto *owned = static_cast<OwnedFunction *>(lua_touserdata(state, 1));
  if (owned != nullptr) {
    delete owned->function;
    owned->function = nullptr;
  }
  return 0;
}

int invoke_compiled_function(lua_State *state) {
  CallInfo *call = state->ci;
  const CClosure *closure = clCvalue(s2v(call->func.p));
  auto *owned = reinterpret_cast<OwnedFunction *>(
      getudatamem(uvalue(&closure->upvalue[0])));
  if (owned == nullptr || owned->function == nullptr ||
      !owned->function->valid()) {
    return luaL_error(state, "invalid UniJIT compiled function");
  }

  const StkId argument_base = call->func.p + 1;
  const std::size_t supplied =
      static_cast<std::size_t>(state->top.p - argument_base);
  if (supplied < owned->parameter_count) {
    return luaL_error(state, "compiled function requires %d arguments",
                      static_cast<int>(owned->parameter_count));
  }

  std::array<Word, kMaximumLuaParameters> arguments;
  for (std::size_t index = 0; index < owned->parameter_count; ++index) {
    const TValue *argument = s2v(argument_base + index);
    if (owned->mode == NumericMode::kInteger && !ttisinteger(argument)) {
      return luaL_error(state, "argument %d must be a Lua integer",
                        static_cast<int>(index + 1));
    }
    if (owned->mode == NumericMode::kFloat64 && !ttisfloat(argument)) {
      return luaL_error(state, "argument %d must be a Lua Float64",
                        static_cast<int>(index + 1));
    }
    arguments[index] =
        owned->mode == NumericMode::kInteger
            ? static_cast<Word>(ivalue(argument))
            : unijit::ir::pack_float64(static_cast<double>(fltvalue(argument)));
  }

  Word value = 0;
  bool invoked = false;
  char invocation_error[256] = {};
  {
    if (owned->function->requires_context()) {
      const unijit::ir::EvaluationResult result =
          owned->function->invoke(arguments.data(), owned->parameter_count);
      if (result.ok()) {
        value = result.value;
        invoked = true;
      } else {
        std::snprintf(invocation_error, sizeof(invocation_error),
                      "UniJIT invocation failed at site %zu: %s",
                      result.status.location(),
                      result.status.message().c_str());
      }
    } else {
      value = owned->function->native_entry()(arguments.data(), nullptr);
      invoked = true;
    }
  }
  if (!invoked) {
    return luaL_error(state, "%s", invocation_error);
  }
  if (owned->mode == NumericMode::kInteger) {
    setivalue(s2v(state->top.p), static_cast<lua_Integer>(value));
  } else {
    setfltvalue(s2v(state->top.p),
                static_cast<lua_Number>(unijit::ir::unpack_float64(value)));
  }
  ++state->top.p;
  return 1;
}

int compile_lua_function_mode(lua_State *state, NumericMode mode,
                              const char *api_name) {
  if (lua_type(state, 1) != LUA_TFUNCTION || lua_iscfunction(state, 1) != 0) {
    return luaL_error(state, "%s expects a Lua function", api_name);
  }

  void *storage = lua_newuserdatauv(state, sizeof(OwnedFunction), 0);
  auto *owned = new (storage) OwnedFunction{};
  luaL_setmetatable(state, kCompiledFunctionMetatable);

  const auto *closure =
      reinterpret_cast<const LClosure *>(lua_topointer(state, 1));
  if (closure == nullptr || closure->p == nullptr) {
    return luaL_error(state, "unable to inspect the Lua function prototype");
  }
  char error[512] = {};
  bool compiled = false;
  {
    std::string cache_key;
    const bool cacheable = prototype_cache_key(*closure->p, mode, &cache_key);
    const std::uint64_t fingerprint =
        mode == NumericMode::kInteger ? kIntegerCacheFingerprint
                                      : kFloatCacheFingerprint;
    CodeHandle code = cacheable
                          ? compiled_function_cache(mode).find(cache_key,
                                                               fingerprint)
                          : CodeHandle{};
    if (!code.valid()) {
      CompilationResult result =
          mode == NumericMode::kInteger
              ? compile_prototype(*closure->p)
              : unijit::frontend::lua55::detail::compile_float64_prototype(
                    *closure->p);
      if (result.ok()) {
        unijit::jit::CodeCachePublication publication =
            cacheable
                ? compiled_function_cache(mode).publish(
                      cache_key, fingerprint, std::move(result.function))
                : uncached_publication_cache().publish(
                      "uncached-lua-prototype", fingerprint,
                      std::move(result.function));
        if (publication.ok()) {
          code = std::move(publication.handle);
        } else {
          std::snprintf(error, sizeof(error),
                        "unable to publish Lua native code: %s",
                        publication.status.message().c_str());
        }
      } else {
        std::snprintf(error, sizeof(error), "Lua bytecode at pc %zu: %s",
                      result.status.location(),
                      result.status.message().c_str());
      }
    }
    if (code.valid()) {
      owned->function = new (std::nothrow) CodeHandle(std::move(code));
      if (owned->function == nullptr) {
        std::snprintf(error, sizeof(error),
                      "unable to allocate a Lua native-code lease");
      } else {
        owned->parameter_count = closure->p->numparams;
        owned->mode = mode;
        compiled = true;
      }
    }
  }
  if (!compiled) {
    return luaL_error(state, "%s", error);
  }

  lua_pushcclosure(state, invoke_compiled_function, 1);
  return 1;
}

int compile_lua_function(lua_State *state) {
  return compile_lua_function_mode(state, NumericMode::kInteger,
                                   "unijit.compile");
}

int compile_float_lua_function(lua_State *state) {
  return compile_lua_function_mode(state, NumericMode::kFloat64,
                                   "unijit.compile_float");
}

} // namespace

extern "C" int luaopen_unijit(lua_State *state) {
  if (luaL_newmetatable(state, kCompiledFunctionMetatable) != 0) {
    lua_pushcfunction(state, destroy_compiled_function);
    lua_setfield(state, -2, "__gc");
  }
  lua_pop(state, 1);

  static const luaL_Reg functions[] = {
      {"compile", compile_lua_function},
      {"compile_float", compile_float_lua_function},
      {nullptr, nullptr},
  };
  luaL_newlib(state, functions);
  return 1;
}
