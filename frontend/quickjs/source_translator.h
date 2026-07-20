#ifndef UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H
#define UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H

#include <cstddef>
#include <memory>
#include <string_view>

#include "unijit/jit/compiler.h"
#include "unijit/status.h"

namespace unijit::frontend::quickjs {

struct TranslationResult final {
  Status status;
  std::size_t parameter_count{0};
  std::unique_ptr<jit::CompiledFunction> function;

  bool ok() const noexcept { return status.ok() && function != nullptr; }
};

TranslationResult translate_numeric_function(std::string_view source);

}  // namespace unijit::frontend::quickjs

#endif  // UNIJIT_FRONTEND_QUICKJS_SOURCE_TRANSLATOR_H
