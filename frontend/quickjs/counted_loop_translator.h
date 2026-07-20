#ifndef UNIJIT_FRONTEND_QUICKJS_COUNTED_LOOP_TRANSLATOR_H
#define UNIJIT_FRONTEND_QUICKJS_COUNTED_LOOP_TRANSLATOR_H

#include <string_view>

#include "source_translator.h"

namespace unijit::frontend::quickjs {

bool looks_like_counted_loop(std::string_view source) noexcept;
TranslationResult translate_counted_loop(std::string_view source);

}  // namespace unijit::frontend::quickjs

#endif  // UNIJIT_FRONTEND_QUICKJS_COUNTED_LOOP_TRANSLATOR_H
