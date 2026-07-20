add_executable(unijit_tests test_main.cpp)
target_link_libraries(unijit_tests PRIVATE UniJIT::unijit)
target_compile_features(unijit_tests PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(unijit_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(unijit_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME unijit.unit COMMAND unijit_tests)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME unijit.release_metadata
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/release.py" check)
  add_test(
    NAME unijit.release_tool
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/release_tool_test.py")
endif()
