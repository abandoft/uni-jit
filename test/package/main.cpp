#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/jit/tiering.h"
#include "unijit/runtime/deoptimization.h"

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
  if (!guarded_builder.guard_float64_nonzero(divisor, 17).valid() ||
      !guarded_builder.set_return(divisor).ok()) {
    return 8;
  }
  unijit::runtime::DeoptimizationRecord record;
  record.site = 17;
  record.resume_offset = 9;
  record.reason = unijit::runtime::DeoptimizationReason::kDivisionByZero;
  record.recovery = {unijit::runtime::RecoveryOperation::argument(
      0, unijit::ir::ValueType::kFloat64, 0)};
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
      guarded_publication.handle.deoptimization_record(17) == nullptr) {
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
  const auto *recovered = reconstructed.frame.find(0);
  if (guarded_result.ok() || !reconstructed.ok() || recovered == nullptr ||
      recovered->value != zero[0]) {
    return 12;
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
  return optimized_result.ok() &&
                 optimized_result.attempted_handle.generation() ==
                     optimized_publication.handle.generation() &&
                 optimized_result.attempted_tier ==
                     unijit::jit::CodeTier::kOptimized &&
                 optimized_result.result.value == tiered_arguments[0]
             ? 0
             : 23;
}
