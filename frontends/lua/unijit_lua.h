#ifndef UNIJIT_FRONTENDS_LUA_UNIJIT_LUA_H
#define UNIJIT_FRONTENDS_LUA_UNIJIT_LUA_H

#if defined(__cplusplus)
extern "C" {
#endif

#include "lua.h"

int luaopen_unijit(lua_State *state);

#if defined(__cplusplus)
}
#endif

#endif  // UNIJIT_FRONTENDS_LUA_UNIJIT_LUA_H
