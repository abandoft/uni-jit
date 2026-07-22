#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"
#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"
#include "unijit/runtime/deoptimization.h"
#include "unijit/runtime/execution_context.h"
#include "unijit/runtime/materialization.h"

namespace {

struct PackageMaterializer final {
  unijit::ir::Word primitive{0};
  bool began{false};
  std::size_t frame_primitives{0};
  std::size_t frame_objects{0};
  unijit::runtime::ObjectHandle installed_object{0};
  std::size_t commits{0};
  std::size_t rollbacks{0};
};

unijit::Status begin_objects(
    void* opaque, unijit::runtime::DeoptimizationReason reason,
    std::size_t site, std::size_t resume_offset, std::size_t object_count,
    std::size_t frame_value_count) noexcept {
  if (reason != unijit::runtime::DeoptimizationReason::kDivisionByZero ||
      site != 17 || resume_offset != 9 || object_count != 1 ||
      frame_value_count != 3) {
    return {unijit::StatusCode::kInvalidArgument,
            "unexpected package materialization transaction"};
  }
  static_cast<PackageMaterializer*>(opaque)->began = true;
  return unijit::Status::ok_status();
}

unijit::Status allocate_object(
    void*, std::size_t id, std::uint64_t kind, std::size_t fields,
    unijit::runtime::ObjectHandle* handle) noexcept {
  if (id != 5 || kind != 6 || fields != 1 || handle == nullptr) {
    return {unijit::StatusCode::kInvalidArgument,
            "unexpected package object recipe"};
  }
  *handle = 77;
  return unijit::Status::ok_status();
}

unijit::Status store_primitive(
    void* opaque, unijit::runtime::ObjectHandle object, std::size_t field,
    unijit::ir::ValueType type, unijit::ir::Word value) noexcept {
  if (object != 77 || field != 0 ||
      type != unijit::ir::ValueType::kFloat64) {
    return {unijit::StatusCode::kInvalidArgument,
            "unexpected package primitive field"};
  }
  static_cast<PackageMaterializer*>(opaque)->primitive = value;
  return unijit::Status::ok_status();
}

unijit::Status store_object(void*, unijit::runtime::ObjectHandle,
                            std::size_t,
                            unijit::runtime::ObjectHandle) noexcept {
  return unijit::Status::ok_status();
}

unijit::Status store_frame_primitive(
    void* opaque, std::size_t slot, unijit::ir::ValueType type,
    unijit::ir::Word) noexcept {
  if (slot > 1 || type != unijit::ir::ValueType::kFloat64) {
    return {unijit::StatusCode::kInvalidArgument,
            "unexpected package primitive frame slot"};
  }
  ++static_cast<PackageMaterializer*>(opaque)->frame_primitives;
  return unijit::Status::ok_status();
}

unijit::Status store_frame_object(
    void* opaque, std::size_t slot,
    unijit::runtime::ObjectHandle value) noexcept {
  if (slot != 10 || value != 77) {
    return {unijit::StatusCode::kInvalidArgument,
            "unexpected package object frame slot"};
  }
  auto* state = static_cast<PackageMaterializer*>(opaque);
  ++state->frame_objects;
  state->installed_object = value;
  return unijit::Status::ok_status();
}

unijit::Status commit_objects(void* opaque) noexcept {
  ++static_cast<PackageMaterializer*>(opaque)->commits;
  return unijit::Status::ok_status();
}

void rollback_objects(void* opaque) noexcept {
  ++static_cast<PackageMaterializer*>(opaque)->rollbacks;
}

std::size_t package_cfg_call_count = 0;

unijit::ir::Word package_cfg_helper(const unijit::ir::Word* arguments,
                                    std::size_t count) {
  ++package_cfg_call_count;
  if (count != 2) {
    return unijit::ir::pack_float64(0.0);
  }
  return unijit::ir::pack_float64(
      unijit::ir::unpack_float64(arguments[0]) +
      unijit::ir::unpack_float64(arguments[1]));
}

}  // namespace

int main() {
  unijit::ir::FunctionBuilder builder(2);
  const auto sum = builder.add(builder.parameter(0), builder.parameter(1));
  if (!builder.set_return(sum).ok()) {
    return 1;
  }
  const auto function = std::move(builder).build();
  auto compilation = unijit::jit::Compiler::compile(
      function,
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kBaseline});
  if (!compilation.ok()) {
    return 2;
  }
  unijit::jit::CodeCache cache({1, 64U * 1024U});
  auto publication = cache.publish("package-consumer-sum", 1,
                                   std::move(compilation.function));
  if (!publication.ok() || !publication.cached || publication.reused) {
    return 3;
  }
  auto cached = cache.find("package-consumer-sum", 1);
  if (!cached || cached.generation() != publication.handle.generation()) {
    return 4;
  }
  const std::array<unijit::ir::Word, 2> arguments = {20, 22};
  const auto result = cached.invoke(arguments.data(), arguments.size());
  if (!result.ok() || result.value != 42) {
    return 5;
  }
  unijit::ir::FunctionBuilder comparison_builder(
      std::vector<unijit::ir::ValueType>(
          2, unijit::ir::ValueType::kFloat64));
  const auto comparison_equal = comparison_builder.float64_equal(
      comparison_builder.parameter(0), comparison_builder.parameter(1));
  const auto comparison_not_equal = comparison_builder.float64_not_equal(
      comparison_builder.parameter(0), comparison_builder.parameter(1));
  if (!comparison_builder
           .set_return(comparison_builder.add(
               comparison_builder.multiply(comparison_equal,
                                           comparison_builder.constant(10)),
               comparison_not_equal))
           .ok()) {
    return 42;
  }
  auto comparison_compilation = unijit::jit::Compiler::compile(
      std::move(comparison_builder).build());
  const std::array<unijit::ir::Word, 2> signed_zeroes = {
      unijit::ir::pack_float64(0.0), unijit::ir::pack_float64(-0.0)};
  const auto comparison_result =
      comparison_compilation.ok()
          ? comparison_compilation.function->invoke(signed_zeroes.data(),
                                                     signed_zeroes.size())
          : unijit::ir::EvaluationResult{};
  if (!comparison_compilation.ok() || !comparison_result.ok() ||
      comparison_result.value != 10) {
    return 43;
  }
  unijit::ir::FunctionBuilder negate_builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  if (!negate_builder
           .set_return(negate_builder.float64_negate(
               negate_builder.parameter(0)))
           .ok()) {
    return 46;
  }
  auto negate_compilation = unijit::jit::Compiler::compile(
      std::move(negate_builder).build());
  const std::uint64_t positive_nan = UINT64_C(0x7ff8000000001234);
  const unijit::ir::Word negate_argument =
      static_cast<unijit::ir::Word>(positive_nan);
  const auto negate_result =
      negate_compilation.ok()
          ? negate_compilation.function->invoke(&negate_argument, 1)
          : unijit::ir::EvaluationResult{};
  if (!negate_compilation.ok() || !negate_result.ok() ||
      static_cast<std::uint64_t>(negate_result.value) !=
          (positive_nan ^ (UINT64_C(1) << 63U))) {
    return 47;
  }
  unijit::ir::FunctionBuilder word_unary_builder(1);
  if (!word_unary_builder
           .set_return(word_unary_builder.add(
               word_unary_builder.negate(word_unary_builder.parameter(0)),
               word_unary_builder.bitwise_not(
                   word_unary_builder.parameter(0))))
           .ok()) {
    return 50;
  }
  auto word_unary_compilation = unijit::jit::Compiler::compile(
      std::move(word_unary_builder).build());
  const unijit::ir::Word word_unary_argument =
      std::numeric_limits<unijit::ir::Word>::min();
  const auto word_unary_result =
      word_unary_compilation.ok()
          ? word_unary_compilation.function->invoke(&word_unary_argument, 1)
          : unijit::ir::EvaluationResult{};
  if (!word_unary_compilation.ok() || !word_unary_result.ok() ||
      static_cast<std::uint64_t>(word_unary_result.value) !=
          UINT64_C(0xffffffffffffffff)) {
    return 51;
  }
  unijit::ir::FunctionBuilder bitwise_builder(2);
  const auto bitwise_and = bitwise_builder.bitwise_and(
      bitwise_builder.parameter(0), bitwise_builder.parameter(1));
  const auto bitwise_xor = bitwise_builder.bitwise_xor(
      bitwise_builder.parameter(0), bitwise_builder.parameter(1));
  if (!bitwise_builder
           .set_return(bitwise_builder.bitwise_or(bitwise_and, bitwise_xor))
           .ok()) {
    return 54;
  }
  auto bitwise_compilation = unijit::jit::Compiler::compile(
      std::move(bitwise_builder).build());
  const std::array<unijit::ir::Word, 2> bitwise_arguments = {
      static_cast<unijit::ir::Word>(UINT64_C(0x5555555555555555)),
      static_cast<unijit::ir::Word>(UINT64_C(0xaaaaaaaaaaaaaaaa))};
  const auto bitwise_result =
      bitwise_compilation.ok()
          ? bitwise_compilation.function->invoke(bitwise_arguments.data(),
                                                  bitwise_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!bitwise_compilation.ok() || !bitwise_result.ok() ||
      static_cast<std::uint64_t>(bitwise_result.value) !=
          UINT64_C(0xffffffffffffffff)) {
    return 55;
  }
  unijit::ir::FunctionBuilder shift_builder(2);
  if (!shift_builder
           .set_return(shift_builder.shift_left(shift_builder.parameter(0),
                                                shift_builder.parameter(1)))
           .ok()) {
    return 58;
  }
  auto shift_compilation = unijit::jit::Compiler::compile(
      std::move(shift_builder).build());
  const std::array<unijit::ir::Word, 2> shift_arguments = {-1, -63};
  const auto shift_result =
      shift_compilation.ok()
          ? shift_compilation.function->invoke(shift_arguments.data(),
                                                shift_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!shift_compilation.ok() || !shift_result.ok() ||
      shift_result.value != 1) {
    return 59;
  }
  unijit::ir::FunctionBuilder floor_builder(2);
  const auto floor_quotient = floor_builder.floor_divide(
      floor_builder.parameter(0), floor_builder.parameter(1));
  const auto floor_remainder = floor_builder.floor_modulo(
      floor_builder.parameter(0), floor_builder.parameter(1));
  if (!floor_builder
           .set_return(floor_builder.add(
               floor_builder.multiply(floor_quotient,
                                      floor_builder.constant(10)),
               floor_remainder))
           .ok()) {
    return 62;
  }
  auto floor_compilation = unijit::jit::Compiler::compile(
      std::move(floor_builder).build());
  const std::array<unijit::ir::Word, 2> floor_arguments = {-17, 5};
  const auto floor_result =
      floor_compilation.ok()
          ? floor_compilation.function->invoke(floor_arguments.data(),
                                                floor_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!floor_compilation.ok() || !floor_result.ok() ||
      floor_result.value != -37) {
    return 63;
  }
  unijit::ir::FunctionBuilder word_comparison_builder(2);
  const auto word_comparison_lhs = word_comparison_builder.parameter(0);
  const auto word_comparison_rhs = word_comparison_builder.parameter(1);
  if (!word_comparison_builder
           .set_return(word_comparison_builder.add(
               word_comparison_builder.multiply(
                   word_comparison_builder.less_than(word_comparison_lhs,
                                                     word_comparison_rhs),
                   word_comparison_builder.constant(1000)),
               word_comparison_builder.add(
                   word_comparison_builder.multiply(
                       word_comparison_builder.less_equal(word_comparison_lhs,
                                                          word_comparison_rhs),
                       word_comparison_builder.constant(100)),
                   word_comparison_builder.add(
                       word_comparison_builder.multiply(
                           word_comparison_builder.equal(word_comparison_lhs,
                                                         word_comparison_rhs),
                           word_comparison_builder.constant(10)),
                       word_comparison_builder.not_equal(
                           word_comparison_lhs, word_comparison_rhs)))))
           .ok()) {
    return 66;
  }
  auto word_comparison_compilation = unijit::jit::Compiler::compile(
      std::move(word_comparison_builder).build());
  const auto word_comparison_result =
      word_comparison_compilation.ok()
          ? word_comparison_compilation.function->invoke(
                floor_arguments.data(), floor_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!word_comparison_compilation.ok() || !word_comparison_result.ok() ||
      word_comparison_result.value != 1101) {
    return 67;
  }
  unijit::jit::CompilationLimits package_limits;
  package_limits.maximum_ir_nodes = 2;
  const auto limited_compilation = unijit::jit::Compiler::compile(
      function,
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kOptimized, package_limits});
  if (limited_compilation.ok() ||
      limited_compilation.status.code() !=
          unijit::StatusCode::kResourceExhausted) {
    return 33;
  }

  unijit::ir::ControlFlowBuilder cfg_builder(
      {unijit::ir::ValueType::kFloat64,
       unijit::ir::ValueType::kFloat64});
  const auto cfg_bias = cfg_builder.float64_add(
      cfg_builder.float64_constant(0.25),
      cfg_builder.float64_constant(0.25));
  const auto cfg_live = cfg_builder.float64_add(
      cfg_builder.parameter(0), cfg_bias);
  const auto cfg_called = cfg_builder.call(
      package_cfg_helper, {cfg_live, cfg_builder.parameter(1)},
      unijit::ir::ValueType::kFloat64);
  if (!cfg_builder
           .set_return(cfg_builder.float64_add(cfg_live, cfg_called))
           .ok()) {
    return 34;
  }
  const auto cfg_function = std::move(cfg_builder).build();
  if (!unijit::ir::verify(cfg_function).ok()) {
    return 35;
  }
  auto cfg_baseline = unijit::jit::Compiler::compile(
      cfg_function,
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kBaseline});
  auto cfg_compilation = unijit::jit::Compiler::compile(
      cfg_function,
      unijit::jit::CompilationOptions{
          unijit::jit::OptimizationLevel::kOptimized});
  if (!cfg_baseline.ok() || !cfg_compilation.ok() ||
      cfg_baseline.function->stats().optimized_ir_nodes !=
          cfg_baseline.function->stats().input_ir_nodes ||
      cfg_compilation.function->stats().optimized_ir_nodes >=
          cfg_baseline.function->stats().optimized_ir_nodes) {
    return 36;
  }
  const std::array<unijit::ir::Word, 2> cfg_arguments = {
      unijit::ir::pack_float64(2.5),
      unijit::ir::pack_float64(4.0)};
  package_cfg_call_count = 0;
  const auto cfg_result = cfg_compilation.function->invoke(
      cfg_arguments.data(), cfg_arguments.size());
  if (!cfg_result.ok() ||
      cfg_result.value != unijit::ir::pack_float64(10.0) ||
      package_cfg_call_count != 1) {
    return 37;
  }
  unijit::ir::ControlFlowBuilder cfg_comparison_builder(
      {unijit::ir::ValueType::kFloat64,
       unijit::ir::ValueType::kFloat64});
  if (!cfg_comparison_builder
           .set_return(cfg_comparison_builder.float64_not_equal(
               cfg_comparison_builder.parameter(0),
               cfg_comparison_builder.parameter(1)))
           .ok()) {
    return 44;
  }
  auto cfg_comparison = unijit::jit::Compiler::compile(
      std::move(cfg_comparison_builder).build());
  const std::array<unijit::ir::Word, 2> unordered = {
      unijit::ir::pack_float64(std::numeric_limits<double>::quiet_NaN()),
      unijit::ir::pack_float64(0.0)};
  const auto cfg_comparison_result =
      cfg_comparison.ok()
          ? cfg_comparison.function->invoke(unordered.data(), unordered.size())
          : unijit::ir::EvaluationResult{};
  if (!cfg_comparison.ok() || !cfg_comparison_result.ok() ||
      cfg_comparison_result.value != 1) {
    return 45;
  }
  unijit::ir::ControlFlowBuilder cfg_negate_builder(
      {unijit::ir::ValueType::kFloat64});
  if (!cfg_negate_builder
           .set_return(cfg_negate_builder.float64_negate(
               cfg_negate_builder.parameter(0)))
           .ok()) {
    return 48;
  }
  auto cfg_negate = unijit::jit::Compiler::compile(
      std::move(cfg_negate_builder).build());
  const unijit::ir::Word positive_zero = unijit::ir::pack_float64(0.0);
  const auto cfg_negate_result =
      cfg_negate.ok()
          ? cfg_negate.function->invoke(&positive_zero, 1)
          : unijit::ir::EvaluationResult{};
  if (!cfg_negate.ok() || !cfg_negate_result.ok() ||
      cfg_negate_result.value != unijit::ir::pack_float64(-0.0)) {
    return 49;
  }
  unijit::ir::ControlFlowBuilder cfg_word_unary_builder(1);
  if (!cfg_word_unary_builder
           .set_return(cfg_word_unary_builder.bitwise_not(
               cfg_word_unary_builder.negate(
                   cfg_word_unary_builder.parameter(0))))
           .ok()) {
    return 52;
  }
  auto cfg_word_unary = unijit::jit::Compiler::compile(
      std::move(cfg_word_unary_builder).build());
  const unijit::ir::Word cfg_word_unary_argument = 0;
  const auto cfg_word_unary_result =
      cfg_word_unary.ok()
          ? cfg_word_unary.function->invoke(&cfg_word_unary_argument, 1)
          : unijit::ir::EvaluationResult{};
  if (!cfg_word_unary.ok() || !cfg_word_unary_result.ok() ||
      cfg_word_unary_result.value != -1) {
    return 53;
  }
  unijit::ir::ControlFlowBuilder cfg_bitwise_builder(2);
  const auto cfg_or = cfg_bitwise_builder.bitwise_or(
      cfg_bitwise_builder.parameter(0), cfg_bitwise_builder.parameter(1));
  const auto cfg_and = cfg_bitwise_builder.bitwise_and(
      cfg_bitwise_builder.parameter(0), cfg_bitwise_builder.parameter(1));
  if (!cfg_bitwise_builder
           .set_return(cfg_bitwise_builder.bitwise_xor(cfg_or, cfg_and))
           .ok()) {
    return 56;
  }
  auto cfg_bitwise = unijit::jit::Compiler::compile(
      std::move(cfg_bitwise_builder).build());
  const std::array<unijit::ir::Word, 2> cfg_bitwise_arguments = {0x55, 0x0f};
  const auto cfg_bitwise_result =
      cfg_bitwise.ok()
          ? cfg_bitwise.function->invoke(cfg_bitwise_arguments.data(),
                                         cfg_bitwise_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!cfg_bitwise.ok() || !cfg_bitwise_result.ok() ||
      cfg_bitwise_result.value != 0x5a) {
    return 57;
  }
  unijit::ir::ControlFlowBuilder cfg_shift_builder(2);
  if (!cfg_shift_builder
           .set_return(cfg_shift_builder.shift_left(
               cfg_shift_builder.parameter(0),
               cfg_shift_builder.parameter(1)))
           .ok()) {
    return 60;
  }
  auto cfg_shift = unijit::jit::Compiler::compile(
      std::move(cfg_shift_builder).build());
  const std::array<unijit::ir::Word, 2> cfg_shift_arguments = {1, 63};
  const auto cfg_shift_result =
      cfg_shift.ok()
          ? cfg_shift.function->invoke(cfg_shift_arguments.data(),
                                       cfg_shift_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!cfg_shift.ok() || !cfg_shift_result.ok() ||
      static_cast<std::uint64_t>(cfg_shift_result.value) !=
          UINT64_C(0x8000000000000000)) {
    return 61;
  }
  unijit::ir::ControlFlowBuilder cfg_floor_builder(2);
  if (!cfg_floor_builder
           .set_return(cfg_floor_builder.add(
               cfg_floor_builder.floor_divide(
                   cfg_floor_builder.parameter(0),
                   cfg_floor_builder.parameter(1)),
               cfg_floor_builder.floor_modulo(
                   cfg_floor_builder.parameter(0),
                   cfg_floor_builder.parameter(1))))
           .ok()) {
    return 64;
  }
  auto cfg_floor = unijit::jit::Compiler::compile(
      std::move(cfg_floor_builder).build());
  const auto cfg_floor_result =
      cfg_floor.ok()
          ? cfg_floor.function->invoke(floor_arguments.data(),
                                       floor_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!cfg_floor.ok() || !cfg_floor_result.ok() ||
      cfg_floor_result.value != -1) {
    return 65;
  }
  unijit::ir::ControlFlowBuilder cfg_word_comparison_builder(2);
  if (!cfg_word_comparison_builder
           .set_return(cfg_word_comparison_builder.add(
               cfg_word_comparison_builder.equal(
                   cfg_word_comparison_builder.parameter(0),
                   cfg_word_comparison_builder.parameter(1)),
               cfg_word_comparison_builder.not_equal(
                   cfg_word_comparison_builder.parameter(0),
                   cfg_word_comparison_builder.parameter(1))))
           .ok()) {
    return 68;
  }
  auto cfg_word_comparison = unijit::jit::Compiler::compile(
      std::move(cfg_word_comparison_builder).build());
  const auto cfg_word_comparison_result =
      cfg_word_comparison.ok()
          ? cfg_word_comparison.function->invoke(floor_arguments.data(),
                                                 floor_arguments.size())
          : unijit::ir::EvaluationResult{};
  if (!cfg_word_comparison.ok() || !cfg_word_comparison_result.ok() ||
      cfg_word_comparison_result.value != 1) {
    return 69;
  }
  unijit::ir::FunctionBuilder safepoint_builder(0);
  if (!safepoint_builder.safepoint(23).valid() ||
      !safepoint_builder
           .set_return(safepoint_builder.constant(42))
           .ok()) {
    return 38;
  }
  const auto safepoint_function = std::move(safepoint_builder).build();
  auto safepoint_compilation = unijit::jit::Compiler::compile(
      safepoint_function);
  if (!safepoint_compilation.ok()) {
    return 39;
  }
  unijit::runtime::ExecutionContext safepoint_context;
  const auto safepoint_result = safepoint_compilation.function->invoke(
      nullptr, 0, &safepoint_context);
  if (!safepoint_result.ok() || safepoint_result.value != 42 ||
      safepoint_context.safepoint_polls() != 1) {
    return 40;
  }
  unijit::jit::CompilationOptions unmeasured_options;
  unmeasured_options.measure_safepoint_polls = false;
  auto unmeasured_safepoint = unijit::jit::Compiler::compile(
      safepoint_function, unmeasured_options);
  if (!unmeasured_safepoint.ok()) {
    return 41;
  }
  const auto unmeasured_result = unmeasured_safepoint.function->invoke(
      nullptr, 0, &safepoint_context);
  if (!unmeasured_result.ok() ||
      unmeasured_result.value != 42 ||
      safepoint_context.safepoint_polls() != 0) {
    return 41;
  }
  unijit::runtime::OsrFrame osr_frame(31, 7);
  unijit::runtime::OsrEntryPlan osr_plan(31, 7);
  if (!osr_frame.add(8, unijit::ir::ValueType::kWord, 20).ok() ||
      !osr_frame.add(3, unijit::ir::ValueType::kWord, 22).ok() ||
      !osr_plan.add_argument(8, unijit::ir::ValueType::kWord).ok() ||
      !osr_plan.add_argument(3, unijit::ir::ValueType::kWord).ok()) {
    return 29;
  }
  const auto osr = cached.enter_osr(osr_frame, osr_plan);
  if (!osr.ok() || osr.result.value != 42 || osr.arguments.count != 2 ||
      osr.arguments.values[0] != 20 || osr.arguments.values[1] != 22) {
    return 30;
  }
  if (!cache.invalidate("package-consumer-sum", 1) ||
      cache.find("package-consumer-sum", 1)) {
    return 6;
  }
  const auto leased_result =
      publication.handle.invoke(arguments.data(), arguments.size());
  if (!leased_result.ok() || leased_result.value != 42) {
    return 7;
  }

  unijit::ir::FunctionBuilder guarded_builder(
      std::vector<unijit::ir::ValueType>(1,
                                         unijit::ir::ValueType::kFloat64));
  const auto divisor = guarded_builder.parameter(0);
  const auto snapshot = guarded_builder.float64_add(
      divisor, guarded_builder.float64_constant(2.5));
  if (!guarded_builder.guard_float64_nonzero(divisor, 17).valid() ||
      !guarded_builder.set_return(divisor).ok()) {
    return 8;
  }
  unijit::runtime::DeoptimizationRecord record;
  record.site = 17;
  record.resume_offset = 9;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {
      unijit::runtime::RecoveryOperation::argument(
          0, unijit::ir::ValueType::kFloat64, 0),
      unijit::runtime::RecoveryOperation::captured_value(
          1, unijit::ir::ValueType::kFloat64, snapshot)};
  unijit::runtime::DeoptimizationTable metadata;
  if (!metadata.add(record).ok()) {
    return 9;
  }
  auto guarded_compilation = unijit::jit::Compiler::compile(
      std::move(guarded_builder).build(), metadata);
  if (!guarded_compilation.ok()) {
    return 10;
  }
  auto guarded_publication = cache.publish(
      "package-consumer-guard", 2, std::move(guarded_compilation.function));
  if (!guarded_publication.ok() ||
      guarded_publication.handle.deoptimization_record(17) == nullptr ||
      guarded_publication.handle.stack_maps() == nullptr ||
      guarded_publication.handle.stack_map(17) == nullptr ||
      guarded_publication.handle.compilation_stats() == nullptr ||
      guarded_publication.handle.compilation_stats()->stack_map_count != 1) {
    return 11;
  }
  const std::array<unijit::ir::Word, 1> zero = {
      unijit::ir::pack_float64(-0.0)};
  unijit::runtime::ExecutionContext context;
  const auto guarded_result = guarded_publication.handle.invoke(
      zero.data(), zero.size(), &context);
  const auto reconstructed =
      guarded_publication.handle.reconstruct_deoptimization(
          17, zero.data(), zero.size(), context);
  const auto captured =
      guarded_publication.handle.reconstruct_stack_map(context);
  const auto *recovered = reconstructed.frame.find(0);
  const auto *recovered_snapshot = reconstructed.frame.find(1);
  if (guarded_result.ok() || !reconstructed.ok() || recovered == nullptr ||
      recovered->value != zero[0] || recovered_snapshot == nullptr ||
      recovered_snapshot->value != unijit::ir::pack_float64(2.5) ||
      !captured.ok() || captured.capture.values.size() != 2) {
    return 12;
  }
  unijit::runtime::MaterializationPlan object_plan(17, 9);
  if (!object_plan
           .add({5,
                 10,
                 6,
                 {unijit::runtime::MaterializationInput::recovered(
                     1, unijit::ir::ValueType::kFloat64)}})
           .ok()) {
    return 27;
  }
  PackageMaterializer object_state;
  const auto materialized =
      guarded_publication.handle.materialize_deoptimization(
          17, zero.data(), zero.size(), context, object_plan,
          {&object_state, begin_objects, allocate_object, store_primitive,
           store_object, store_frame_primitive, store_frame_object,
           commit_objects, rollback_objects});
  const auto *materialized_object = materialized.frame.find(10);
  if (!materialized.ok() || materialized_object == nullptr ||
      materialized_object->kind !=
          unijit::runtime::MaterializedValueKind::kObject ||
      materialized_object->object != 77 ||
      object_state.primitive != unijit::ir::pack_float64(2.5) ||
      !object_state.began || object_state.frame_primitives != 2 ||
      object_state.frame_objects != 1 || object_state.installed_object != 77 ||
      object_state.commits != 1 || object_state.rollbacks != 0) {
    return 28;
  }

  unijit::ir::FunctionBuilder assumed_builder(1);
  if (!assumed_builder
           .set_return(assumed_builder.add(assumed_builder.parameter(0),
                                           assumed_builder.constant(1)))
           .ok()) {
    return 13;
  }
  auto assumption = std::make_shared<unijit::runtime::Assumption>();
  unijit::runtime::AssumptionSet assumptions;
  if (!assumptions.add(assumption, 19, 11).ok()) {
    return 14;
  }
  auto assumed_compilation = unijit::jit::Compiler::compile(
      std::move(assumed_builder).build(), assumptions);
  if (!assumed_compilation.ok()) {
    return 15;
  }
  auto assumed_publication = cache.publish(
      "package-consumer-assumption", 3,
      std::move(assumed_compilation.function));
  const std::array<unijit::ir::Word, 1> assumed_arguments = {41};
  const auto assumed_valid = assumed_publication.handle.invoke(
      assumed_arguments.data(), assumed_arguments.size());
  if (!assumed_publication.ok() ||
      assumed_publication.handle.assumption_count() != 1 ||
      !assumed_valid.ok() || assumed_valid.value != 42) {
    return 16;
  }
  if (!assumption->invalidate()) {
    return 17;
  }
  unijit::runtime::ExecutionContext invalidation_context;
  const auto assumed_invalid = assumed_publication.handle.invoke(
      assumed_arguments.data(), assumed_arguments.size(),
      &invalidation_context);
  const auto assumed_frame =
      assumed_publication.handle.reconstruct_deoptimization(
          19, assumed_arguments.data(), assumed_arguments.size(),
          invalidation_context);
  if (assumed_invalid.ok() || !assumed_frame.ok() ||
      assumed_frame.frame.find(0) == nullptr ||
      assumed_frame.frame.find(0)->value != 41) {
    return 18;
  }

  unijit::jit::TieredCode tiered({1, 1, 1});
  if (!tiered.publish_baseline(guarded_publication.handle).ok()) {
    return 19;
  }
  unijit::runtime::OsrFrame tiered_osr_frame(32, 8);
  unijit::runtime::OsrEntryPlan tiered_osr_plan(32, 8);
  if (!tiered_osr_frame
           .add(4, unijit::ir::ValueType::kFloat64,
                unijit::ir::pack_float64(2.0))
           .ok() ||
      !tiered_osr_plan
           .add_argument(4, unijit::ir::ValueType::kFloat64)
           .ok()) {
    return 31;
  }
  const auto tiered_osr =
      tiered.enter_osr(tiered_osr_frame, tiered_osr_plan);
  if (!tiered_osr.ok() ||
      unijit::ir::unpack_float64(tiered_osr.entry.result.value) != 2.0 ||
      tiered.stats().osr_entries != 1) {
    return 32;
  }
  const auto baseline_snapshot = tiered.snapshot();
  const std::array<unijit::ir::Word, 1> tiered_arguments = {
      unijit::ir::pack_float64(2.0)};
  const auto baseline_result =
      tiered.invoke(tiered_arguments.data(), tiered_arguments.size());
  if (!baseline_result.ok() || !tiered.try_begin_optimization()) {
    return 20;
  }

  unijit::ir::FunctionBuilder optimized_builder(
      std::vector<unijit::ir::ValueType>{unijit::ir::ValueType::kFloat64});
  if (!optimized_builder.set_return(optimized_builder.parameter(0)).ok()) {
    return 21;
  }
  auto optimized_compilation = unijit::jit::Compiler::compile(
      std::move(optimized_builder).build());
  auto optimized_publication = cache.publish(
      "package-consumer-tiered", 4,
      std::move(optimized_compilation.function));
  if (!optimized_compilation.status.ok() || !optimized_publication.ok() ||
      !tiered
           .publish_optimized(optimized_publication.handle,
                              baseline_snapshot.generation)
           .ok()) {
    return 22;
  }
  const auto optimized_result =
      tiered.invoke(tiered_arguments.data(), tiered_arguments.size());
  if (!optimized_result.ok() ||
      optimized_result.attempted_handle.generation() !=
          optimized_publication.handle.generation() ||
      optimized_result.attempted_tier !=
          unijit::jit::CodeTier::kOptimized ||
      optimized_result.result.value != tiered_arguments[0]) {
    return 23;
  }

  auto scheduler_creation = unijit::jit::CompilationScheduler::create(
      {1, 2, 1024});
  if (!scheduler_creation.ok()) {
    return 24;
  }
  auto scheduled = scheduler_creation.scheduler->try_submit(
      {"package-consumer", 1, 128,
       unijit::jit::CompilationPriority::kNormal,
       [](const unijit::jit::CompilationCancellation& cancellation) {
         return cancellation.stop_requested()
                    ? unijit::Status{unijit::StatusCode::kCancelled,
                                     "package task cancelled"}
                    : unijit::Status::ok_status();
       }});
  if (!scheduled.ok() || !scheduled.ticket.wait().ok()) {
    return 25;
  }
  scheduler_creation.scheduler->wait_idle();
  const auto scheduler_stats = scheduler_creation.scheduler->stats();
  if (scheduler_stats.submitted != 1 || scheduler_stats.succeeded != 1 ||
      !scheduler_creation.scheduler->shutdown().ok()) {
    return 26;
  }

  unijit::ir::FunctionBuilder vector_builder(0);
  const auto vector_lhs = vector_builder.vector_splat(
      unijit::ir::ValueType::kI8x16, vector_builder.constant(250));
  const auto vector_rhs = vector_builder.vector_splat(
      unijit::ir::ValueType::kI8x16, vector_builder.constant(10));
  const auto vector_sum = vector_builder.vector_binary(
      unijit::ir::VectorBinaryOperation::kAdd, vector_lhs, vector_rhs);
  if (!vector_builder
           .set_return(vector_builder.vector_extract_lane(vector_sum, 3))
           .ok()) {
    return 70;
  }
  auto vector_compilation = unijit::jit::Compiler::compile(
      std::move(vector_builder).build());
  const auto vector_result =
      vector_compilation.ok()
          ? vector_compilation.function->invoke(nullptr, 0)
          : unijit::ir::EvaluationResult{};
  if (!vector_compilation.ok() || !vector_result.ok() ||
      vector_result.value != 4) {
    return 71;
  }

  unijit::ir::Vector128 vector_memory_bits;
  for (std::size_t index = 0; index < vector_memory_bits.bytes.size();
       ++index) {
    vector_memory_bits.bytes[index] = static_cast<std::uint8_t>(index + 1U);
  }
  unijit::ir::MemoryAccessDescriptor vector_memory_access;
  vector_memory_access.width = unijit::ir::MemoryWidth::k128;
  vector_memory_access.alignment = 1;
  vector_memory_access.byte_order = unijit::ir::MemoryByteOrder::kBigEndian;
  unijit::ir::FunctionBuilder vector_memory_builder(0, 1);
  const auto vector_memory_constant = vector_memory_builder.vector_constant(
      unijit::ir::ValueType::kI32x4, vector_memory_bits);
  vector_memory_builder.store_vector(vector_memory_builder.constant(3),
                                     vector_memory_constant,
                                     vector_memory_access, 72);
  const auto vector_memory_loaded = vector_memory_builder.load_vector(
      vector_memory_builder.constant(3), unijit::ir::ValueType::kI32x4,
      vector_memory_access, 73);
  if (!vector_memory_builder
           .set_return(vector_memory_builder.vector_extract_lane(
               vector_memory_loaded, 2))
           .ok()) {
    return 72;
  }
  const auto vector_memory_function = std::move(vector_memory_builder).build();
  const auto vector_memory_preflight = unijit::jit::preflight_capabilities(
      vector_memory_function, unijit::jit::baseline_target_profile());
  auto vector_memory_compilation =
      unijit::jit::Compiler::compile(vector_memory_function);
  alignas(16) std::array<std::uint8_t, 32> vector_memory_bytes{};
  vector_memory_bytes.fill(0xCC);
  unijit::runtime::MemoryRegion vector_memory_region{
      vector_memory_bytes.data(), vector_memory_bytes.size(), true};
  unijit::runtime::ExecutionContext vector_memory_context;
  if (!vector_memory_context.bind_memory_regions(&vector_memory_region, 1)
           .ok()) {
    return 73;
  }
  const auto vector_memory_result =
      vector_memory_compilation.ok()
          ? vector_memory_compilation.function->invoke(nullptr, 0,
                                                       &vector_memory_context)
          : unijit::ir::EvaluationResult{};
  const auto vector_memory_expected = unijit::ir::vector_extract_lane_bits(
      vector_memory_bits, unijit::ir::ValueType::kI32x4, 2, false);
  if (!vector_memory_preflight.ok() || !vector_memory_compilation.ok() ||
      !vector_memory_result.ok() ||
      vector_memory_compilation.function->capabilities().target_key() !=
          vector_memory_preflight.target_key() ||
      vector_memory_compilation.function->capabilities().overall_strategy !=
          vector_memory_preflight.overall_strategy ||
      vector_memory_result.value != vector_memory_expected ||
      vector_memory_bytes[3] != 4 || vector_memory_bytes[4] != 3 ||
      vector_memory_bytes[5] != 2 || vector_memory_bytes[6] != 1 ||
      vector_memory_bytes[15] != 16 || vector_memory_bytes[16] != 15 ||
      vector_memory_bytes[17] != 14 || vector_memory_bytes[18] != 13) {
    return 74;
  }

  const auto atomic_access = [](unijit::ir::AtomicMemoryOrder order) {
    unijit::ir::AtomicAccessDescriptor descriptor;
    descriptor.memory.width = unijit::ir::MemoryWidth::k64;
    descriptor.memory.alignment = 8;
    descriptor.order = order;
    return descriptor;
  };
  const auto atomic_host = unijit::jit::host_target_profile();
  unijit::jit::CompilationOptions atomic_options;
  atomic_options.target_profile = atomic_host;
  unijit::ir::FunctionBuilder atomic_builder(0, 1);
  const auto atomic_offset = atomic_builder.constant(0);
  atomic_builder.atomic_store(
      atomic_offset, atomic_builder.constant(41),
      atomic_access(unijit::ir::AtomicMemoryOrder::kRelease), 75);
  const auto atomic_observed = atomic_builder.atomic_fetch_add(
      atomic_offset, atomic_builder.constant(1),
      atomic_access(
          unijit::ir::AtomicMemoryOrder::kSequentiallyConsistent),
      76);
  const auto atomic_loaded = atomic_builder.atomic_load(
      atomic_offset,
      atomic_access(unijit::ir::AtomicMemoryOrder::kAcquire), 77);
  if (!atomic_builder
           .set_return(atomic_builder.add(atomic_observed, atomic_loaded))
           .ok()) {
    return 75;
  }
  const auto atomic_function = std::move(atomic_builder).build();
  const auto atomic_preflight =
      unijit::jit::preflight_capabilities(atomic_function, atomic_host);
  auto atomic_compilation =
      unijit::jit::Compiler::compile(atomic_function, atomic_options);

  unijit::ir::ControlFlowBuilder atomic_cfg_builder(1, 1);
  const auto atomic_cfg_merge = atomic_cfg_builder.create_block(1);
  const auto atomic_cfg_observed = atomic_cfg_builder.atomic_exchange(
      atomic_cfg_builder.parameter(0), atomic_cfg_builder.constant(5),
      atomic_access(unijit::ir::AtomicMemoryOrder::kAcquireRelease), 78);
  if (!atomic_cfg_builder.jump(atomic_cfg_merge, {atomic_cfg_observed}).ok() ||
      !atomic_cfg_builder.set_insertion_block(atomic_cfg_merge).ok() ||
      !atomic_cfg_builder
           .set_return(atomic_cfg_builder.add(
               atomic_cfg_builder.block_parameter(atomic_cfg_merge, 0),
               atomic_cfg_builder.constant(1)))
           .ok()) {
    return 76;
  }
  const auto atomic_cfg_function = std::move(atomic_cfg_builder).build();
  const auto atomic_cfg_preflight =
      unijit::jit::preflight_capabilities(atomic_cfg_function, atomic_host);
  auto atomic_cfg_compilation =
      unijit::jit::Compiler::compile(atomic_cfg_function, atomic_options);
  const bool atomic_profile_supported =
      atomic_host.architecture !=
          unijit::jit::TargetArchitecture::kRiscV64 ||
      unijit::jit::has_target_feature(
          atomic_host, unijit::jit::TargetFeature::kRiscVAtomic);
  if (!atomic_profile_supported) {
    if (atomic_preflight.ok() || atomic_compilation.ok() ||
        atomic_cfg_preflight.ok() || atomic_cfg_compilation.ok()) {
      return 77;
    }
    return 0;
  }

  alignas(8) std::uint64_t atomic_cell = 0;
  unijit::runtime::MemoryRegion atomic_region{&atomic_cell,
                                               sizeof(atomic_cell), true};
  unijit::runtime::ExecutionContext atomic_context;
  if (!atomic_context.bind_memory_regions(&atomic_region, 1).ok()) {
    return 78;
  }
  const auto atomic_result =
      atomic_compilation.ok()
          ? atomic_compilation.function->invoke(nullptr, 0, &atomic_context)
          : unijit::ir::EvaluationResult{};
  if (!atomic_preflight.ok() || !atomic_compilation.ok() ||
      !atomic_result.ok() || atomic_result.value != 83 || atomic_cell != 42 ||
      !atomic_preflight.requires_execution_context ||
      atomic_compilation.function->capabilities().target_key() !=
          atomic_preflight.target_key()) {
    return 79;
  }

  atomic_cell = 3;
  const std::array<unijit::ir::Word, 1> atomic_cfg_arguments = {0};
  const auto atomic_cfg_result =
      atomic_cfg_compilation.ok()
          ? atomic_cfg_compilation.function->invoke(
                atomic_cfg_arguments.data(), atomic_cfg_arguments.size(),
                &atomic_context)
          : unijit::ir::EvaluationResult{};
  if (!atomic_cfg_preflight.ok() || !atomic_cfg_compilation.ok() ||
      !atomic_cfg_result.ok() || atomic_cfg_result.value != 4 ||
      atomic_cell != 5 ||
      atomic_cfg_compilation.function->capabilities().target_key() !=
          atomic_cfg_preflight.target_key()) {
    return 80;
  }
  return 0;
}
