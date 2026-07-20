cmake_minimum_required(VERSION 3.20)

project(
  UniJIT
  VERSION 0.1.1
  DESCRIPTION "A unified high-performance JIT runtime"
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UNIJIT_BUILD_TESTS "Build UniJIT tests" ON)
option(UNIJIT_WARNINGS_AS_ERRORS "Treat UniJIT warnings as errors" OFF)

include(GNUInstallDirs)

add_subdirectory(src)

if(UNIJIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(
  EXPORT UniJITTargets
  FILE UniJITTargets.cmake
  NAMESPACE UniJIT::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/UniJIT")
