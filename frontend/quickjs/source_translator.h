#ifndef UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H
#define UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "unijit/jit/compiler.h"
#include "unijit/status.h"

namespace unijit::frontend::quickjs {

enum class ResultKind : std::uint8_t {
  kFloat64 = 0,
  kBoolean,
};

struct TranslationResult final {
  Status status;
  std::size_t parameter_count{0};
  std::unique_ptr<jit::CompiledFunction> function;
  ResultKind result_kind{ResultKind::kFloat64};

  bool ok() const noexcept { return status.ok() && function != nullptr; }
};

TranslationResult translate_numeric_function(
    std::string_view source,
    jit::OptimizationLevel optimization_level =
        jit::OptimizationLevel::kOptimized);
bool supports_tiered_translation(std::string_view source) noexcept;

}  // namespace unijit::frontend::quickjs

#endif  // UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H
