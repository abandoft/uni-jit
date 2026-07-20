#include <array>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/jit/code_cache.h"
#include "unijit/jit/compiler.h"
#include "unijit/runtime/deoptimization.h"

int main() {
  unijit::ir::FunctionBuilder builder(2);
  const auto sum = builder.add(builder.parameter(0), builder.parameter(1));
  if (!builder.set_return(sum).ok()) {
    return 1;
  }
  const auto function = std::move(builder).build();
  auto compilation = unijit::jit::Compiler::compile(function);
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
  return !guarded_result.ok() && reconstructed.ok() && recovered != nullptr &&
                 recovered->value == zero[0]
             ? 0
             : 12;
}
