#include "float_translator.h"

#include <cstddef>
#include <cstdio>
#include <new>
#include <vector>

extern "C" {
#include "lobject.h"
#include "lopcodes.h"
}

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::frontend::lua55::detail {
namespace {

using ir::FunctionBuilder;
using ir::Value;
using ir::ValueType;
using ir::Word;
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

}  // namespace

CompilationResult compile_float64_prototype(
    const Proto& prototype, jit::OptimizationLevel optimization_level) {
  if (isvararg(&prototype)) {
    return translation_error(0, "vararg Lua functions are not supported");
  }
  if (prototype.numparams > prototype.maxstacksize) {
    return translation_error(0, "Lua prototype has an invalid stack layout");
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
          kinds[static_cast<std::size_t>(destination)] =
              kinds[static_cast<std::size_t>(source)];
          break;
        }

        case OP_LOADI:
        case OP_LOADF:
          if (!valid_destination(registers, destination)) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "invalid numeric load destination");
          }
          registers[static_cast<std::size_t>(destination)] =
              builder.float64_constant(
                  static_cast<double>(GETARG_sBx(instruction)));
          kinds[static_cast<std::size_t>(destination)] =
              opcode == OP_LOADF ? NumericKind::kFloat64
                                 : NumericKind::kInteger;
          break;

        case OP_LOADK: {
          const int constant_index = GETARG_Bx(instruction);
          if (!valid_destination(registers, destination) ||
              constant_index < 0 || constant_index >= prototype.sizek) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "invalid operand in OP_LOADK");
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
            return translation_error(static_cast<std::size_t>(pc),
                                     "Float64 compilation requires a number");
          }
          registers[static_cast<std::size_t>(destination)] =
              builder.float64_constant(value);
          kinds[static_cast<std::size_t>(destination)] = kind;
          break;
        }

        case OP_ADDI: {
          const int source = GETARG_B(instruction);
          if (!valid_destination(registers, destination) ||
              !valid_register(registers, source)) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "invalid register in OP_ADDI");
          }
          if (kinds[static_cast<std::size_t>(source)] !=
              NumericKind::kFloat64) {
            return translation_error(
                static_cast<std::size_t>(pc),
                "Float64 arithmetic requires at least one Float64 operand");
          }
          const Value rhs = builder.float64_constant(
              static_cast<double>(GETARG_sC(instruction)));
          registers[static_cast<std::size_t>(destination)] =
              append_float_binary(&builder, opcode,
                                  registers[static_cast<std::size_t>(source)],
                                  rhs);
          kinds[static_cast<std::size_t>(destination)] = NumericKind::kFloat64;
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
        case OP_MULK:
        case OP_DIVK: {
          const int source = GETARG_B(instruction);
          const int constant_index = GETARG_C(instruction);
          if (!valid_destination(registers, destination) ||
              !valid_register(registers, source) || constant_index < 0 ||
              constant_index >= prototype.sizek) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "invalid operand in constant arithmetic");
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
            return translation_error(
                static_cast<std::size_t>(pc),
                "Float64 constant arithmetic requires a number");
          }
          const NumericKind source_kind =
              kinds[static_cast<std::size_t>(source)];
          if (opcode != OP_DIVK &&
              !has_float_operand(source_kind, constant_kind)) {
            return translation_error(
                static_cast<std::size_t>(pc),
                "Float64 arithmetic requires at least one Float64 operand");
          }
          registers[static_cast<std::size_t>(destination)] =
              append_float_binary(&builder, opcode,
                                  registers[static_cast<std::size_t>(source)],
                                  builder.float64_constant(value));
          kinds[static_cast<std::size_t>(destination)] = NumericKind::kFloat64;
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
        case OP_MUL:
        case OP_DIV: {
          const int lhs_index = GETARG_B(instruction);
          const int rhs_index = GETARG_C(instruction);
          if (!valid_destination(registers, destination) ||
              !valid_register(registers, lhs_index) ||
              !valid_register(registers, rhs_index)) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "invalid register in binary arithmetic");
          }
          const NumericKind lhs_kind =
              kinds[static_cast<std::size_t>(lhs_index)];
          const NumericKind rhs_kind =
              kinds[static_cast<std::size_t>(rhs_index)];
          if (opcode != OP_DIV && !has_float_operand(lhs_kind, rhs_kind)) {
            return translation_error(
                static_cast<std::size_t>(pc),
                "Float64 arithmetic requires at least one Float64 operand");
          }
          registers[static_cast<std::size_t>(destination)] =
              append_float_binary(
                  &builder, opcode,
                  registers[static_cast<std::size_t>(lhs_index)],
                  registers[static_cast<std::size_t>(rhs_index)]);
          kinds[static_cast<std::size_t>(destination)] = NumericKind::kFloat64;
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
        case OP_RETURN:
          if ((opcode == OP_RETURN &&
               (GETARG_B(instruction) != 2 || GETARG_k(instruction) != 0)) ||
              !valid_register(registers, destination)) {
            return translation_error(
                static_cast<std::size_t>(pc),
                "only one-value fixed Float64 returns are supported");
          }
          if (kinds[static_cast<std::size_t>(destination)] !=
              NumericKind::kFloat64) {
            return translation_error(static_cast<std::size_t>(pc),
                                     "compiled result is not Float64");
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
          std::snprintf(message, sizeof(message),
                        "unsupported Lua 5.5 opcode %d",
                        static_cast<int>(opcode));
          return translation_error(static_cast<std::size_t>(pc), message);
        }
      }
    }

    if (!has_return) {
      return translation_error(static_cast<std::size_t>(prototype.sizecode),
                               "Lua function has no supported return");
    }
    return Compiler::compile(std::move(builder).build(),
                             jit::CompilationOptions{optimization_level});
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua Float64 translation state"},
            nullptr};
  }
}

}  // namespace unijit::frontend::lua55::detail
