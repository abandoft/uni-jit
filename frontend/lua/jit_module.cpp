#include "unijit_lua.h"

#include "float_translator.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
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
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"
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

struct PrototypeSnapshot final {
  Proto view() const noexcept {
    Proto prototype{};
    prototype.numparams = numparams;
    prototype.flag = flag;
    prototype.maxstacksize = maxstacksize;
    prototype.sizeupvalues = sizeupvalues;
    prototype.sizek = static_cast<int>(constants.size());
    prototype.sizecode = static_cast<int>(instructions.size());
    prototype.sizep = 0;
    prototype.k = const_cast<TValue *>(constants.data());
    prototype.code = const_cast<Instruction *>(instructions.data());
    return prototype;
  }

  lu_byte numparams{0};
  lu_byte flag{0};
  lu_byte maxstacksize{0};
  int sizeupvalues{0};
  std::vector<TValue> constants;
  std::vector<Instruction> instructions;
};

struct LoopHotnessPlan final {
  enum class LimitKind : unsigned char {
    kUnknown,
    kConstant,
    kParameter,
  };

  std::uint64_t estimate(const Word *arguments,
                         std::size_t argument_count) const noexcept {
    if (!is_loop) {
      return 0;
    }
    Word limit = 0;
    if (limit_kind == LimitKind::kConstant) {
      limit = limit_constant;
    } else if (limit_kind == LimitKind::kParameter &&
               limit_parameter < argument_count) {
      limit = arguments[limit_parameter];
    } else {
      return 1;
    }
    std::uint64_t distance = 0;
    std::uint64_t stride = 0;
    if (step > 0) {
      if (limit < start) {
        return 0;
      }
      distance = static_cast<std::uint64_t>(limit) -
                 static_cast<std::uint64_t>(start);
      stride = static_cast<std::uint64_t>(step);
    } else if (step < 0) {
      if (start < limit) {
        return 0;
      }
      distance = static_cast<std::uint64_t>(start) -
                 static_cast<std::uint64_t>(limit);
      stride = std::uint64_t{0} - static_cast<std::uint64_t>(step);
    } else {
      return 0;
    }
    const std::uint64_t completed_backedges = distance / stride;
    return completed_backedges == std::numeric_limits<std::uint64_t>::max()
               ? completed_backedges
               : completed_backedges + 1;
  }

  bool is_loop{false};
  LimitKind limit_kind{LimitKind::kUnknown};
  Word start{0};
  Word step{0};
  Word limit_constant{0};
  std::size_t limit_parameter{0};
};

constexpr std::uint64_t kIntegerBaselineFingerprint =
    0x554A4C5549424153ULL;
constexpr std::uint64_t kIntegerOptimizedFingerprint =
    0x554A4C5541494E54ULL;
constexpr std::uint64_t kFloatBaselineFingerprint = 0x554A4C5546424153ULL;
constexpr std::uint64_t kFloatOptimizedFingerprint = 0x554A4C5541464C54ULL;
constexpr unijit::jit::TieringThresholds kLuaTieringThresholds{64, 10000,
                                                               64};
constexpr std::size_t kLuaSchedulerBytes = 16U * 1024U * 1024U;

struct CompiledFunctionState final {
  CompiledFunctionState(
      std::uint64_t id, std::size_t parameters, NumericMode numeric_mode,
      std::shared_ptr<const PrototypeSnapshot> retained_prototype,
      std::string retained_cache_key, LoopHotnessPlan retained_loop_hotness,
      bool supports_tiering)
      : parameter_count(parameters),
        mode(numeric_mode),
        prototype(std::move(retained_prototype)),
        cache_key(std::move(retained_cache_key)),
        loop_hotness(retained_loop_hotness),
        tierable(supports_tiering),
        scheduling_identity("lua55:" + std::to_string(id)),
        code(kLuaTieringThresholds) {}

  void retain_ticket(unijit::jit::CompilationTicket retained) {
    std::lock_guard<std::mutex> lock(ticket_mutex);
    ticket = std::move(retained);
  }

  unijit::jit::CompilationTicket current_ticket() const {
    std::lock_guard<std::mutex> lock(ticket_mutex);
    return ticket;
  }

  std::size_t parameter_count;
  NumericMode mode;
  std::shared_ptr<const PrototypeSnapshot> prototype;
  std::string cache_key;
  LoopHotnessPlan loop_hotness;
  bool tierable;
  std::string scheduling_identity;
  unijit::jit::TieredCode code;
  mutable std::mutex ticket_mutex;
  unijit::jit::CompilationTicket ticket;
};

struct OwnedFunction final {
  std::shared_ptr<CompiledFunctionState> *state{nullptr};
};

struct LuaService final {
  LuaService() : uncached_cache(unijit::jit::CodeCacheLimits{0, 0}) {
    unijit::jit::CompilationSchedulerOptions options;
    options.worker_count = 1;
    options.maximum_queued_tasks = 64;
    options.maximum_queued_bytes = kLuaSchedulerBytes;
    unijit::jit::CompilationSchedulerCreation creation =
        unijit::jit::CompilationScheduler::create(options);
    scheduler_status = std::move(creation.status);
    scheduler = std::move(creation.scheduler);
  }

  CodeCache &cache(NumericMode mode,
                   unijit::jit::OptimizationLevel level) noexcept {
    if (mode == NumericMode::kInteger) {
      return level == unijit::jit::OptimizationLevel::kBaseline
                 ? integer_baseline_cache
                 : integer_optimized_cache;
    }
    return level == unijit::jit::OptimizationLevel::kBaseline
               ? float_baseline_cache
               : float_optimized_cache;
  }

  std::uint64_t allocate_identity() noexcept {
    std::uint64_t current =
        next_identity.fetch_add(1, std::memory_order_relaxed);
    if (current == 0) {
      current = next_identity.fetch_add(1, std::memory_order_relaxed);
    }
    return current;
  }

  CodeCache integer_baseline_cache;
  CodeCache integer_optimized_cache;
  CodeCache float_baseline_cache;
  CodeCache float_optimized_cache;
  CodeCache uncached_cache;
  unijit::Status scheduler_status;
  std::unique_ptr<unijit::jit::CompilationScheduler> scheduler;
  std::atomic<std::uint64_t> next_identity{1};
};

LuaService &lua_service() {
  static LuaService service;
  return service;
}

std::uint64_t cache_fingerprint(
    NumericMode mode, unijit::jit::OptimizationLevel level) noexcept {
  if (mode == NumericMode::kInteger) {
    return level == unijit::jit::OptimizationLevel::kBaseline
               ? kIntegerBaselineFingerprint
               : kIntegerOptimizedFingerprint;
  }
  return level == unijit::jit::OptimizationLevel::kBaseline
             ? kFloatBaselineFingerprint
             : kFloatOptimizedFingerprint;
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

std::shared_ptr<const PrototypeSnapshot> capture_prototype(
    const Proto &prototype) {
  if (prototype.sizecode < 0 || prototype.sizek < 0 ||
      prototype.sizeupvalues < 0 || prototype.sizep != 0 ||
      (prototype.sizecode != 0 && prototype.code == nullptr) ||
      (prototype.sizek != 0 && prototype.k == nullptr)) {
    return {};
  }
  try {
    auto snapshot = std::make_shared<PrototypeSnapshot>();
    snapshot->numparams = prototype.numparams;
    snapshot->flag = prototype.flag;
    snapshot->maxstacksize = prototype.maxstacksize;
    snapshot->sizeupvalues = prototype.sizeupvalues;
    if (prototype.sizecode != 0) {
      snapshot->instructions.assign(
          prototype.code, prototype.code + prototype.sizecode);
    }
    snapshot->constants.reserve(static_cast<std::size_t>(prototype.sizek));
    for (int index = 0; index < prototype.sizek; ++index) {
      const TValue &constant = prototype.k[index];
      if (!ttisinteger(&constant) && !ttisfloat(&constant)) {
        return {};
      }
      snapshot->constants.push_back(constant);
    }
    return snapshot;
  } catch (const std::bad_alloc &) {
    return {};
  }
}

LoopHotnessPlan analyze_loop_hotness(const Proto &prototype) {
  struct SymbolicInteger final {
    std::optional<Word> constant;
    std::optional<std::size_t> parameter;
  };

  LoopHotnessPlan plan;
  if (prototype.maxstacksize == 0 || prototype.code == nullptr) {
    return plan;
  }
  std::vector<SymbolicInteger> registers(prototype.maxstacksize);
  for (std::size_t index = 0; index < prototype.numparams; ++index) {
    registers[index].parameter = index;
  }

  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    const Instruction instruction = prototype.code[pc];
    const OpCode opcode = GET_OPCODE(instruction);
    const int destination = GETARG_A(instruction);
    if (opcode == OP_FORPREP) {
      plan.is_loop = true;
      const int state_base = destination;
      if (state_base < 0 || state_base + 2 >= prototype.maxstacksize) {
        return plan;
      }
      const SymbolicInteger &start =
          registers[static_cast<std::size_t>(state_base)];
      const SymbolicInteger &limit =
          registers[static_cast<std::size_t>(state_base + 1)];
      const SymbolicInteger &step =
          registers[static_cast<std::size_t>(state_base + 2)];
      if (!start.constant.has_value() || !step.constant.has_value() ||
          step.constant.value() == 0) {
        return plan;
      }
      plan.start = start.constant.value();
      plan.step = step.constant.value();
      if (limit.constant.has_value()) {
        plan.limit_kind = LoopHotnessPlan::LimitKind::kConstant;
        plan.limit_constant = limit.constant.value();
      } else if (limit.parameter.has_value()) {
        plan.limit_kind = LoopHotnessPlan::LimitKind::kParameter;
        plan.limit_parameter = limit.parameter.value();
      }
      return plan;
    }

    if (destination < 0 || destination >= prototype.maxstacksize) {
      continue;
    }
    SymbolicInteger &result =
        registers[static_cast<std::size_t>(destination)];
    switch (opcode) {
      case OP_MOVE: {
        const int source = GETARG_B(instruction);
        result = source >= 0 && source < prototype.maxstacksize
                     ? registers[static_cast<std::size_t>(source)]
                     : SymbolicInteger{};
        break;
      }
      case OP_LOADI:
        result.constant = static_cast<Word>(GETARG_sBx(instruction));
        result.parameter.reset();
        break;
      case OP_LOADK: {
        const int constant_index = GETARG_Bx(instruction);
        if (constant_index >= 0 && constant_index < prototype.sizek &&
            ttisinteger(&prototype.k[constant_index])) {
          result.constant =
              static_cast<Word>(ivalue(&prototype.k[constant_index]));
          result.parameter.reset();
        } else {
          result = {};
        }
        break;
      }
      case OP_MMBIN:
      case OP_MMBINI:
      case OP_MMBINK:
        break;
      default:
        result = {};
        break;
    }
  }
  return plan;
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

CompilationResult compile_straight_prototype(
    const Proto &prototype,
    unijit::jit::OptimizationLevel optimization_level) {
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
    return Compiler::compile(
        std::move(builder).build(),
        unijit::jit::CompilationOptions{optimization_level});
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate Lua frontend translation state"},
            nullptr};
  }
}

CompilationResult compile_numeric_for_prototype(
    const Proto &prototype,
    unijit::jit::OptimizationLevel optimization_level) {
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
        !known[static_cast<std::size_t>(state_base + 2)].has_value()) {
      return translation_error(
          static_cast<std::size_t>(preparation_pc),
          "numeric for loop requires integer constant start and step");
    }
    const Word loop_step =
        known[static_cast<std::size_t>(state_base + 2)].value();
    if (loop_step == 0) {
      return translation_error(static_cast<std::size_t>(preparation_pc),
                               "numeric for loop step cannot be zero");
    }
    const bool ascending = loop_step > 0;

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
    Word loop_unroll_factor = 1;
    Word unrolled_index_delta = 0;
    constexpr Word kOptimizedUnrollFactor = 8;
    constexpr Word kUnrolledStepCount = kOptimizedUnrollFactor - 1;
    if (optimization_level != unijit::jit::OptimizationLevel::kBaseline &&
        ((ascending &&
          loop_step <=
              std::numeric_limits<Word>::max() / kUnrolledStepCount) ||
         (!ascending &&
          loop_step >=
              std::numeric_limits<Word>::min() / kUnrolledStepCount))) {
      loop_unroll_factor = kOptimizedUnrollFactor;
      unrolled_index_delta = loop_step * kUnrolledStepCount;
    }
    const bool has_unit_step = loop_step == 1 || loop_step == -1;
    const unijit::ir::Block dispatch =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block unroll_check =
        builder.create_block(carried_registers.size() + 1);
    const unijit::ir::Block unrolled_loop =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block unrolled_continue_check =
        has_unit_step ? unijit::ir::Block{}
                      : builder.create_block(carried_registers.size() + 1);
    const unijit::ir::Block scalar_loop =
        builder.create_block(carried_registers.size());
    const unijit::ir::Block scalar_continue_check =
        has_unit_step ? unijit::ir::Block{}
                      : builder.create_block(carried_registers.size() + 1);
    const unijit::ir::Block exit =
        builder.create_block(carried_registers.size());
    if (!dispatch.valid() || !unroll_check.valid() ||
        !unrolled_loop.valid() ||
        (!has_unit_step && !unrolled_continue_check.valid()) ||
        !scalar_loop.valid() ||
        (!has_unit_step && !scalar_continue_check.valid()) || !exit.valid()) {
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
    const Value start = registers[static_cast<std::size_t>(state_base)];
    const Value limit = registers[static_cast<std::size_t>(state_base + 1)];
    const Value enters_loop =
        ascending ? builder.less_equal(start, limit)
                  : builder.less_equal(limit, start);
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
    const Value last_unrolled_index = builder.add(
        dispatch_index, builder.constant(unrolled_index_delta));
    const Value unrolled_group_advances =
        loop_unroll_factor == 1
            ? builder.constant(1)
            : (ascending
                   ? builder.less_than(dispatch_index, last_unrolled_index)
                   : builder.less_than(last_unrolled_index, dispatch_index));
    std::vector<Value> unroll_check_arguments = dispatch_arguments;
    unroll_check_arguments.push_back(last_unrolled_index);
    status = builder.branch(unrolled_group_advances, unroll_check,
                            unroll_check_arguments, scalar_loop,
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
    const Value check_limit =
        check_registers[static_cast<std::size_t>(state_base + 1)];
    const Value checked_last_unrolled_index =
        builder.block_parameter(unroll_check, carried_registers.size());
    const Value has_full_unrolled_group =
        ascending ? builder.less_equal(checked_last_unrolled_index, check_limit)
                  : builder.less_equal(check_limit,
                                       checked_last_unrolled_index);
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
    for (Word iteration = 0; iteration < loop_unroll_factor; ++iteration) {
      status = translate_range(preparation_pc + 1, loop_pc, false, state_base,
                               &unrolled_registers, &unrolled_known,
                               &returned);
      if (!status.ok()) {
        return {status, nullptr};
      }
      if (iteration + 1 < loop_unroll_factor) {
        unrolled_registers[static_cast<std::size_t>(state_base + 2)] =
            builder.add(
                unrolled_registers[static_cast<std::size_t>(state_base + 2)],
                registers[static_cast<std::size_t>(state_base + 2)]);
      }
    }
    const Value unrolled_index =
        unrolled_registers[static_cast<std::size_t>(state_base + 2)];
    if (!unrolled_index.valid() ||
        !unrolled_registers[static_cast<std::size_t>(state_base + 1)].valid()) {
      return translation_error(static_cast<std::size_t>(loop_pc),
                               "numeric loop lost its induction state");
    }
    const Value next_unrolled_index = builder.add(
        unrolled_index, registers[static_cast<std::size_t>(state_base + 2)]);
    std::vector<Value> unrolled_completed;
    unrolled_completed.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      const Value value = unrolled_registers[lua_register];
      if (!value.valid()) {
        return translation_error(
            static_cast<std::size_t>(loop_pc),
            "numeric loop has an undefined carried register");
      }
      unrolled_completed.push_back(value);
    }
    if (has_unit_step) {
      const Value unrolled_limit =
          unrolled_registers[static_cast<std::size_t>(state_base + 1)];
      const Value continues_unrolled =
          ascending ? builder.less_than(unrolled_index, unrolled_limit)
                    : builder.less_than(unrolled_limit, unrolled_index);
      std::vector<Value> unrolled_backedge = unrolled_completed;
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        if (carried_registers[index] ==
            static_cast<std::size_t>(state_base + 2)) {
          unrolled_backedge[index] = next_unrolled_index;
        }
      }
      status = builder.branch(continues_unrolled, dispatch,
                              unrolled_backedge, exit, unrolled_completed);
      if (!status.ok()) {
        return {status, nullptr};
      }
    } else {
      const Value unrolled_advance_is_safe =
          ascending ? builder.less_than(unrolled_index, next_unrolled_index)
                    : builder.less_than(next_unrolled_index, unrolled_index);
      std::vector<Value> unrolled_continue_arguments = unrolled_completed;
      unrolled_continue_arguments.push_back(next_unrolled_index);
      status = builder.branch(unrolled_advance_is_safe,
                              unrolled_continue_check,
                              unrolled_continue_arguments, exit,
                              unrolled_completed);
      if (!status.ok()) {
        return {status, nullptr};
      }

      status = builder.set_insertion_block(unrolled_continue_check);
      if (!status.ok()) {
        return {status, nullptr};
      }
      const std::vector<Value> unrolled_current_arguments =
          block_arguments(unrolled_continue_check);
      const std::vector<Value> unrolled_current_registers =
          block_registers(unrolled_continue_check);
      const Value checked_next_unrolled_index = builder.block_parameter(
          unrolled_continue_check, carried_registers.size());
      const Value checked_unrolled_limit = unrolled_current_registers[
          static_cast<std::size_t>(state_base + 1)];
      const Value continues_unrolled =
          ascending
              ? builder.less_equal(checked_next_unrolled_index,
                                   checked_unrolled_limit)
              : builder.less_equal(checked_unrolled_limit,
                                   checked_next_unrolled_index);
      std::vector<Value> unrolled_backedge = unrolled_current_arguments;
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        if (carried_registers[index] ==
            static_cast<std::size_t>(state_base + 2)) {
          unrolled_backedge[index] = checked_next_unrolled_index;
        }
      }
      status = builder.branch(continues_unrolled, dispatch,
                              unrolled_backedge, exit,
                              unrolled_current_arguments);
      if (!status.ok()) {
        return {status, nullptr};
      }
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
    if (!scalar_index.valid() ||
        !scalar_registers[static_cast<std::size_t>(state_base + 1)].valid()) {
      return translation_error(static_cast<std::size_t>(loop_pc),
                               "numeric loop lost its scalar induction state");
    }
    const Value next_scalar_index = builder.add(
        scalar_index, registers[static_cast<std::size_t>(state_base + 2)]);
    std::vector<Value> scalar_completed;
    scalar_completed.reserve(carried_registers.size());
    for (const std::size_t lua_register : carried_registers) {
      const Value value = scalar_registers[lua_register];
      if (!value.valid()) {
        return translation_error(
            static_cast<std::size_t>(loop_pc),
            "numeric scalar tail has an undefined carried register");
      }
      scalar_completed.push_back(value);
    }
    if (has_unit_step) {
      const Value scalar_limit =
          scalar_registers[static_cast<std::size_t>(state_base + 1)];
      const Value continues_scalar =
          ascending ? builder.less_than(scalar_index, scalar_limit)
                    : builder.less_than(scalar_limit, scalar_index);
      std::vector<Value> scalar_backedge = scalar_completed;
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        if (carried_registers[index] ==
            static_cast<std::size_t>(state_base + 2)) {
          scalar_backedge[index] = next_scalar_index;
        }
      }
      status = builder.branch(continues_scalar, scalar_loop, scalar_backedge,
                              exit, scalar_completed);
    } else {
      const Value scalar_advance_is_safe =
          ascending ? builder.less_than(scalar_index, next_scalar_index)
                    : builder.less_than(next_scalar_index, scalar_index);
      std::vector<Value> scalar_continue_arguments = scalar_completed;
      scalar_continue_arguments.push_back(next_scalar_index);
      status = builder.branch(scalar_advance_is_safe, scalar_continue_check,
                              scalar_continue_arguments, exit,
                              scalar_completed);
      if (!status.ok()) {
        return {status, nullptr};
      }

      status = builder.set_insertion_block(scalar_continue_check);
      if (!status.ok()) {
        return {status, nullptr};
      }
      const std::vector<Value> scalar_current_arguments =
          block_arguments(scalar_continue_check);
      const std::vector<Value> scalar_current_registers =
          block_registers(scalar_continue_check);
      const Value checked_next_scalar_index = builder.block_parameter(
          scalar_continue_check, carried_registers.size());
      const Value checked_scalar_limit = scalar_current_registers[
          static_cast<std::size_t>(state_base + 1)];
      const Value continues_scalar =
          ascending
              ? builder.less_equal(checked_next_scalar_index,
                                   checked_scalar_limit)
              : builder.less_equal(checked_scalar_limit,
                                   checked_next_scalar_index);
      std::vector<Value> scalar_backedge = scalar_current_arguments;
      for (std::size_t index = 0; index < carried_registers.size(); ++index) {
        if (carried_registers[index] ==
            static_cast<std::size_t>(state_base + 2)) {
          scalar_backedge[index] = checked_next_scalar_index;
        }
      }
      status = builder.branch(continues_scalar, scalar_loop, scalar_backedge,
                              exit, scalar_current_arguments);
    }
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

CompilationResult compile_prototype(
    const Proto &prototype,
    unijit::jit::OptimizationLevel optimization_level) {
  for (int pc = 0; pc < prototype.sizecode; ++pc) {
    if (GET_OPCODE(prototype.code[pc]) == OP_FORPREP) {
      return compile_numeric_for_prototype(prototype, optimization_level);
    }
  }
  return compile_straight_prototype(prototype, optimization_level);
}

CompilationResult compile_numeric_mode(
    const Proto &prototype, NumericMode mode,
    unijit::jit::OptimizationLevel optimization_level) {
  return mode == NumericMode::kInteger
             ? compile_prototype(prototype, optimization_level)
             : unijit::frontend::lua55::detail::compile_float64_prototype(
                   prototype, optimization_level);
}

Status cancelled_compilation_status() {
  return {StatusCode::kCancelled, "Lua optimization was cancelled"};
}

Status fail_optimization(
    const std::shared_ptr<CompiledFunctionState> &compiled,
    Status status) {
  (void)compiled->code.report_optimization_failure();
  return status;
}

Status compile_optimized(
    LuaService *service,
    const std::shared_ptr<CompiledFunctionState> &compiled,
    std::uint64_t generation,
    const unijit::jit::CompilationCancellation &cancellation) {
  if (cancellation.stop_requested()) {
    return fail_optimization(compiled, cancelled_compilation_status());
  }
  if (compiled->prototype == nullptr || compiled->cache_key.empty()) {
    return fail_optimization(
        compiled,
        {StatusCode::kInvalidArgument,
         "Lua optimized compilation has no retained prototype"});
  }

  constexpr auto kOptimized = unijit::jit::OptimizationLevel::kOptimized;
  const std::uint64_t fingerprint =
      cache_fingerprint(compiled->mode, kOptimized);
  CodeHandle optimized = service->cache(compiled->mode, kOptimized)
                             .find(compiled->cache_key, fingerprint);
  if (!optimized.valid()) {
    const Proto prototype = compiled->prototype->view();
    CompilationResult result =
        compile_numeric_mode(prototype, compiled->mode, kOptimized);
    if (!result.ok()) {
      return fail_optimization(compiled, std::move(result.status));
    }
    if (result.function->parameter_count() != compiled->parameter_count) {
      return fail_optimization(
          compiled,
          {StatusCode::kCodeGenerationFailed,
           "Lua optimized signature differs from its baseline"});
    }
    if (cancellation.stop_requested()) {
      return fail_optimization(compiled, cancelled_compilation_status());
    }
    unijit::jit::CodeCachePublication publication =
        service->cache(compiled->mode, kOptimized)
            .publish(compiled->cache_key, fingerprint,
                     std::move(result.function));
    if (!publication.ok()) {
      return fail_optimization(compiled, std::move(publication.status));
    }
    optimized = std::move(publication.handle);
  }

  if (optimized.parameter_count() != compiled->parameter_count) {
    return fail_optimization(
        compiled,
        {StatusCode::kCodeGenerationFailed,
         "cached Lua optimized signature differs from baseline"});
  }
  if (cancellation.stop_requested()) {
    return fail_optimization(compiled, cancelled_compilation_status());
  }
  const Status promotion =
      compiled->code.publish_optimized(std::move(optimized), generation);
  if (!promotion.ok()) {
    return fail_optimization(compiled, promotion);
  }
  return Status::ok_status();
}

void schedule_if_hot(
    const std::shared_ptr<CompiledFunctionState> &compiled) noexcept {
  if (!compiled->tierable || !compiled->code.try_begin_optimization()) {
    return;
  }
  const unijit::jit::TieredCodeSnapshot baseline = compiled->code.snapshot();
  if (baseline.tier != unijit::jit::CodeTier::kBaseline) {
    (void)compiled->code.report_optimization_failure();
    return;
  }

  try {
    LuaService &service = lua_service();
    if (service.scheduler == nullptr) {
      (void)compiled->code.report_optimization_failure();
      return;
    }
    std::size_t estimated_bytes = compiled->cache_key.size();
    const auto add_estimate = [&estimated_bytes](std::size_t bytes) {
      estimated_bytes =
          bytes > std::numeric_limits<std::size_t>::max() - estimated_bytes
              ? std::numeric_limits<std::size_t>::max()
              : estimated_bytes + bytes;
    };
    add_estimate(compiled->prototype->instructions.size() *
                 sizeof(Instruction));
    add_estimate(compiled->prototype->constants.size() * sizeof(TValue));
    add_estimate(4096U);

    unijit::jit::CompilationRequest request;
    request.identity = compiled->scheduling_identity;
    request.generation = baseline.generation;
    request.estimated_bytes = estimated_bytes;
    request.priority = unijit::jit::CompilationPriority::kNormal;
    request.job = [&service, compiled, generation = baseline.generation](
                      const unijit::jit::CompilationCancellation &
                          cancellation) {
      return compile_optimized(&service, compiled, generation, cancellation);
    };
    unijit::jit::CompilationSubmission submission =
        service.scheduler->try_submit(std::move(request));
    if (!submission.ok()) {
      (void)compiled->code.report_optimization_failure();
      return;
    }
    compiled->retain_ticket(std::move(submission.ticket));
  } catch (...) {
    (void)compiled->code.report_optimization_failure();
  }
}

int destroy_compiled_function(lua_State *state) {
  auto *owned = static_cast<OwnedFunction *>(lua_touserdata(state, 1));
  if (owned != nullptr && owned->state != nullptr) {
    (void)(*owned->state)->current_ticket().cancel();
    delete owned->state;
    owned->state = nullptr;
  }
  return 0;
}

int invoke_compiled_function(lua_State *state) {
  CallInfo *call = state->ci;
  const CClosure *closure = clCvalue(s2v(call->func.p));
  auto *owned = reinterpret_cast<OwnedFunction *>(
      getudatamem(uvalue(&closure->upvalue[0])));
  if (owned == nullptr || owned->state == nullptr ||
      !(*owned->state)->code.snapshot().valid()) {
    return luaL_error(state, "invalid UniJIT compiled function");
  }
  CompiledFunctionState *compiled = owned->state->get();

  const StkId argument_base = call->func.p + 1;
  const std::size_t supplied =
      static_cast<std::size_t>(state->top.p - argument_base);
  if (supplied < compiled->parameter_count) {
    return luaL_error(state, "compiled function requires %d arguments",
                      static_cast<int>(compiled->parameter_count));
  }

  std::array<Word, kMaximumLuaParameters> arguments;
  for (std::size_t index = 0; index < compiled->parameter_count; ++index) {
    const TValue *argument = s2v(argument_base + index);
    if (compiled->mode == NumericMode::kInteger && !ttisinteger(argument)) {
      return luaL_error(state, "argument %d must be a Lua integer",
                        static_cast<int>(index + 1));
    }
    if (compiled->mode == NumericMode::kFloat64 && !ttisfloat(argument)) {
      return luaL_error(state, "argument %d must be a Lua Float64",
                        static_cast<int>(index + 1));
    }
    arguments[index] =
        compiled->mode == NumericMode::kInteger
            ? static_cast<Word>(ivalue(argument))
            : unijit::ir::pack_float64(static_cast<double>(fltvalue(argument)));
  }

  Word value = 0;
  bool invoked = false;
  char invocation_error[256] = {};
  {
    unijit::runtime::ExecutionContext execution_context;
    const unijit::jit::TieredInvocationResult result = compiled->code.invoke(
        arguments.data(), compiled->parameter_count, &execution_context);
    if (result.ok()) {
      value = result.result.value;
      invoked = true;
    } else {
      std::snprintf(invocation_error, sizeof(invocation_error),
                    "UniJIT invocation failed at site %zu: %s",
                    result.result.status.location(),
                    result.result.status.message().c_str());
    }
  }
  if (!invoked) {
    return luaL_error(state, "%s", invocation_error);
  }
  compiled->code.record_backedges(
      compiled->loop_hotness.estimate(arguments.data(),
                                      compiled->parameter_count));
  schedule_if_hot(*owned->state);
  if (compiled->mode == NumericMode::kInteger) {
    setivalue(s2v(state->top.p), static_cast<lua_Integer>(value));
  } else {
    setfltvalue(s2v(state->top.p),
                static_cast<lua_Number>(unijit::ir::unpack_float64(value)));
  }
  ++state->top.p;
  return 1;
}

CompiledFunctionState *compiled_state_argument(lua_State *state, int index) {
  if (lua_type(state, index) != LUA_TFUNCTION ||
      lua_iscfunction(state, index) == 0 ||
      lua_getupvalue(state, index, 1) == nullptr) {
    return nullptr;
  }
  auto *owned = static_cast<OwnedFunction *>(
      luaL_testudata(state, -1, kCompiledFunctionMetatable));
  CompiledFunctionState *compiled =
      owned == nullptr || owned->state == nullptr ? nullptr
                                                  : owned->state->get();
  lua_pop(state, 1);
  return compiled;
}

const char *tier_name(unijit::jit::CodeTier tier) noexcept {
  switch (tier) {
    case unijit::jit::CodeTier::kBaseline:
      return "baseline";
    case unijit::jit::CodeTier::kOptimized:
      return "optimized";
    default:
      return "none";
  }
}

const char *task_state_name(
    unijit::jit::CompilationTaskState task_state) noexcept {
  switch (task_state) {
    case unijit::jit::CompilationTaskState::kQueued:
      return "queued";
    case unijit::jit::CompilationTaskState::kRunning:
      return "running";
    case unijit::jit::CompilationTaskState::kSucceeded:
      return "succeeded";
    case unijit::jit::CompilationTaskState::kFailed:
      return "failed";
    case unijit::jit::CompilationTaskState::kCancelled:
      return "cancelled";
    default:
      return "idle";
  }
}

lua_Integer metric_value(std::uint64_t value) noexcept {
  constexpr auto kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max());
  return static_cast<lua_Integer>(value > kMaximum ? kMaximum : value);
}

void set_metric(lua_State *state, const char *name, std::uint64_t value) {
  lua_pushinteger(state, metric_value(value));
  lua_setfield(state, -2, name);
}

void set_flag(lua_State *state, const char *name, bool value) {
  lua_pushboolean(state, value ? 1 : 0);
  lua_setfield(state, -2, name);
}

void set_text(lua_State *state, const char *name, const char *value) {
  lua_pushstring(state, value);
  lua_setfield(state, -2, name);
}

int compiled_function_stats(lua_State *state) {
  if (lua_gettop(state) != 1) {
    return luaL_error(state, "unijit.stats expects one compiled function");
  }
  CompiledFunctionState *compiled = compiled_state_argument(state, 1);
  if (compiled == nullptr) {
    return luaL_error(state,
                      "unijit.stats expects a function from unijit.compile");
  }
  unijit::jit::TieredCodeStats stats;
  unijit::jit::CompilationTaskState task_state =
      unijit::jit::CompilationTaskState::kInvalid;
  bool cancellation_requested = false;
  unijit::jit::CompilationSchedulerStats scheduler_stats;
  std::size_t code_size = 0;
  std::size_t input_ir_nodes = 0;
  std::size_t active_ir_nodes = 0;
  {
    stats = compiled->code.stats();
    const unijit::jit::TieredCodeSnapshot snapshot =
        compiled->code.snapshot();
    const unijit::jit::CompilationStats *compilation =
        snapshot.handle.compilation_stats();
    if (compilation != nullptr) {
      code_size = compilation->code_size;
      input_ir_nodes = compilation->input_ir_nodes;
      active_ir_nodes = compilation->optimized_ir_nodes;
    }
    const unijit::jit::CompilationTicket ticket =
        compiled->current_ticket();
    task_state = ticket.state();
    cancellation_requested = ticket.cancellation_requested();
    LuaService &service = lua_service();
    if (service.scheduler != nullptr) {
      scheduler_stats = service.scheduler->stats();
    }
  }
  LuaService &service = lua_service();

  lua_createtable(state, 0, 22);
  set_text(state, "active_tier", tier_name(stats.active_tier));
  set_flag(state, "tierable", compiled->tierable);
  set_flag(state, "loop", compiled->loop_hotness.is_loop);
  set_metric(state, "generation", stats.generation);
  set_metric(state, "invocations", stats.hotness.invocations);
  set_metric(state, "backedges", stats.hotness.backedges);
  set_metric(state, "compilation_attempts",
             stats.hotness.compilation_attempts);
  set_metric(state, "successful_compilations",
             stats.hotness.successful_compilations);
  set_metric(state, "failed_compilations",
             stats.hotness.failed_compilations);
  set_metric(state, "promotions", stats.promotions);
  set_metric(state, "withdrawals", stats.withdrawals);
  set_metric(state, "osr_attempts", stats.osr_attempts);
  set_metric(state, "osr_entries", stats.osr_entries);
  set_metric(state, "osr_exits", stats.osr_exits);
  set_text(state, "compilation_state", task_state_name(task_state));
  set_flag(state, "cancellation_requested", cancellation_requested);
  set_flag(state, "scheduler_available", service.scheduler != nullptr);
  set_metric(state, "scheduler_queued_tasks", scheduler_stats.queued_tasks);
  set_metric(state, "scheduler_active_workers",
             scheduler_stats.active_workers);
  set_metric(state, "code_size", code_size);
  set_metric(state, "input_ir_nodes", input_ir_nodes);
  set_metric(state, "active_ir_nodes", active_ir_nodes);
  return 1;
}

int wait_for_compiled_function(lua_State *state) {
  if (lua_gettop(state) != 2 || !lua_isinteger(state, 2)) {
    return luaL_error(
        state,
        "unijit.wait expects a compiled function and integer timeout");
  }
  const lua_Integer timeout = lua_tointeger(state, 2);
  if (timeout < 0) {
    return luaL_error(state, "unijit.wait timeout cannot be negative");
  }
  CompiledFunctionState *compiled = compiled_state_argument(state, 1);
  if (compiled == nullptr) {
    return luaL_error(state,
                      "unijit.wait expects a function from unijit.compile");
  }
  bool completed = false;
  {
    const unijit::jit::CompilationTicket ticket =
        compiled->current_ticket();
    completed =
        !ticket.valid() || ticket.wait_for(std::chrono::milliseconds(timeout));
  }
  lua_pushboolean(state, completed ? 1 : 0);
  return 1;
}

int cancel_compiled_function(lua_State *state) {
  if (lua_gettop(state) != 1) {
    return luaL_error(state, "unijit.cancel expects one compiled function");
  }
  CompiledFunctionState *compiled = compiled_state_argument(state, 1);
  if (compiled == nullptr) {
    return luaL_error(state,
                      "unijit.cancel expects a function from unijit.compile");
  }
  bool cancelled = false;
  {
    const unijit::jit::CompilationTicket ticket =
        compiled->current_ticket();
    cancelled = ticket.cancel();
  }
  lua_pushboolean(state, cancelled ? 1 : 0);
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
    try {
      std::string cache_key;
      const bool cacheable =
          prototype_cache_key(*closure->p, mode, &cache_key);
      std::shared_ptr<const PrototypeSnapshot> snapshot =
          capture_prototype(*closure->p);
      const bool tierable = cacheable && snapshot != nullptr;
      const unijit::jit::OptimizationLevel initial_level =
          tierable ? unijit::jit::OptimizationLevel::kBaseline
                   : unijit::jit::OptimizationLevel::kOptimized;
      const std::uint64_t fingerprint =
          cache_fingerprint(mode, initial_level);
      LuaService &service = lua_service();
      CodeHandle code =
          cacheable
              ? service.cache(mode, initial_level).find(cache_key, fingerprint)
              : CodeHandle{};
      if (!code.valid()) {
        CompilationResult result =
            compile_numeric_mode(*closure->p, mode, initial_level);
        if (result.ok()) {
          unijit::jit::CodeCachePublication publication =
              cacheable
                  ? service.cache(mode, initial_level)
                        .publish(cache_key, fingerprint,
                                 std::move(result.function))
                  : service.uncached_cache.publish(
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
        LoopHotnessPlan loop_hotness = analyze_loop_hotness(*closure->p);
        auto compiled_state = std::make_shared<CompiledFunctionState>(
            service.allocate_identity(), closure->p->numparams, mode,
            std::move(snapshot), std::move(cache_key), loop_hotness, tierable);
        const Status baseline_status =
            compiled_state->code.publish_baseline(std::move(code));
        if (!baseline_status.ok()) {
          std::snprintf(error, sizeof(error),
                        "unable to publish Lua baseline: %s",
                        baseline_status.message().c_str());
        } else {
          owned->state =
              new (std::nothrow) std::shared_ptr<CompiledFunctionState>(
                  std::move(compiled_state));
          if (owned->state == nullptr) {
            std::snprintf(error, sizeof(error),
                          "unable to allocate a Lua native-code lease");
          } else {
            compiled = true;
          }
        }
      }
    } catch (const std::bad_alloc &) {
      std::snprintf(error, sizeof(error),
                    "unable to allocate Lua compilation state");
    } catch (...) {
      std::snprintf(error, sizeof(error),
                    "unexpected Lua compilation failure");
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
      {"stats", compiled_function_stats},
      {"wait", wait_for_compiled_function},
      {"cancel", cancel_compiled_function},
      {nullptr, nullptr},
  };
  luaL_newlib(state, functions);
  return 1;
}
