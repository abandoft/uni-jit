#include <cstdlib>
#include <iostream>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

#include "unijit_lua.h"

namespace {

int report_lua_error(lua_State *state) {
  const char *message = lua_tostring(state, -1);
  std::cerr << (message == nullptr ? "unknown Lua error" : message) << '\n';
  return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: lua5.5-unijit SCRIPT [ARGUMENT ...]\n";
    return EXIT_FAILURE;
  }

  lua_State *state = luaL_newstate();
  if (state == nullptr) {
    std::cerr << "unable to create Lua state\n";
    return EXIT_FAILURE;
  }

  luaL_openlibs(state);
  luaL_requiref(state, "unijit", luaopen_unijit, 1);
  lua_pop(state, 1);

  lua_createtable(state, argc - 1, 0);
  for (int index = 1; index < argc; ++index) {
    lua_pushstring(state, argv[index]);
    lua_rawseti(state, -2, index - 1);
  }
  lua_setglobal(state, "arg");

  int status = luaL_loadfile(state, argv[1]);
  if (status == LUA_OK) {
    status = lua_pcall(state, 0, LUA_MULTRET, 0);
  }
  if (status != LUA_OK) {
    const int result = report_lua_error(state);
    lua_close(state);
    return result;
  }

  lua_close(state);
  return EXIT_SUCCESS;
}
