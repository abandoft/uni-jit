set(UNIJIT_POCKETPY_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third/pocketpy")

file(GLOB_RECURSE UNIJIT_POCKETPY_SOURCES CONFIGURE_DEPENDS
  "${UNIJIT_POCKETPY_SOURCE_DIR}/src/*.c")

add_library(unijit_pocketpy_reference STATIC ${UNIJIT_POCKETPY_SOURCES})
target_include_directories(unijit_pocketpy_reference SYSTEM
  PUBLIC "${UNIJIT_POCKETPY_SOURCE_DIR}/include")
target_compile_features(unijit_pocketpy_reference PRIVATE c_std_17)
target_compile_definitions(unijit_pocketpy_reference PRIVATE
  PK_ENABLE_CUSTOM_SNAME=0
  PK_ENABLE_DETERMINISM=1
  PK_ENABLE_MIMALLOC=0
  PK_ENABLE_OS=1
  PK_ENABLE_THREADS=1
  PK_ENABLE_WATCHDOG=0)

find_package(Threads REQUIRED)
target_link_libraries(unijit_pocketpy_reference
  PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
  target_link_libraries(unijit_pocketpy_reference PUBLIC m)
elseif(WIN32)
  target_link_libraries(unijit_pocketpy_reference PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_definitions(unijit_pocketpy_reference PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_compile_options(unijit_pocketpy_reference PRIVATE
    /utf-8 /jumptablerdata /experimental:c11atomics)
else()
  target_compile_options(unijit_pocketpy_reference PRIVATE -ffp-contract=off)
endif()

set_target_properties(unijit_pocketpy_reference PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

if(UNIJIT_BUILD_TESTS)
  add_executable(unijit_pocketpy_reference_tests
    "${PROJECT_SOURCE_DIR}/test/pocketpy_reference_test.c")
  target_compile_features(unijit_pocketpy_reference_tests PRIVATE c_std_17)
  target_link_libraries(unijit_pocketpy_reference_tests
    PRIVATE unijit_pocketpy_reference)
  set_target_properties(unijit_pocketpy_reference_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

  if(MSVC)
    target_compile_options(unijit_pocketpy_reference_tests PRIVATE /W4)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_pocketpy_reference_tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(unijit_pocketpy_reference_tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(UNIJIT_WARNINGS_AS_ERRORS)
      target_compile_options(unijit_pocketpy_reference_tests PRIVATE -Werror)
    endif()
  endif()

  add_test(NAME unijit.pocketpy_reference
    COMMAND unijit_pocketpy_reference_tests)
endif()
