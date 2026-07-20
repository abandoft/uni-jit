set(UNIJIT_LUA_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third/lua")

set(UNIJIT_LUA_CORE_SOURCES
  lapi.c
  lcode.c
  lctype.c
  ldebug.c
  ldo.c
  ldump.c
  lfunc.c
  lgc.c
  llex.c
  lmem.c
  lobject.c
  lopcodes.c
  lparser.c
  lstate.c
  lstring.c
  ltable.c
  ltm.c
  lundump.c
  lvm.c
  lzio.c
  lauxlib.c
  lbaselib.c
  ldblib.c
  liolib.c
  lmathlib.c
  loslib.c
  ltablib.c
  lstrlib.c
  lutf8lib.c
  loadlib.c
  lcorolib.c
  linit.c)

list(TRANSFORM UNIJIT_LUA_CORE_SOURCES
  PREPEND "${UNIJIT_LUA_SOURCE_DIR}/")

add_library(unijit_lua55_reference STATIC ${UNIJIT_LUA_CORE_SOURCES})
target_include_directories(unijit_lua55_reference
  PUBLIC "${UNIJIT_LUA_SOURCE_DIR}")
target_compile_features(unijit_lua55_reference PRIVATE c_std_17)
target_link_libraries(unijit_lua55_reference PUBLIC ${CMAKE_DL_LIBS})

if(UNIX)
  target_link_libraries(unijit_lua55_reference PUBLIC m)
endif()

if(APPLE)
  target_compile_definitions(unijit_lua55_reference PRIVATE LUA_USE_MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(unijit_lua55_reference PRIVATE LUA_USE_LINUX)
elseif(WIN32)
  target_compile_definitions(unijit_lua55_reference PRIVATE LUA_USE_WINDOWS)
endif()

set_target_properties(unijit_lua55_reference PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

add_executable(unijit_lua55 "${UNIJIT_LUA_SOURCE_DIR}/lua.c")
target_compile_features(unijit_lua55 PRIVATE c_std_17)
target_link_libraries(unijit_lua55 PRIVATE unijit_lua55_reference)
set_target_properties(unijit_lua55 PROPERTIES
  OUTPUT_NAME "lua5.5-reference"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
