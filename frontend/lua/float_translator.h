#ifndef UNIJIT_FRONTEND_LUA_FLOAT_TRANSLATOR_H
#define UNIJIT_FRONTEND_LUA_FLOAT_TRANSLATOR_H

#include "unijit/jit/compiler.h"

extern "C" {
struct Proto;
}

namespace unijit::frontend::lua55::detail {

jit::CompilationResult compile_float64_prototype(const Proto& prototype);

}  // namespace unijit::frontend::lua55::detail

#endif  // UNIJIT_FRONTEND_LUA_FLOAT_TRANSLATOR_H
