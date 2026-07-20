cmake_minimum_required(VERSION 3.20)

project(
  UniJIT
  VERSION 0.1.2
  DESCRIPTION "A unified high-performance JIT runtime"
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UNIJIT_BUILD_TESTS "Build UniJIT tests" ON)
option(UNIJIT_BUILD_BENCHMARKS "Build UniJIT benchmarks" OFF)
option(UNIJIT_BUILD_LUA_REFERENCE "Build the pinned stock Lua runtime" OFF)
option(UNIJIT_BUILD_LUAJIT_REFERENCE "Build the pinned LuaJIT runtime" OFF)
option(UNIJIT_WARNINGS_AS_ERRORS "Treat UniJIT warnings as errors" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_subdirectory(src)

if(UNIJIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(UNIJIT_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

if(UNIJIT_BUILD_LUA_REFERENCE)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third/lua/lua.h")
    message(FATAL_ERROR "Lua submodule is missing; run git submodule update --init")
  endif()
  add_subdirectory(frontends/lua)
endif()

if(UNIJIT_BUILD_LUAJIT_REFERENCE)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third/luajit/src/Makefile")
    message(FATAL_ERROR "LuaJIT submodule is missing; run git submodule update --init")
  endif()
  add_subdirectory(benchmark/references)
endif()

install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(
  EXPORT UniJITTargets
  FILE UniJITTargets.cmake
  NAMESPACE UniJIT::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/UniJIT")

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/UniJITConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/UniJITConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/UniJIT")

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/UniJITConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(
  FILES
    "${PROJECT_BINARY_DIR}/UniJITConfig.cmake"
    "${PROJECT_BINARY_DIR}/UniJITConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/UniJIT")
