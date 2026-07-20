set(UNIJIT_QUICKJS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third/quickjs")
file(STRINGS "${UNIJIT_QUICKJS_SOURCE_DIR}/VERSION" UNIJIT_QUICKJS_VERSION
  LIMIT_COUNT 1)
set(UNIJIT_QUICKJS_INCLUDE_DIR
  "${PROJECT_BINARY_DIR}/generated/quickjs/include")
file(MAKE_DIRECTORY "${UNIJIT_QUICKJS_INCLUDE_DIR}")
configure_file(
  "${UNIJIT_QUICKJS_SOURCE_DIR}/quickjs.h"
  "${UNIJIT_QUICKJS_INCLUDE_DIR}/quickjs.h"
  COPYONLY)

add_library(unijit_quickjs_reference STATIC
  "${UNIJIT_QUICKJS_SOURCE_DIR}/quickjs.c"
  "${UNIJIT_QUICKJS_SOURCE_DIR}/dtoa.c"
  "${UNIJIT_QUICKJS_SOURCE_DIR}/libregexp.c"
  "${UNIJIT_QUICKJS_SOURCE_DIR}/libunicode.c"
  "${UNIJIT_QUICKJS_SOURCE_DIR}/cutils.c")
target_include_directories(unijit_quickjs_reference
  PRIVATE "${UNIJIT_QUICKJS_SOURCE_DIR}")
target_compile_features(unijit_quickjs_reference PRIVATE c_std_17)
target_compile_definitions(unijit_quickjs_reference PRIVATE
  _GNU_SOURCE
  CONFIG_VERSION="${UNIJIT_QUICKJS_VERSION}")

if(NOT MSVC)
  target_compile_options(unijit_quickjs_reference PRIVATE -fwrapv)
endif()

find_package(Threads REQUIRED)
target_link_libraries(unijit_quickjs_reference
  PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
  target_link_libraries(unijit_quickjs_reference PUBLIC m)
endif()

set_target_properties(unijit_quickjs_reference PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
  C_EXTENSIONS ON
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

if(UNIJIT_BUILD_TESTS)
  add_executable(unijit_quickjs_tests
    "${PROJECT_SOURCE_DIR}/test/quickjs_frontend_test.cpp")
  target_compile_features(unijit_quickjs_tests PRIVATE cxx_std_17)
  target_include_directories(unijit_quickjs_tests SYSTEM
    PRIVATE "${UNIJIT_QUICKJS_INCLUDE_DIR}")
  target_link_libraries(unijit_quickjs_tests PRIVATE unijit_quickjs_reference)
  set_target_properties(unijit_quickjs_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

  if(MSVC)
    target_compile_options(unijit_quickjs_tests PRIVATE /W4 /permissive-)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_quickjs_tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(unijit_quickjs_tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_quickjs_tests PRIVATE -Werror)
    endif()
  endif()

  add_test(NAME unijit.quickjs_frontend COMMAND unijit_quickjs_tests)
endif()
