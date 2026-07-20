#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/ir/interpreter.h"
#include "unijit/ir/optimizer.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"

namespace {

using unijit::ir::Function;
using unijit::ir::FunctionBuilder;
using unijit::ir::Interpreter;
using unijit::ir::Value;
using unijit::ir::Word;
using unijit::jit::Compiler;

int failures = 0;
std::size_t runtime_call_count = 0;

Word sum_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  Word result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    result += arguments[index];
  }
  return result;
}

Word float_runtime_helper(const Word* arguments, std::size_t count) {
  ++runtime_call_count;
  if (count != 2) {
    return unijit::ir::pack_float64(0.0);
  }
  return unijit::ir::pack_float64(
      unijit::ir::unpack_float64(arguments[0]) *
      unijit::ir::unpack_float64(arguments[1]));
}

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::unique_ptr<unijit::jit::CompiledFunction> compile_constant(Word value) {
  FunctionBuilder builder(0);
  const Value result = builder.constant(value);
  if (!builder.set_return(result).ok()) {
    return nullptr;
  }
  auto compilation = Compiler::compile(std::move(builder).build());
  return compilation.ok() ? std::move(compilation.function) : nullptr;
}

void test_code_cache_lifecycle() {
  using unijit::jit::CodeCache;
  using unijit::jit::CodeCacheLimits;
  using unijit::jit::CodeHandle;

  CodeCache cache(CodeCacheLimits{2, 1024U * 1024U});
  auto first = cache.publish("alpha", 1, compile_constant(11));
  expect(first.ok() && first.cached && !first.reused,
         "code cache must publish a new compiled function");
  expect(first.handle.parameter_count() == 0 &&
             first.handle.compilation_stats() != nullptr &&
             first.handle.compilation_stats()->executable_mapping_size >=
                 first.handle.compilation_stats()->code_size,
         "code handles must expose immutable compilation metadata");
  const CodeHandle first_lease = first.handle;

  const CodeHandle first_hit = cache.find("alpha", 1);
  const auto first_result = first_hit.invoke(nullptr, 0);
  expect(first_result.ok() && first_result.value == 11,
         "code-cache lookup must return callable native code");
  expect(!cache.find("alpha", 2).valid(),
         "code-cache lookup must reject a mismatched fingerprint");

  auto reused = cache.publish("alpha", 1, compile_constant(99));
  const auto reused_result = reused.handle.invoke(nullptr, 0);
  expect(reused.ok() && reused.cached && reused.reused &&
             reused.handle.generation() == first.handle.generation() &&
             reused_result.ok() && reused_result.value == 11,
         "duplicate publication must reuse the resident generation");

  auto replacement = cache.publish("alpha", 2, compile_constant(22));
  const auto replacement_result = replacement.handle.invoke(nullptr, 0);
  const auto stale_result = first_lease.invoke(nullptr, 0);
  expect(replacement.ok() && replacement.cached && !replacement.reused &&
             replacement.handle.generation() != first.handle.generation() &&
             replacement_result.ok() && replacement_result.value == 22,
         "new fingerprints must replace the resident generation");
  expect(stale_result.ok() && stale_result.value == 11,
         "replacement must not reclaim an active code lease");

  expect(!cache.invalidate("alpha", 1) && cache.invalidate("alpha", 2) &&
             !cache.find("alpha", 2).valid(),
         "fingerprinted invalidation must only remove the matching entry");
  const auto invalidated_lease_result = replacement.handle.invoke(nullptr, 0);
  expect(invalidated_lease_result.ok() && invalidated_lease_result.value == 22,
         "invalidated code must remain callable through an active lease");

  auto beta = cache.publish("beta", 1, compile_constant(31));
  auto gamma = cache.publish("gamma", 1, compile_constant(32));
  expect(beta.ok() && gamma.ok() && cache.find("beta", 1).valid(),
         "LRU fixture must populate and touch two entries");
  auto delta = cache.publish("delta", 1, compile_constant(33));
  expect(delta.ok() && cache.find("beta", 1).valid() &&
             !cache.find("gamma", 1).valid() &&
             cache.find("delta", 1).valid(),
         "entry budget must evict the least-recently-used generation");

  const CodeHandle surviving_clear = beta.handle;
  cache.clear();
  const auto clear_result = surviving_clear.invoke(nullptr, 0);
  const auto statistics = cache.stats();
  expect(clear_result.ok() && clear_result.value == 31,
         "cache clear must preserve active code leases");
  expect(statistics.resident_entries == 0 &&
             statistics.resident_code_bytes == 0 &&
             statistics.hits >= 4 && statistics.misses >= 3 &&
             statistics.publication_reuses == 1 &&
             statistics.replacements == 1 &&
             statistics.invalidations == 1 && statistics.evictions == 1 &&
             statistics.clears == 1,
         "code cache must report bounded lifecycle metrics");

  CodeCache disabled(CodeCacheLimits{0, 0});
  auto uncached = disabled.publish("bounded", 1, compile_constant(41));
  const auto uncached_result = uncached.handle.invoke(nullptr, 0);
  expect(uncached.ok() && !uncached.cached && uncached_result.ok() &&
             uncached_result.value == 41 &&
             disabled.stats().uncached_publications == 1,
         "disabled caching must still return an owned callable lease");

  CodeHandle surviving_cache;
  {
    CodeCache temporary;
    auto publication =
        temporary.publish("survivor", 7, compile_constant(77));
    surviving_cache = publication.handle;
  }
  const auto destroyed_cache_result = surviving_cache.invoke(nullptr, 0);
  expect(destroyed_cache_result.ok() && destroyed_cache_result.value == 77,
         "destroying a cache must not reclaim an active lease");
}

void test_code_cache_concurrency() {
  using unijit::jit::CodeCache;
  using unijit::jit::CodeCacheLimits;

  CodeCache cache(CodeCacheLimits{2, 1024U * 1024U});
  expect(cache.publish("hot", 1, compile_constant(101)).ok(),
         "concurrent cache fixture must publish its first generation");

  std::atomic<bool> start{false};
  std::atomic<std::size_t> errors{0};
  std::vector<std::thread> readers;
  for (std::size_t thread_index = 0; thread_index < 4; ++thread_index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
        for (std::uint64_t fingerprint = 1; fingerprint <= 2;
             ++fingerprint) {
          const auto handle = cache.find("hot", fingerprint);
          if (!handle.valid()) {
            continue;
          }
          const auto result = handle.invoke(nullptr, 0);
          const Word expected = fingerprint == 1 ? 101 : 202;
          if (!result.ok() || result.value != expected) {
            errors.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::size_t iteration = 0; iteration < 128; ++iteration) {
    const std::uint64_t fingerprint = (iteration & 1U) == 0 ? 2 : 1;
    const Word value = fingerprint == 1 ? 101 : 202;
    const auto publication =
        cache.publish("hot", fingerprint, compile_constant(value));
    if (!publication.ok()) {
      errors.fetch_add(1, std::memory_order_relaxed);
    }
    if ((iteration % 11U) == 0) {
      (void)cache.invalidate("hot", fingerprint);
    }
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  expect(errors.load(std::memory_order_relaxed) == 0,
         "concurrent lookup, replacement, invalidation, and invocation must be safe");
}

Function arithmetic_function() {
  FunctionBuilder builder(2);
  const Value sum = builder.add(builder.parameter(0), builder.parameter(1));
  const Value scaled = builder.multiply(sum, builder.constant(-7));
  const Value result = builder.subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "arithmetic return must be accepted");
  return std::move(builder).build();
}

void test_verifier_rejects_forward_reference() {
  FunctionBuilder builder(0);
  const Value invalid_binary = builder.add(Value{7}, Value{8});
  expect(builder.set_return(invalid_binary).ok(),
         "builder records a structurally present return value");
  const Function function = std::move(builder).build();
  const unijit::Status status = unijit::ir::verify(function);
  expect(!status.ok(), "verifier must reject non-dominating SSA operands");
  expect(status.location() == 0, "verifier must report the invalid node index");
}

void test_constant_native_function() {
  FunctionBuilder builder(0);
  const Value constant = builder.constant(std::numeric_limits<Word>::min());
  expect(builder.set_return(constant).ok(), "constant return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok(), "constant native function must execute");
  expect(result.value == std::numeric_limits<Word>::min(),
         "native constant materialization must preserve all 64 bits");
}

void test_execution_context_lifecycle() {
  unijit::runtime::ExecutionContext context;
  expect(!context.interrupt_requested(),
         "new execution contexts must not request interruption");
  context.request_interrupt();
  expect(context.interrupt_requested(),
         "execution contexts must publish interruption requests");
  context.clear_interrupt();
  expect(!context.interrupt_requested(),
         "execution contexts must clear interruption requests");

  context.record_exit(unijit::runtime::ExitReason::kRuntime, 91, -17);
  expect(context.exit_reason() == unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 91 && context.exit_value() == -17,
         "execution contexts must retain runtime-exit diagnostics");

  FunctionBuilder builder(0);
  expect(builder.set_return(builder.constant(73)).ok(),
         "execution-context fixture must have a return value");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "execution-context fixture must compile");
  if (compilation.ok()) {
    const auto result = compilation.function->invoke(nullptr, 0, &context);
    expect(result.ok() && result.value == 73,
           "invocation must clear stale exits before entering native code");
    expect(context.exit_reason() == unijit::runtime::ExitReason::kNone,
           "successful invocation must leave no exit reason");
  }
}

void test_safepoint_ir_and_interpreter() {
  FunctionBuilder builder(0);
  const Value base = builder.call(
      sum_runtime_helper, {builder.constant(70), builder.constant(3)});
  const Value safepoint = builder.safepoint(42);
  expect(safepoint.valid(), "safepoint must produce an effect value");
  const Value result = builder.add(base, safepoint);
  expect(builder.set_return(result).ok(),
         "safepoint fixture must have a return value");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "safepoint IR must verify");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "safepoint IR must optimize");
  if (optimization.ok()) {
    const bool preserved = std::any_of(
        optimization.function.nodes().begin(),
        optimization.function.nodes().end(), [](const unijit::ir::Node& node) {
          return node.opcode == unijit::ir::Opcode::kSafepoint;
        });
    expect(preserved, "optimizer must preserve a dead-result safepoint");
  }

  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interrupted = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(!interrupted.ok() &&
             interrupted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interrupted.status.location() == 42 &&
             context.exit_reason() ==
                 unijit::runtime::ExitReason::kSafepoint &&
             context.exit_site() == 42,
         "interpreter safepoints must report interruption and site identity");

  context.clear_interrupt();
  const auto completed = Interpreter::evaluate(function, nullptr, 0, &context);
  expect(completed.ok() && completed.value == 73 &&
             context.exit_reason() == unijit::runtime::ExitReason::kNone,
         "clear safepoints must continue without changing the result");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "safepoint IR must compile to native code");
  if (compilation.ok()) {
    context.request_interrupt();
    const auto native_interrupted =
        compilation.function->invoke(nullptr, 0, &context);
    expect(!native_interrupted.ok() &&
               native_interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               native_interrupted.status.location() == 42 &&
               context.exit_reason() ==
                   unijit::runtime::ExitReason::kSafepoint,
           "native safepoints must exit with the matching site identity");

    context.clear_interrupt();
    const auto native_completed =
        compilation.function->invoke(nullptr, 0, &context);
    expect(native_completed.ok() && native_completed.value == 73,
           "native clear safepoints must continue with a zero effect value");
    expect(compilation.function->native_entry()(nullptr, nullptr) == 73,
           "a null execution context must bypass safepoint polling");
  }
}

void test_differential_arithmetic() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "arithmetic function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }

  std::mt19937_64 random(0x554E494A4954ULL);
  for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
    const std::array<Word, 2> args = {static_cast<Word>(random()),
                                      static_cast<Word>(random())};
    const auto interpreted =
        Interpreter::evaluate(function, args.data(), args.size());
    const auto native = compilation.function->invoke(args.data(), args.size());
    if (!interpreted.ok() || !native.ok() ||
        interpreted.value != native.value) {
      expect(false, "native arithmetic must match the interpreter oracle");
      return;
    }
  }
}

Function float64_function() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value sum =
      builder.float64_add(builder.parameter(0), builder.parameter(1));
  const Value scaled =
      builder.float64_multiply(sum, builder.float64_constant(0.5));
  const Value result = builder.float64_subtract(scaled, builder.parameter(1));
  expect(builder.set_return(result).ok(), "Float64 return must be accepted");
  return std::move(builder).build();
}

void test_float64_ir_and_interpreter() {
  const Function function = float64_function();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 SSA must pass verification");
  expect(function.return_type() == unijit::ir::ValueType::kFloat64,
         "Float64 result type must remain visible in the function");

  const std::array<Word, 2> args = {unijit::ir::pack_float64(19.25),
                                    unijit::ir::pack_float64(-4.75)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok(), "Float64 interpreter execution must succeed");
  expect(unijit::ir::unpack_float64(interpreted.value) == 12.0,
         "Float64 interpreter must preserve IEEE-754 arithmetic");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             optimization.function.return_type() ==
                 unijit::ir::ValueType::kFloat64,
         "optimizer must preserve Float64 types and operations");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 SSA must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  const std::array<double, 8> samples = {0.0,      -0.0,   1.25,   -7.5,
                                         1024.75, -33.125, 1.0e12, -1.0e-9};
  for (std::size_t lhs = 0; lhs < samples.size(); ++lhs) {
    const std::array<Word, 2> native_args = {
        unijit::ir::pack_float64(samples[lhs]),
        unijit::ir::pack_float64(samples[samples.size() - lhs - 1])};
    const auto expected =
        Interpreter::evaluate(function, native_args.data(), native_args.size());
    const auto native =
        compilation.function->invoke(native_args.data(), native_args.size());
    expect(native.ok() && expected.ok() && native.value == expected.value,
           "native Float64 arithmetic must match the interpreter bits");
  }
}

void test_float64_division() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value quotient =
      builder.float64_divide(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(quotient).ok(),
         "Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 division must pass verification");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 division must compile to native code");
  if (!compilation.ok()) {
    return;
  }

  constexpr std::array<std::array<double, 2>, 6> kSamples = {{
      {{9.0, 3.0}},
      {{-7.5, 2.5}},
      {{0.0, -3.0}},
      {{-0.0, 7.0}},
      {{1.0e-300, 2.0}},
      {{std::numeric_limits<double>::max(), 2.0}},
  }};
  for (const auto& sample : kSamples) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(sample[0]),
        unijit::ir::pack_float64(sample[1])};
    const auto interpreted =
        Interpreter::evaluate(function, arguments.data(), arguments.size());
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(interpreted.ok() && native.ok() &&
               native.value == interpreted.value,
           "native Float64 division must match the interpreter bits");
  }
}

void test_float64_nonzero_guard() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value guard = builder.guard_float64_nonzero(builder.parameter(1), 77);
  expect(guard.valid(), "Float64 nonzero guard must produce an effect value");
  const Value quotient =
      builder.float64_divide(builder.parameter(0), builder.parameter(1));
  expect(builder.set_return(quotient).ok(),
         "guarded Float64 division fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 nonzero guard must pass verification");

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::any_of(optimization.function.nodes().begin(),
                         optimization.function.nodes().end(),
                         [](const unijit::ir::Node& node) {
                           return node.opcode ==
                                  unijit::ir::Opcode::kGuardFloatNonzero;
                         }),
         "optimizer must preserve a dead-result Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 nonzero guard must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  const auto* default_deoptimization =
      compilation.function->deoptimization_record(77);
  expect(default_deoptimization != nullptr &&
             default_deoptimization->reason ==
                 unijit::runtime::DeoptimizationReason::kGuardFailed &&
             default_deoptimization->recovery.size() == 1,
         "generic guards must receive default reconstruction metadata");

  const std::array<Word, 2> valid = {unijit::ir::pack_float64(9.0),
                                     unijit::ir::pack_float64(3.0)};
  const auto interpreted =
      Interpreter::evaluate(function, valid.data(), valid.size());
  const auto native = compilation.function->invoke(valid.data(), valid.size());
  expect(interpreted.ok() && native.ok() &&
             unijit::ir::unpack_float64(native.value) == 3.0,
         "a nonzero Float64 divisor must pass the guard");

  constexpr std::array<double, 2> kZeroes = {0.0, -0.0};
  for (double zero : kZeroes) {
    const std::array<Word, 2> invalid = {unijit::ir::pack_float64(9.0),
                                         unijit::ir::pack_float64(zero)};
    unijit::runtime::ExecutionContext interpreter_context;
    const auto rejected_by_interpreter = Interpreter::evaluate(
        function, invalid.data(), invalid.size(), &interpreter_context);
    expect(!rejected_by_interpreter.ok() &&
               rejected_by_interpreter.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_by_interpreter.status.location() == 77 &&
               interpreter_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime &&
               interpreter_context.exit_value() == invalid[1],
           "interpreter guard must reject both signed Float64 zeroes");

    const auto rejected_with_local_context =
        compilation.function->invoke(invalid.data(), invalid.size());
    expect(!rejected_with_local_context.ok() &&
               rejected_with_local_context.status.code() ==
                   unijit::StatusCode::kRuntimeExit &&
               rejected_with_local_context.status.location() == 77,
           "managed invocation must diagnose a guard without caller context");

    unijit::runtime::ExecutionContext native_context;
    const auto rejected_by_native = compilation.function->invoke(
        invalid.data(), invalid.size(), &native_context);
    expect(!rejected_by_native.ok() &&
               native_context.exit_reason() ==
                   unijit::runtime::ExitReason::kRuntime &&
               native_context.exit_site() == 77 &&
               native_context.exit_value() == invalid[1],
           "native guard must publish its stable exit site and value bits");
  }
}

void test_deoptimization_reconstruction() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value divisor = builder.parameter(1);
  expect(builder.guard_float64_nonzero(divisor, 113).valid(),
         "deoptimization fixture must create its guard");
  const Value quotient =
      builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "deoptimization fixture must record its result");
  const Function function = std::move(builder).build();

  unijit::runtime::DeoptimizationRecord record;
  record.site = 113;
  record.resume_offset = 29;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          0, unijit::ir::ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::argument(
          1, unijit::ir::ValueType::kFloat64, 1),
      unijit::runtime::RecoveryOperation::constant_value(
          2, unijit::ir::ValueType::kWord, 41),
      unijit::runtime::RecoveryOperation::exit_value(
          3, unijit::ir::ValueType::kFloat64)};
  unijit::runtime::DeoptimizationTable metadata;
  expect(metadata.add(record).ok(),
         "valid deoptimization metadata must be accepted");
  expect(!metadata.add(record).ok(),
         "duplicate deoptimization sites must be rejected");

  auto compilation = Compiler::compile(function, metadata);
  expect(compilation.ok(),
         "a function with explicit deoptimization metadata must compile");
  if (!compilation.ok()) {
    return;
  }
  const auto* compiled_record =
      compilation.function->deoptimization_record(113);
  expect(compilation.function->deoptimization_table().size() == 1 &&
             compiled_record != nullptr &&
             compiled_record->reason ==
                 unijit::runtime::DeoptimizationReason::kDivisionByZero &&
             compiled_record->resume_offset == 29,
         "compiled functions must retain immutable deoptimization records");

  const std::array<Word, 2> arguments = {
      unijit::ir::pack_float64(9.0), unijit::ir::pack_float64(-0.0)};
  unijit::runtime::ExecutionContext context;
  const auto exited = compilation.function->invoke(
      arguments.data(), arguments.size(), &context);
  expect(!exited.ok() &&
             exited.status.code() == unijit::StatusCode::kRuntimeExit &&
             context.exit_value() == arguments[1],
         "a guarded exit must retain the exact triggering value bits");

  const auto reconstruction =
      compilation.function->reconstruct_deoptimization(
          exited.status.location(), arguments.data(), arguments.size(),
          context);
  const auto* recovered_argument = reconstruction.frame.find(0);
  const auto* recovered_constant = reconstruction.frame.find(2);
  const auto* recovered_exit = reconstruction.frame.find(3);
  expect(reconstruction.ok() && reconstruction.frame.site == 113 &&
             reconstruction.frame.resume_offset == 29 &&
             reconstruction.frame.reason ==
                 unijit::runtime::DeoptimizationReason::kDivisionByZero &&
             recovered_argument != nullptr &&
             recovered_argument->value == arguments[0] &&
             recovered_constant != nullptr && recovered_constant->value == 41 &&
             recovered_exit != nullptr &&
             recovered_exit->value == arguments[1],
         "deoptimization must reconstruct arguments, constants, and exits");

  unijit::runtime::ExecutionContext mismatched_context;
  mismatched_context.record_exit(unijit::runtime::ExitReason::kRuntime, 114);
  expect(!compilation.function
              ->reconstruct_deoptimization(113, arguments.data(),
                                           arguments.size(),
                                           mismatched_context)
              .ok(),
         "reconstruction must reject stale or mismatched execution contexts");

  unijit::jit::CodeCache cache;
  const auto publication = cache.publish(
      "deoptimization-fixture", 1, std::move(compilation.function));
  expect(publication.ok() &&
             publication.handle.deoptimization_record(113) != nullptr,
         "code-cache leases must retain deoptimization metadata");
  const auto cached_reconstruction =
      publication.handle.reconstruct_deoptimization(
          113, arguments.data(), arguments.size(), context);
  expect(cached_reconstruction.ok() &&
             cached_reconstruction.frame.find(3) != nullptr &&
             cached_reconstruction.frame.find(3)->value == arguments[1],
         "cached execution leases must reconstruct diagnosed exits");

  unijit::runtime::DeoptimizationRecord unknown_site = record;
  unknown_site.site = 999;
  unijit::runtime::DeoptimizationTable invalid_metadata;
  expect(invalid_metadata.add(unknown_site).ok() &&
             !Compiler::compile(function, invalid_metadata).ok(),
         "compilation must reject metadata for a nonexistent guard site");

  auto colliding_assumption =
      std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet colliding_assumptions;
  expect(colliding_assumptions.add(colliding_assumption, 113, 29).ok() &&
             !Compiler::compile(function, metadata, colliding_assumptions)
                  .ok(),
         "runtime guards and assumptions must not share an exit site");
}

void test_constant_float64_nonzero_guard_elimination() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value divisor = builder.float64_constant(2.0);
  const Value guard = builder.guard_float64_nonzero(divisor, 81);
  expect(guard.valid(), "constant nonzero guard fixture must be constructible");
  const Value quotient = builder.float64_divide(builder.parameter(0), divisor);
  expect(builder.set_return(quotient).ok(),
         "constant nonzero guard fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok() &&
             std::none_of(optimization.function.nodes().begin(),
                          optimization.function.nodes().end(),
                          [](const unijit::ir::Node& node) {
                            return node.opcode ==
                                   unijit::ir::Opcode::kGuardFloatNonzero;
                          }),
         "optimizer must eliminate a provably passing Float64 guard");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "constant-guard fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(!compilation.function->requires_context(),
         "eliminated guards must not require a managed execution context");
  expect(compilation.function->deoptimization_table().empty(),
         "eliminated guards must not retain stale deoptimization metadata");
  const std::array<Word, 1> arguments = {unijit::ir::pack_float64(9.0)};
  const auto result =
      compilation.function->invoke(arguments.data(), arguments.size());
  expect(result.ok() && unijit::ir::unpack_float64(result.value) == 4.5,
         "constant-guard elimination must preserve the quotient");
}

void test_verifier_rejects_mixed_arithmetic() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  const Value invalid =
      builder.add(builder.parameter(0), builder.float64_constant(1.0));
  expect(builder.set_return(invalid).ok(),
         "mixed-type fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject mixed Word and Float64 arithmetic");
}

void test_float64_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(std::vector<unijit::ir::ValueType>(
      kParameters, unijit::ir::ValueType::kFloat64));
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(
            builder.float64_add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "Float64 spill fixture must return its reduction");
  const Function function = std::move(builder).build();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 register pressure must compile");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "Float64 register pressure must exercise spill slots");

  std::array<Word, kParameters> args{};
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = unijit::ir::pack_float64(
        static_cast<double>(index + 1) * 0.25);
  }
  const auto expected =
      Interpreter::evaluate(function, args.data(), args.size());
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && expected.ok() && native.value == expected.value,
         "spilled native Float64 values must match the interpreter bits");
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool preserves_host_float_registers(unijit::jit::NativeEntry entry) {
  double lhs = 1.25;
  double rhs = -7.5;
  for (std::size_t iteration = 0; iteration < 512; ++iteration) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const double expected = (lhs + rhs) * (lhs - 3.25) + rhs * 0.75;
    const double native =
        unijit::ir::unpack_float64(entry(arguments.data(), nullptr));
    if (native != expected) {
      return false;
    }
    lhs += 0.125;
    rhs -= 0.0625;
  }
  return lhs == 65.25 && rhs == -39.5;
}

void test_float64_preserves_host_abi() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value lhs = builder.parameter(0);
  const Value rhs = builder.parameter(1);
  const Value product = builder.float64_multiply(
      builder.float64_add(lhs, rhs),
      builder.float64_subtract(lhs, builder.float64_constant(3.25)));
  const Value result = builder.float64_add(
      product,
      builder.float64_multiply(rhs, builder.float64_constant(0.75)));
  expect(builder.set_return(result).ok(),
         "host-ABI fixture must return its Float64 expression");
  auto compilation = Compiler::compile(std::move(builder).build());
  expect(compilation.ok(), "host-ABI Float64 fixture must compile");
  if (compilation.ok()) {
    expect(preserves_host_float_registers(
               compilation.function->native_entry()),
           "native Float64 code must preserve host callee-saved registers");
  }
}

void test_runtime_helper_call() {
  FunctionBuilder builder(2);
  const Value live = builder.add(builder.parameter(0), builder.constant(5));
  const Value called = builder.call(
      sum_runtime_helper, {live, builder.parameter(1), builder.constant(7)});
  const Value result = builder.multiply(live, called);
  expect(builder.set_return(result).ok(),
         "runtime-call fixture must record its result");
  const Function function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "runtime helper call must satisfy SSA verification");

  const std::array<Word, 2> args = {3, 4};
  runtime_call_count = 0;
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 152 &&
             runtime_call_count == 1,
         "interpreter must execute runtime helpers with ordered arguments");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "runtime helper call must compile to native code");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native runtime call must preserve live values and helper effects");
}

void test_effectful_dead_runtime_call() {
  FunctionBuilder builder(0);
  const Value ignored = builder.call(sum_runtime_helper, {});
  (void)ignored;
  expect(builder.set_return(builder.constant(42)).ok(),
         "effectful-call fixture must record a return");
  const Function function = std::move(builder).build();
  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept an effectful runtime call");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "dead-result runtime call must remain compilable");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto result = compilation.function->invoke(nullptr, 0);
  expect(result.ok() && result.value == 42 && runtime_call_count == 1,
         "optimizer must preserve runtime calls whose result is dead");
}

void test_float64_runtime_helper_call() {
  FunctionBuilder builder(
      std::vector<unijit::ir::ValueType>(2,
                                         unijit::ir::ValueType::kFloat64));
  const Value live =
      builder.float64_add(builder.parameter(0), builder.float64_constant(0.5));
  const Value called = builder.call(
      float_runtime_helper, {live, builder.parameter(1)},
      unijit::ir::ValueType::kFloat64);
  const Value result = builder.float64_add(live, called);
  expect(builder.set_return(result).ok(),
         "Float64 runtime call must record its result");
  const Function function = std::move(builder).build();
  const std::array<Word, 2> args = {unijit::ir::pack_float64(3.5),
                                    unijit::ir::pack_float64(-2.0)};
  const auto interpreted =
      Interpreter::evaluate(function, args.data(), args.size());
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 runtime helper call must compile");
  if (!compilation.ok()) {
    return;
  }
  runtime_call_count = 0;
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && interpreted.ok() && native.value == interpreted.value &&
             runtime_call_count == 1,
         "native Float64 call must preserve live FP registers and result bits");
}

void test_verifier_rejects_null_runtime_helper() {
  FunctionBuilder builder(0);
  const Value invalid = builder.call(nullptr, {});
  expect(builder.set_return(invalid).ok(),
         "null-helper fixture must contain a return");
  expect(!unijit::ir::verify(std::move(builder).build()).ok(),
         "verifier must reject a null runtime helper target");
}

void test_spill_path() {
  constexpr std::size_t kParameters = 16;
  FunctionBuilder builder(kParameters);
  std::vector<Value> values;
  values.reserve(kParameters);
  for (std::size_t index = 0; index < kParameters; ++index) {
    values.push_back(builder.parameter(index));
  }
  while (values.size() > 1) {
    std::vector<Value> reduced;
    reduced.reserve((values.size() + 1) / 2);
    for (std::size_t index = 0; index < values.size(); index += 2) {
      if (index + 1 < values.size()) {
        reduced.push_back(builder.add(values[index], values[index + 1]));
      } else {
        reduced.push_back(values[index]);
      }
    }
    values = std::move(reduced);
  }
  expect(builder.set_return(values.front()).ok(),
         "spill return must be accepted");
  const Function function = std::move(builder).build();

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "register-pressure function must compile");
  if (!compilation.ok()) {
    std::cerr << compilation.status.message() << '\n';
    return;
  }
  expect(compilation.function->stats().spill_slots > 0,
         "register-pressure function must exercise spill slots");

  std::array<Word, kParameters> args{};
  Word expected = 0;
  for (std::size_t index = 0; index < args.size(); ++index) {
    args[index] = static_cast<Word>(index + 1);
    expected += args[index];
  }
  const auto result = compilation.function->invoke(args.data(), args.size());
  expect(result.ok(), "spilled native function must execute");
  expect(result.value == expected, "spilled values must survive allocation");
}

void test_argument_validation() {
  const Function function = arithmetic_function();
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "validation fixture must compile");
  if (!compilation.ok()) {
    return;
  }
  const std::array<Word, 1> too_few = {1};
  expect(!compilation.function->invoke(too_few.data(), too_few.size()).ok(),
         "compiled invocation must reject the wrong argument count");
  expect(!Interpreter::evaluate(function, nullptr, 2).ok(),
         "interpreter must reject null argument storage");
}

void test_optimization_pipeline() {
  FunctionBuilder builder(1);
  const Value zero = builder.constant(0);
  const Value dead_sum =
      builder.add(builder.parameter(0), builder.constant(99));
  const Value annihilated = builder.multiply(dead_sum, zero);
  const Value answer = builder.add(annihilated, builder.constant(42));
  expect(builder.set_return(answer).ok(), "optimized return must be accepted");
  const Function function = std::move(builder).build();

  auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimization pipeline must accept verified SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.input_nodes == 7,
         "optimizer must report the input SSA size");
  expect(optimization.stats.output_nodes == 2,
         "optimizer must fold the graph to one parameter and one constant");
  expect(optimization.stats.constants_folded > 0,
         "optimizer must exercise constant folding");
  expect(optimization.stats.algebraic_simplifications > 0,
         "optimizer must exercise algebraic simplification");

  const std::array<Word, 1> args = {std::numeric_limits<Word>::max()};
  const auto interpreted =
      Interpreter::evaluate(optimization.function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 42,
         "optimized SSA must preserve observable semantics");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "compiler must consume optimized SSA");
  if (!compilation.ok()) {
    return;
  }
  expect(compilation.function->stats().input_ir_nodes == 7,
         "compiler must report input IR nodes");
  expect(compilation.function->stats().optimized_ir_nodes == 2,
         "compiler must report optimized IR nodes");
  const auto native = compilation.function->invoke(args.data(), args.size());
  expect(native.ok() && native.value == 42,
         "optimized native code must preserve observable semantics");
}

void test_float64_constant_folding() {
  FunctionBuilder builder(0);
  const Value numerator = builder.float64_add(
      builder.float64_constant(12.0), builder.float64_constant(5.0));
  const Value quotient =
      builder.float64_divide(numerator, builder.float64_constant(2.0));
  expect(builder.set_return(quotient).ok(),
         "Float64 folding fixture must record its result");
  const Function function = std::move(builder).build();

  const auto optimization = unijit::ir::Optimizer::run(function);
  expect(optimization.ok(), "optimizer must accept constant Float64 SSA");
  if (!optimization.ok()) {
    return;
  }
  expect(optimization.stats.output_nodes == 1 &&
             optimization.stats.constants_folded == 2,
         "optimizer must fold chained Float64 arithmetic to one constant");
  const auto interpreted =
      Interpreter::evaluate(optimization.function, nullptr, 0);
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(8.5),
         "folded Float64 constants must preserve result bits");
}

void test_control_flow_counted_loop() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);

  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {one, zero}).ok(),
         "entry must jump to the loop header");

  expect(builder.set_insertion_block(loop).ok(),
         "loop block must accept insertion");
  const Value index = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value next_sum = builder.add(sum, index);
  const Value next_index = builder.add(index, one);
  const Value continue_loop =
      builder.less_equal(next_index, builder.parameter(0));
  expect(
      builder
          .branch(continue_loop, loop, {next_index, next_sum}, exit, {next_sum})
          .ok(),
      "loop latch must branch with explicit block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "exit block must accept insertion");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "exit block must return its block parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "counted-loop CFG must satisfy dominance and edge invariants");
  const std::array<Word, 1> args = {100};
  const auto result = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(result.ok() && result.value == 5050,
         "CFG interpreter must execute loop-carried block parameters");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "counted-loop CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == result.value,
           "native counted loop must match the CFG interpreter");
  }
}

void test_control_flow_float64_loop() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block loop = builder.create_block(
      std::vector<ValueType>{ValueType::kWord, ValueType::kFloat64});
  const unijit::ir::Block exit =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1)}).ok(),
         "typed CFG entry must pass Word and Float64 parameters");

  expect(builder.set_insertion_block(loop).ok(),
         "typed Float64 loop block must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value accumulator = builder.block_parameter(loop, 1);
  const Value scaled = builder.float64_multiply(
      accumulator, builder.float64_constant(1.5));
  const Value adjusted = builder.float64_subtract(
      scaled, builder.float64_constant(0.25));
  const Value divided =
      builder.float64_divide(adjusted, builder.float64_constant(2.0));
  const Value next_accumulator =
      builder.float64_add(divided, builder.float64_constant(0.5));
  const Value next_remaining = builder.subtract(remaining, builder.constant(1));
  const Value continues =
      builder.less_than(builder.constant(0), next_remaining);
  expect(builder
             .branch(continues, loop,
                     {next_remaining, next_accumulator}, exit,
                     {next_accumulator})
             .ok(),
         "typed Float64 loop must preserve edge types");

  expect(builder.set_insertion_block(exit).ok(),
         "typed Float64 exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "typed Float64 exit must return its block parameter");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "typed Float64 CFG must satisfy verifier invariants");

  double expected = 1.0;
  for (int iteration = 0; iteration < 4; ++iteration) {
    expected = ((expected * 1.5) - 0.25) / 2.0 + 0.5;
  }
  const std::array<Word, 2> arguments = {
      4, unijit::ir::pack_float64(1.0)};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, arguments.data(), arguments.size());
  expect(interpreted.ok() &&
             interpreted.value == unijit::ir::pack_float64(expected),
         "typed CFG interpreter must preserve Float64 loop values");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "typed Float64 CFG must compile to native code");
  if (compilation.ok()) {
    const auto native =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(native.ok() && native.value == interpreted.value,
           "native typed Float64 CFG must match the interpreter");
  }
}

void test_control_flow_rejects_mixed_edge_types() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target =
      builder.create_block(std::vector<ValueType>{ValueType::kFloat64});
  expect(!builder.jump(target, {builder.constant(1)}).ok(),
         "CFG builder must reject a Word edge argument for Float64 parameter");
}

void test_control_flow_float64_comparisons() {
  using unijit::ir::ValueType;
  unijit::ir::ControlFlowBuilder builder(
      std::vector<ValueType>{ValueType::kFloat64, ValueType::kFloat64});
  const Value less =
      builder.float64_less_than(builder.parameter(0), builder.parameter(1));
  const Value less_equal =
      builder.float64_less_equal(builder.parameter(0), builder.parameter(1));
  const Value encoded =
      builder.add(builder.multiply(less, builder.constant(10)), less_equal);
  expect(builder.set_return(encoded).ok(),
         "Float64 CFG comparison fixture must return its encoded flags");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "Float64 CFG comparisons must satisfy type verification");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "Float64 CFG comparisons must compile");

  const auto check = [&](double lhs, double rhs, Word expected,
                         const char *message) {
    const std::array<Word, 2> arguments = {
        unijit::ir::pack_float64(lhs), unijit::ir::pack_float64(rhs)};
    const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
        function, arguments.data(), arguments.size());
    expect(interpreted.ok() && interpreted.value == expected, message);
    if (compilation.ok()) {
      const auto native =
          compilation.function->invoke(arguments.data(), arguments.size());
      expect(native.ok() && native.value == interpreted.value,
             "native Float64 CFG comparison must match the interpreter");
    }
  };
  check(1.0, 2.0, 11, "less operands must set both comparison flags");
  check(2.0, 2.0, 1, "equal operands must only set the inclusive flag");
  check(3.0, 2.0, 0, "greater operands must clear both comparison flags");
  check(std::numeric_limits<double>::quiet_NaN(), 2.0, 0,
        "unordered operands must clear both comparison flags");
}

void test_control_flow_merge() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block take_lhs = builder.create_block(0);
  const unijit::ir::Block take_rhs = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition =
      builder.less_than(builder.parameter(0), builder.parameter(1));
  expect(builder.branch(condition, take_rhs, {}, take_lhs, {}).ok(),
         "entry comparison must branch to both arms");

  expect(builder.set_insertion_block(take_lhs).ok(),
         "left arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(0)}).ok(),
         "left arm must pass its result to the merge");
  expect(builder.set_insertion_block(take_rhs).ok(),
         "right arm must accept insertion");
  expect(builder.jump(merge, {builder.parameter(1)}).ok(),
         "right arm must pass its result to the merge");
  expect(builder.set_insertion_block(merge).ok(),
         "merge block must accept insertion");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "merge must return its explicit SSA parameter");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "diamond CFG must pass the dominance verifier");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {17, 91}).value ==
          91,
      "true branch must select the right operand");
  expect(
      unijit::ir::ControlFlowInterpreter::evaluate(function, {117, -4}).value ==
          117,
      "false branch must select the left operand");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "diamond CFG must compile to native code");
  if (compilation.ok()) {
    const std::array<Word, 2> true_args = {17, 91};
    const std::array<Word, 2> false_args = {117, -4};
    expect(compilation.function->invoke(true_args.data(), true_args.size())
                   .value == 91,
           "native true branch must select the right operand");
    expect(compilation.function->invoke(false_args.data(), false_args.size())
                   .value == 117,
           "native false branch must select the left operand");
  }
}

void test_control_flow_parallel_edge_copy() {
  unijit::ir::ControlFlowBuilder builder(2);
  const unijit::ir::Block loop = builder.create_block(3);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  const Value two = builder.constant(2);
  const Value hundred = builder.constant(100);
  expect(builder.jump(loop, {builder.parameter(0), builder.parameter(1), two})
             .ok(),
         "parallel-copy fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "parallel-copy loop block must exist");
  const Value lhs = builder.block_parameter(loop, 0);
  const Value rhs = builder.block_parameter(loop, 1);
  const Value remaining = builder.block_parameter(loop, 2);
  const Value encoded = builder.add(builder.multiply(lhs, hundred), rhs);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {rhs, lhs, next_remaining}, exit,
                     {encoded})
             .ok(),
         "backedge must support swapped block arguments");

  expect(builder.set_insertion_block(exit).ok(),
         "parallel-copy exit block must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "parallel-copy exit must return its encoded pair");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  const std::array<Word, 2> args = {3, 7};
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size());
  expect(interpreted.ok() && interpreted.value == 703,
         "interpreter edge copies must be parallel");
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "parallel-copy CFG must compile to native code");
  if (compilation.ok()) {
    const auto native = compilation.function->invoke(args.data(), args.size());
    expect(native.ok() && native.value == interpreted.value,
           "native edge copies must preserve swapped loop parameters");
  }
}

void test_control_flow_preserves_nonlocal_merge_state() {
  using unijit::ir::ValueType;
  const std::vector<ValueType> state_types(4, ValueType::kFloat64);
  unijit::ir::ControlFlowBuilder builder(state_types);
  const unijit::ir::Block state = builder.create_block(state_types);
  const unijit::ir::Block true_arm = builder.create_block(0);
  const unijit::ir::Block false_arm = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(state_types);
  expect(builder
             .jump(state, {builder.parameter(0), builder.parameter(1),
                           builder.parameter(2), builder.parameter(3)})
             .ok(),
         "nonlocal merge fixture must enter its state block");

  expect(builder.set_insertion_block(state).ok(),
         "nonlocal merge state block must exist");
  const Value condition = builder.float64_less_than(
      builder.block_parameter(state, 1), builder.float64_constant(10.0));
  expect(builder.branch(condition, true_arm, {}, false_arm, {}).ok(),
         "nonlocal merge fixture must branch through empty arms");

  const std::vector<Value> state_values = {
      builder.block_parameter(state, 0), builder.block_parameter(state, 1),
      builder.block_parameter(state, 2), builder.block_parameter(state, 3)};
  expect(builder.set_insertion_block(true_arm).ok() &&
             builder.jump(merge, state_values).ok(),
         "true arm must forward nonlocal state");
  expect(builder.set_insertion_block(false_arm).ok() &&
             builder.jump(merge, state_values).ok(),
         "false arm must forward nonlocal state");
  expect(builder.set_insertion_block(merge).ok() &&
             builder.set_return(builder.block_parameter(merge, 2)).ok(),
         "merge must return the third state value");

  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "nonlocal merge state must satisfy CFG verification");
  const std::array<Word, 4> arguments = {
      unijit::ir::pack_float64(1.0), unijit::ir::pack_float64(2.0),
      unijit::ir::pack_float64(3.0), unijit::ir::pack_float64(4.0)};
  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "nonlocal merge state must compile");
  if (compilation.ok()) {
    const auto result =
        compilation.function->invoke(arguments.data(), arguments.size());
    expect(result.ok() &&
               result.value == unijit::ir::pack_float64(3.0),
           "edge copies must preserve simultaneously live nonlocal state");
  }
}

void test_control_flow_rejects_non_dominating_value() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block left = builder.create_block(0);
  const unijit::ir::Block right = builder.create_block(0);
  const unijit::ir::Block merge = builder.create_block(1);
  const Value condition = builder.constant(1);
  expect(builder.branch(condition, left, {}, right, {}).ok(),
         "invalid-dominance fixture entry must be terminated");

  expect(builder.set_insertion_block(left).ok(),
         "invalid-dominance left block must exist");
  const Value leaked = builder.constant(7);
  expect(builder.jump(merge, {leaked}).ok(), "left block must reach the merge");

  expect(builder.set_insertion_block(right).ok(),
         "invalid-dominance right block must exist");
  const Value invalid = builder.add(leaked, builder.constant(1));
  expect(builder.jump(merge, {invalid}).ok(),
         "right block must reach the merge");

  expect(builder.set_insertion_block(merge).ok(),
         "invalid-dominance merge block must exist");
  expect(builder.set_return(builder.block_parameter(merge, 0)).ok(),
         "invalid-dominance merge must be terminated");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(!unijit::ir::verify(function).ok(),
         "CFG verifier must reject a sibling-block SSA use");
}

void test_control_flow_safepoint() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(2);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0), zero}).ok(),
         "CFG safepoint fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "CFG safepoint loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  const Value sum = builder.block_parameter(loop, 1);
  const Value safepoint = builder.safepoint(314);
  expect(safepoint.valid(), "CFG safepoint must be inserted in the loop");
  const Value next_sum = builder.add(builder.add(sum, remaining), safepoint);
  const Value next_remaining = builder.subtract(remaining, one);
  const Value continue_loop = builder.less_than(zero, next_remaining);
  expect(builder
             .branch(continue_loop, loop, {next_remaining, next_sum}, exit,
                     {next_sum})
             .ok(),
         "CFG safepoint loop must branch to its backedge and exit");

  expect(builder.set_insertion_block(exit).ok(),
         "CFG safepoint exit must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "CFG safepoint exit must return its sum");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(), "CFG safepoint IR must verify");

  const std::array<Word, 1> args = {4};
  unijit::runtime::ExecutionContext context;
  context.request_interrupt();
  const auto interpreted = unijit::ir::ControlFlowInterpreter::evaluate(
      function, args.data(), args.size(), 100, &context);
  expect(!interpreted.ok() &&
             interpreted.status.code() ==
                 unijit::StatusCode::kExecutionInterrupted &&
             interpreted.status.location() == 314,
         "CFG interpreter must exit at the requested loop safepoint");

  auto compilation = Compiler::compile(function);
  expect(compilation.ok(), "CFG safepoint loop must compile");
  if (compilation.ok()) {
    expect(compilation.function->requires_context(),
           "compiled CFG safepoints must require an execution context");
    context.request_interrupt();
    const auto interrupted =
        compilation.function->invoke(args.data(), args.size(), &context);
    expect(!interrupted.ok() &&
               interrupted.status.code() ==
                   unijit::StatusCode::kExecutionInterrupted &&
               interrupted.status.location() == 314 &&
               context.exit_site() == 314,
           "native CFG loop must exit at the requested safepoint");

    context.clear_interrupt();
    const auto completed =
        compilation.function->invoke(args.data(), args.size(), &context);
    expect(completed.ok() && completed.value == 10,
           "native CFG loop must continue when interruption is clear");
    const auto completed_with_local_context =
        compilation.function->invoke(args.data(), args.size());
    expect(completed_with_local_context.ok() &&
               completed_with_local_context.value == 10,
           "CFG invocation must provision a local safepoint context");
    expect(compilation.function->native_entry()(args.data(), nullptr) == 10,
           "null execution contexts must bypass CFG safepoints");
  }
}

void test_assumption_invalidation() {
  unijit::ir::ControlFlowBuilder builder(1);
  const unijit::ir::Block loop = builder.create_block(1);
  const unijit::ir::Block exit = builder.create_block(1);
  const Value zero = builder.constant(0);
  const Value one = builder.constant(1);
  expect(builder.jump(loop, {builder.parameter(0)}).ok(),
         "assumption fixture must enter its loop");

  expect(builder.set_insertion_block(loop).ok(),
         "assumption fixture loop must exist");
  const Value remaining = builder.block_parameter(loop, 0);
  expect(builder.safepoint(401).valid(),
         "assumption fixture must contain a safepoint");
  const Value next = builder.subtract(remaining, one);
  const Value continues = builder.less_than(zero, next);
  expect(builder.branch(continues, loop, {next}, exit, {next}).ok(),
         "assumption fixture must branch to its backedge and exit");
  expect(builder.set_insertion_block(exit).ok(),
         "assumption fixture exit must exist");
  expect(builder.set_return(builder.block_parameter(exit, 0)).ok(),
         "assumption fixture must return its loop state");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();

  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  expect(assumptions.add(assumption, 2718, 73).ok(),
         "a valid runtime assumption must be attachable");
  expect(!assumptions.add(assumption, 2719, 74).ok(),
         "an assumption token must not be registered twice");

  unijit::runtime::DeoptimizationRecord wrong_reason;
  wrong_reason.site = 2718;
  wrong_reason.resume_offset = 73;
  wrong_reason.reason = unijit::runtime::DeoptimizationReason::kGuardFailed;
  unijit::runtime::DeoptimizationTable wrong_metadata;
  expect(wrong_metadata.add(wrong_reason).ok() &&
             !Compiler::compile(function, wrong_metadata, assumptions).ok(),
         "assumption metadata must use the invalidation semantic reason");

  auto compilation = Compiler::compile(function, assumptions);
  expect(compilation.ok(), "CFG compilation must accept valid assumptions");
  if (!compilation.ok()) {
    return;
  }
  const auto* record = compilation.function->deoptimization_record(2718);
  expect(compilation.function->requires_context() &&
             compilation.function->assumptions().size() == 1 &&
             record != nullptr && record->resume_offset == 73 &&
             record->reason ==
                 unijit::runtime::DeoptimizationReason::kAssumptionInvalidated &&
             record->recovery.size() == 1,
         "assumption compilation must publish entry recovery metadata");

  unijit::jit::CodeCache cache;
  const auto publication = cache.publish(
      "assumption-fixture", 1, std::move(compilation.function));
  expect(publication.ok() && publication.handle.assumption_count() == 1,
         "cached code leases must retain their assumption dependencies");
  if (!publication.ok()) {
    return;
  }

  const std::array<Word, 1> arguments = {1000000000};
  unijit::runtime::ExecutionContext context;
  unijit::ir::EvaluationResult outcome;
  std::thread execution([&] {
    outcome = publication.handle.invoke(arguments.data(), arguments.size(),
                                        &context);
  });
  bool observed_active = false;
  for (std::size_t spin = 0; spin < 1000000; ++spin) {
    if (assumption->active_invocations() != 0) {
      observed_active = true;
      break;
    }
    std::this_thread::yield();
  }
  const bool invalidated = assumption->invalidate();
  execution.join();
  expect(observed_active && invalidated && !assumption->valid() &&
             assumption->active_invocations() == 0,
         "assumption invalidation must wait for active code to quiesce");
  expect(!outcome.ok() &&
             outcome.status.code() == unijit::StatusCode::kRuntimeExit &&
             outcome.status.location() == 2718 &&
             context.exit_reason() ==
                 unijit::runtime::ExitReason::kRuntime &&
             context.exit_site() == 2718 && !context.exit_poll_requested(),
         "invalidation must wake native safepoints and report its exit site");

  const auto reconstruction =
      publication.handle.reconstruct_deoptimization(
          2718, arguments.data(), arguments.size(), context);
  const auto* recovered_argument = reconstruction.frame.find(0);
  expect(reconstruction.ok() && reconstruction.frame.resume_offset == 73 &&
             recovered_argument != nullptr &&
             recovered_argument->value == arguments[0],
         "assumption exits must reconstruct the original entry frame");

  auto replacement_assumption =
      std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet replacement_assumptions;
  expect(replacement_assumptions.add(replacement_assumption, 2718, 73).ok(),
         "replacement code must bind a fresh assumption token");
  auto replacement_compilation =
      Compiler::compile(function, replacement_assumptions);
  const auto replacement = cache.publish(
      "assumption-fixture", 1, std::move(replacement_compilation.function));
  const std::array<Word, 1> short_arguments = {4};
  const auto replacement_result = replacement.handle.invoke(
      short_arguments.data(), short_arguments.size());
  expect(replacement_compilation.status.ok() && replacement.ok() &&
             !replacement.reused &&
             replacement.handle.generation() != publication.handle.generation() &&
             replacement_result.ok() && replacement_result.value == 0 &&
             cache.stats().assumption_invalidations == 1,
         "cache publication must replace an assumption-invalid generation");
  expect(replacement_assumption->invalidate() &&
             !cache.find("assumption-fixture", 1).valid() &&
             cache.stats().assumption_invalidations == 2,
         "cache lookup must retire an assumption-invalid generation");

  context.request_interrupt();
  const auto rejected_again = publication.handle.invoke(
      arguments.data(), arguments.size(), &context);
  expect(!rejected_again.ok() &&
             rejected_again.status.code() ==
                 unijit::StatusCode::kRuntimeExit &&
             rejected_again.status.location() == 2718 &&
             context.interrupt_requested(),
         "entry invalidation must preserve an independent sticky interrupt");
  context.clear_interrupt();
  expect(!assumption->invalidate(),
         "repeated assumption invalidation must be idempotent");
  expect(!Compiler::compile(function, assumptions).ok(),
         "compilation must reject an already invalidated assumption");
}

void test_hotness_and_tiered_switching() {
  unijit::jit::CodeCache cache({16, 1024U * 1024U});
  FunctionBuilder baseline_builder(1);
  expect(baseline_builder
             .set_return(baseline_builder.add(baseline_builder.parameter(0),
                                              baseline_builder.constant(1)))
             .ok(),
         "tiered baseline fixture must record its result");
  auto baseline_compilation =
      Compiler::compile(std::move(baseline_builder).build());
  auto baseline_publication = cache.publish(
      "tiered-baseline", 1, std::move(baseline_compilation.function));
  expect(baseline_compilation.status.ok() && baseline_publication.ok(),
         "tiered baseline fixture must compile and publish");

  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  expect(assumptions.add(assumption, 808, 12).ok(),
         "tiered optimized fixture must bind an assumption");
  FunctionBuilder optimized_builder(1);
  expect(optimized_builder
             .set_return(optimized_builder.add(optimized_builder.parameter(0),
                                               optimized_builder.constant(1)))
             .ok(),
         "tiered optimized fixture must record its result");
  auto optimized_compilation = Compiler::compile(
      std::move(optimized_builder).build(), assumptions);
  auto optimized_publication = cache.publish(
      "tiered-optimized", 1, std::move(optimized_compilation.function));
  expect(optimized_compilation.status.ok() && optimized_publication.ok(),
         "tiered optimized fixture must compile and publish");

  unijit::jit::TieredCode tiered({3, 5, 2});
  const std::array<Word, 1> arguments = {41};
  expect(!tiered.snapshot().valid() &&
             !tiered.invoke(arguments.data(), arguments.size()).ok(),
         "tiered invocation must reject a missing baseline");
  expect(tiered.publish_baseline(baseline_publication.handle).ok(),
         "tiered code must publish an assumption-free baseline");
  const auto baseline_snapshot = tiered.snapshot();
  expect(baseline_snapshot.valid() &&
             baseline_snapshot.tier == unijit::jit::CodeTier::kBaseline &&
             baseline_snapshot.generation != 0,
         "tiered baseline publication must expose a stable generation");

  for (std::size_t invocation = 0; invocation < 2; ++invocation) {
    const auto result = tiered.invoke(arguments.data(), arguments.size());
    expect(result.ok() && result.result.value == 42 &&
               result.attempted_tier ==
                   unijit::jit::CodeTier::kBaseline,
           "cold tiered calls must execute the baseline");
  }
  expect(!tiered.try_begin_optimization(),
         "hotness must not trigger before the invocation threshold");
  expect(tiered.invoke(arguments.data(), arguments.size()).ok() &&
             tiered.try_begin_optimization() &&
             !tiered.try_begin_optimization(),
         "one compiler must claim a hot tiered generation");
  expect(tiered
             .publish_optimized(optimized_publication.handle,
                                baseline_snapshot.generation)
             .ok(),
         "the claimed optimized tier must publish over its baseline");
  const auto optimized_snapshot = tiered.snapshot();
  expect(optimized_snapshot.tier == unijit::jit::CodeTier::kOptimized &&
             optimized_snapshot.generation != baseline_snapshot.generation &&
             tiered.stats().hotness.successful_compilations == 1,
         "optimized publication must advance the tiered generation");
  expect(!tiered
              .publish_optimized(optimized_publication.handle,
                                 baseline_snapshot.generation)
              .ok(),
         "late compilation must not overwrite a newer generation");

  const auto optimized_result =
      tiered.invoke(arguments.data(), arguments.size());
  expect(optimized_result.ok() && optimized_result.result.value == 42 &&
             optimized_result.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized,
         "valid assumptions must select the optimized tier");
  expect(assumption->invalidate(),
         "tiered optimized assumption must invalidate once");
  unijit::runtime::ExecutionContext context;
  const auto fallback = tiered.invoke(
      arguments.data(), arguments.size(), &context,
      unijit::jit::DeoptimizationPolicy::kRetryBaseline);
  expect(fallback.ok() && fallback.result.value == 42 &&
             fallback.deoptimized && fallback.retried_baseline &&
             fallback.attempted_tier ==
                 unijit::jit::CodeTier::kOptimized &&
             tiered.snapshot().tier ==
                 unijit::jit::CodeTier::kBaseline,
         "restartable assumption exits must withdraw and retry the baseline");
  const auto retained_exit = optimized_snapshot.handle.invoke(
      arguments.data(), arguments.size(), &context);
  expect(!retained_exit.ok() &&
             retained_exit.status.code() ==
                 unijit::StatusCode::kRuntimeExit,
         "retained optimized snapshots must stay safe after withdrawal");
  expect(!optimized_snapshot.handle.assumptions_valid() &&
             !tiered.publish_optimized(optimized_snapshot.handle).ok(),
         "tiered publication must reject an already stale optimized handle");

  expect(!tiered.try_begin_optimization(),
         "withdrawal must apply a hotness retry delay");
  tiered.record_backedges(1);
  expect(!tiered.try_begin_optimization(),
         "one backedge must not exhaust a two-event retry delay");
  tiered.record_backedges(1);
  expect(tiered.try_begin_optimization() &&
             tiered.report_optimization_failure().ok() &&
             tiered.stats().hotness.failed_compilations == 1,
         "failed tier compilation must rearm profiling without a claim storm");

  FunctionBuilder stable_optimized_builder(1);
  expect(stable_optimized_builder
             .set_return(stable_optimized_builder.add(
                 stable_optimized_builder.parameter(0),
                 stable_optimized_builder.constant(1)))
             .ok(),
         "stable optimized fixture must record its result");
  auto stable_compilation =
      Compiler::compile(std::move(stable_optimized_builder).build());
  auto stable_publication = cache.publish(
      "tiered-stable", 1, std::move(stable_compilation.function));
  expect(stable_compilation.status.ok() && stable_publication.ok(),
         "stable optimized fixture must compile and publish");

  std::atomic<std::size_t> switching_errors{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> readers;
  for (std::size_t index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t invocation = 0; invocation < 2000; ++invocation) {
        const auto result = tiered.invoke(arguments.data(), arguments.size());
        if (!result.ok() || result.result.value != 42) {
          switching_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::size_t iteration = 0; iteration < 200; ++iteration) {
    const auto current = tiered.snapshot();
    if (current.tier == unijit::jit::CodeTier::kBaseline) {
      if (!tiered
               .publish_optimized(stable_publication.handle,
                                  current.generation)
               .ok()) {
        switching_errors.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (!tiered.withdraw_optimized(current.generation)) {
      switching_errors.fetch_add(1, std::memory_order_relaxed);
    }
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  const auto tiered_stats = tiered.stats();
  expect(switching_errors.load(std::memory_order_relaxed) == 0 &&
             tiered_stats.promotions > 0 && tiered_stats.withdrawals > 0 &&
             tiered_stats.assumption_deoptimizations == 1 &&
             tiered_stats.baseline_retries == 1,
         "concurrent tier switching must retain safe immutable snapshots");
}

void test_control_flow_execution_budget() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block loop = builder.create_block(0);
  expect(builder.jump(loop, {}).ok(), "entry must reach the infinite loop");
  expect(builder.set_insertion_block(loop).ok(),
         "infinite-loop block must exist");
  expect(builder.jump(loop, {}).ok(), "loop must jump to itself");
  const unijit::ir::ControlFlowFunction function = std::move(builder).build();
  expect(unijit::ir::verify(function).ok(),
         "self-loop must be a valid control-flow graph");
  const auto result =
      unijit::ir::ControlFlowInterpreter::evaluate(function, nullptr, 0, 10);
  expect(!result.ok() &&
             result.status.code() == unijit::StatusCode::kResourceExhausted,
         "CFG interpreter must stop when its execution budget is exhausted");
}

void test_control_flow_builder_rejects_edge_arity() {
  unijit::ir::ControlFlowBuilder builder(0);
  const unijit::ir::Block target = builder.create_block(1);
  expect(!builder.jump(target, {}).ok(),
         "builder must reject an edge with missing block arguments");
}

}  // namespace

int main() {
  test_code_cache_lifecycle();
  test_code_cache_concurrency();
  test_verifier_rejects_forward_reference();
  test_constant_native_function();
  test_execution_context_lifecycle();
  test_safepoint_ir_and_interpreter();
  test_differential_arithmetic();
  test_float64_ir_and_interpreter();
  test_float64_division();
  test_float64_nonzero_guard();
  test_deoptimization_reconstruction();
  test_constant_float64_nonzero_guard_elimination();
  test_verifier_rejects_mixed_arithmetic();
  test_float64_spill_path();
  test_float64_preserves_host_abi();
  test_runtime_helper_call();
  test_effectful_dead_runtime_call();
  test_float64_runtime_helper_call();
  test_verifier_rejects_null_runtime_helper();
  test_spill_path();
  test_argument_validation();
  test_optimization_pipeline();
  test_float64_constant_folding();
  test_control_flow_counted_loop();
  test_control_flow_float64_loop();
  test_control_flow_rejects_mixed_edge_types();
  test_control_flow_float64_comparisons();
  test_control_flow_merge();
  test_control_flow_parallel_edge_copy();
  test_control_flow_preserves_nonlocal_merge_state();
  test_control_flow_rejects_non_dominating_value();
  test_control_flow_safepoint();
  test_assumption_invalidation();
  test_hotness_and_tiered_switching();
  test_control_flow_execution_budget();
  test_control_flow_builder_rejects_edge_arity();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all UniJIT tests passed\n";
  return EXIT_SUCCESS;
}
