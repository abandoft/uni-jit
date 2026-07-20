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

add_library(unijit_lua55_frontend STATIC
  float_translator.cpp
  jit_module.cpp)
target_include_directories(unijit_lua55_frontend
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${UNIJIT_LUA_SOURCE_DIR}")
target_compile_features(unijit_lua55_frontend PRIVATE cxx_std_17)
target_link_libraries(unijit_lua55_frontend
  PUBLIC UniJIT::unijit unijit_lua55_reference)
set_target_properties(unijit_lua55_frontend PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

if(MSVC)
  target_compile_options(unijit_lua55_frontend PRIVATE /W4 /permissive-)
  if(UNIJIT_WARNINGS_AS_ERRORS)
    target_compile_options(unijit_lua55_frontend PRIVATE /WX)
  endif()
else()
  target_compile_options(unijit_lua55_frontend PRIVATE -Wall -Wextra -Wpedantic)
  if(UNIJIT_WARNINGS_AS_ERRORS)
    target_compile_options(unijit_lua55_frontend PRIVATE -Werror)
  endif()
endif()

if(UNIJIT_BUILD_TESTS)
  add_executable(unijit_lua55_tests
    "${PROJECT_SOURCE_DIR}/test/lua_frontend_test.cpp")
  target_compile_features(unijit_lua55_tests PRIVATE cxx_std_17)
  target_link_libraries(unijit_lua55_tests PRIVATE unijit_lua55_frontend)
  set_target_properties(unijit_lua55_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

  if(MSVC)
    target_compile_options(unijit_lua55_tests PRIVATE /W4 /permissive-)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_lua55_tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(unijit_lua55_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_lua55_tests PRIVATE -Werror)
    endif()
  endif()

  add_test(NAME unijit.lua55_frontend COMMAND unijit_lua55_tests)
endif()

if(UNIJIT_BUILD_BENCHMARKS)
  add_executable(unijit_lua55_benchmark_runner
    "${PROJECT_SOURCE_DIR}/benchmark/lua/embedded_runner.cpp")
  target_compile_features(unijit_lua55_benchmark_runner PRIVATE cxx_std_17)
  target_link_libraries(unijit_lua55_benchmark_runner
    PRIVATE unijit_lua55_frontend)
  set_target_properties(unijit_lua55_benchmark_runner PROPERTIES
    OUTPUT_NAME "lua5.5-unijit"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

  if(MSVC)
    target_compile_options(unijit_lua55_benchmark_runner
      PRIVATE /W4 /permissive-)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_lua55_benchmark_runner PRIVATE /WX)
    endif()
  else()
    target_compile_options(unijit_lua55_benchmark_runner
      PRIVATE -Wall -Wextra -Wpedantic)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_lua55_benchmark_runner PRIVATE -Werror)
    endif()
  endif()

  if(UNIJIT_BUILD_TESTS)
    add_test(
      NAME unijit.lua55_benchmark_smoke
      COMMAND unijit_lua55_benchmark_runner
        "${PROJECT_SOURCE_DIR}/benchmark/lua/integer_call.lua"
        100 1000 1 unijit)
  endif()
endif()
