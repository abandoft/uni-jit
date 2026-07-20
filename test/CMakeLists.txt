add_executable(unijit_tests test_main.cpp)
find_package(Threads REQUIRED)
target_link_libraries(unijit_tests PRIVATE UniJIT::unijit Threads::Threads)
target_compile_features(unijit_tests PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(unijit_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(unijit_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME unijit.unit COMMAND unijit_tests)

if(UNIJIT_BUILD_QUALIFICATION_TESTS)
  add_executable(unijit_code_cache_stress code_cache_stress.cpp)
  target_link_libraries(
    unijit_code_cache_stress PRIVATE UniJIT::unijit Threads::Threads)
  target_compile_features(unijit_code_cache_stress PRIVATE cxx_std_17)

  add_executable(
    unijit_compilation_scheduler_stress compilation_scheduler_stress.cpp)
  target_link_libraries(
    unijit_compilation_scheduler_stress PRIVATE UniJIT::unijit Threads::Threads)
  target_compile_features(
    unijit_compilation_scheduler_stress PRIVATE cxx_std_17)

  add_executable(unijit_differential_fuzz differential_fuzz.cpp)
  target_link_libraries(unijit_differential_fuzz PRIVATE UniJIT::unijit)
  target_compile_features(unijit_differential_fuzz PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(
      unijit_code_cache_stress PRIVATE /W4 /permissive-)
    target_compile_options(
      unijit_compilation_scheduler_stress PRIVATE /W4 /permissive-)
    target_compile_options(
      unijit_differential_fuzz PRIVATE /W4 /permissive-)
  else()
    target_compile_options(
      unijit_code_cache_stress PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(
      unijit_compilation_scheduler_stress PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(
      unijit_differential_fuzz PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(
    NAME unijit.qualification.code_cache_stress
    COMMAND unijit_code_cache_stress
            --seed 0x4341434845535452
            --readers 8
            --writers 2
            --reads 25000
            --publications 2000
            --keys 16)
  add_test(
    NAME unijit.qualification.differential_fuzz
    COMMAND unijit_differential_fuzz
            --seed 0x554E494A4954465A
            --programs 128
            --inputs 64
            --nodes 48)
  add_test(
    NAME unijit.qualification.compilation_scheduler_stress
    COMMAND unijit_compilation_scheduler_stress
            --producers 8
            --submissions 4096
            --identities 128
            --workers 4)
  set_tests_properties(
    unijit.qualification.code_cache_stress
    unijit.qualification.differential_fuzz
    unijit.qualification.compilation_scheduler_stress
    PROPERTIES LABELS qualification TIMEOUT 180)
endif()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME unijit.release_metadata
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tool/release.py" check)
  add_test(
    NAME unijit.release_tool
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/release_tool_test.py")
  add_test(
    NAME unijit.performance_gate_tool
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/test/performance_gate_test.py")
endif()
