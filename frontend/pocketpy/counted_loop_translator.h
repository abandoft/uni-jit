#ifndef UNIJIT_FRONTEND_POCKETPY_COUNTED_LOOP_TRANSLATOR_H
#define UNIJIT_FRONTEND_POCKETPY_COUNTED_LOOP_TRANSLATOR_H

#include <string_view>

#include "source_translator.h"

namespace unijit::frontend::pocketpy {

bool looks_like_counted_loop(std::string_view source) noexcept;
TranslationResult translate_counted_loop(std::string_view source);

} // namespace unijit::frontend::pocketpy

#endif // UNIJIT_FRONTEND_POCKETPY_COUNTED_LOOP_TRANSLATOR_H
