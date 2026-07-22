add_library(unijit
  embedding.cpp
  ir/atomic_access.cpp
  ir/control_flow.cpp
  ir/function.cpp
  ir/interpreter.cpp
  ir/memory_access.cpp
  ir/object_access.cpp
  ir/optimizer.cpp
  ir/package_sha256.cpp
  ir/vector.cpp
  ir/verifier.cpp
  jit/capability.cpp
  jit/code_cache.cpp
  jit/compilation_scheduler.cpp
  jit/compiler.cpp
  jit/executable_memory.cpp
  jit/register_allocator.cpp
  jit/target.cpp
  jit/tiering.cpp
  runtime/assumption.cpp
  runtime/deoptimization.cpp
  runtime/execution_context.cpp
  runtime/materialization.cpp
  runtime/on_stack_replacement.cpp)

set(UNIJIT_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES UNIJIT_OSX_ARCH_COUNT)
  if(NOT UNIJIT_OSX_ARCH_COUNT EQUAL 1)
    message(FATAL_ERROR "UniJIT currently requires one target architecture per build")
  endif()
  set(UNIJIT_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()

string(TOLOWER "${UNIJIT_PROCESSOR}" UNIJIT_SYSTEM_PROCESSOR)
if(UNIJIT_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  target_sources(unijit PRIVATE
    jit/atomic_fallback.cpp
    jit/backend/aarch64/lower.cpp)
  target_compile_definitions(unijit PRIVATE UNIJIT_TARGET_AARCH64=1)
elseif(UNIJIT_SYSTEM_PROCESSOR MATCHES "^(amd64|x86_64)$")
  target_sources(unijit PRIVATE
    jit/backend/x86_64/lower.cpp)
  target_compile_definitions(unijit PRIVATE UNIJIT_TARGET_X86_64=1)
elseif(UNIJIT_SYSTEM_PROCESSOR MATCHES "^(riscv64|riscv64gc)$")
  target_sources(unijit PRIVATE
    jit/atomic_fallback.cpp
    jit/backend/riscv64/lower.cpp)
  target_compile_definitions(unijit PRIVATE UNIJIT_TARGET_RISCV64=1)
endif()

add_library(UniJIT::unijit ALIAS unijit)

find_package(Threads REQUIRED)
target_link_libraries(unijit PUBLIC Threads::Threads)

target_include_directories(unijit
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_compile_features(unijit PUBLIC cxx_std_17)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(unijit PRIVATE UNIJIT_BUILDING_LIBRARY=1)
endif()

if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(UNIJIT_C_API_VERSION_SCRIPT
      "${PROJECT_SOURCE_DIR}/cmake/unijit-c-api.map")
  target_link_options(
    unijit PRIVATE "-Wl,--version-script=${UNIJIT_C_API_VERSION_SCRIPT}")
  set_property(
    TARGET unijit APPEND PROPERTY LINK_DEPENDS
    "${UNIJIT_C_API_VERSION_SCRIPT}")
endif()

if(MSVC)
  target_compile_options(unijit PRIVATE /W4 /permissive-)
  if(UNIJIT_WARNINGS_AS_ERRORS)
    target_compile_options(unijit PRIVATE /WX)
  endif()
else()
  target_compile_options(unijit PRIVATE -Wall -Wextra -Wpedantic)
  if(UNIJIT_WARNINGS_AS_ERRORS)
    target_compile_options(unijit PRIVATE -Werror)
  endif()
endif()

set_target_properties(unijit PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}")

install(
  TARGETS unijit
  EXPORT UniJITTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
