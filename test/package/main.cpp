#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compilation_scheduler.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"
#include "unijit/runtime/deoptimization.h"
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
  return 0;
}
