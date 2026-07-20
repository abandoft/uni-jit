#include "unijit_lua.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lobject.h"
#include "lopcodes.h"
}

#include "unijit/ir/function.h"
#include "unijit/jit/compiler.h"
#include "unijit/status.h"

namespace {

using unijit::Status;
using unijit::StatusCode;
using unijit::ir::FunctionBuilder;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::CompilationResult;
using unijit::jit::CompiledFunction;
using unijit::jit::Compiler;

constexpr char kCompiledFunctionMetatable[] = "unijit.lua55.compiled";
constexpr std::size_t kMaximumLuaParameters = 255;

static_assert(sizeof(lua_Integer) == sizeof(Word),
              "Lua and UniJIT integers must have the same width");

struct OwnedFunction final {
  CompiledFunction *function{nullptr};
  std::size_t parameter_count{0};
};

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

CompilationResult compile_prototype(const Proto &prototype) {
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

int destroy_compiled_function(lua_State *state) {
  auto *owned = static_cast<OwnedFunction *>(lua_touserdata(state, 1));
  if (owned != nullptr) {
    delete owned->function;
    owned->function = nullptr;
  }
  return 0;
}

int invoke_compiled_function(lua_State *state) {
  auto *owned =
      static_cast<OwnedFunction *>(lua_touserdata(state, lua_upvalueindex(1)));
  if (owned == nullptr || owned->function == nullptr) {
    return luaL_error(state, "invalid UniJIT compiled function");
  }

  const int supplied = lua_gettop(state);
  if (supplied < 0 ||
      static_cast<std::size_t>(supplied) < owned->parameter_count) {
    return luaL_error(state, "compiled function requires %d arguments",
                      static_cast<int>(owned->parameter_count));
  }

  std::array<Word, kMaximumLuaParameters> arguments{};
  for (std::size_t index = 0; index < owned->parameter_count; ++index) {
    const int lua_index = static_cast<int>(index + 1);
    if (lua_isinteger(state, lua_index) == 0) {
      return luaL_error(state, "argument %d must be a Lua integer", lua_index);
    }
    arguments[index] = static_cast<Word>(lua_tointeger(state, lua_index));
  }

  const Word value = owned->function->native_entry()(arguments.data());
  lua_pushinteger(state, static_cast<lua_Integer>(value));
  return 1;
}

int compile_lua_function(lua_State *state) {
  if (lua_type(state, 1) != LUA_TFUNCTION || lua_iscfunction(state, 1) != 0) {
    return luaL_error(state, "unijit.compile expects a Lua function");
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
    CompilationResult result = compile_prototype(*closure->p);
    if (result.ok()) {
      owned->parameter_count = closure->p->numparams;
      owned->function = result.function.release();
      compiled = true;
    } else {
      std::snprintf(error, sizeof(error), "Lua bytecode at pc %zu: %s",
                    result.status.location(), result.status.message().c_str());
    }
  }
  if (!compiled) {
    return luaL_error(state, "%s", error);
  }

  lua_pushcclosure(state, invoke_compiled_function, 1);
  return 1;
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
      {nullptr, nullptr},
  };
  luaL_newlib(state, functions);
  return 1;
}
