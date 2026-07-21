add_executable(unijit_core_benchmark core_arithmetic.cpp)
target_link_libraries(unijit_core_benchmark PRIVATE UniJIT::unijit)
target_compile_features(unijit_core_benchmark PRIVATE cxx_std_17)

add_executable(unijit_cfg_float64_benchmark cfg_float64.cpp)
target_link_libraries(unijit_cfg_float64_benchmark PRIVATE UniJIT::unijit)
target_compile_features(unijit_cfg_float64_benchmark PRIVATE cxx_std_17)

add_executable(unijit_cfg_simd_benchmark cfg_simd.cpp)
target_link_libraries(unijit_cfg_simd_benchmark PRIVATE UniJIT::unijit)
target_compile_features(unijit_cfg_simd_benchmark PRIVATE cxx_std_17)

add_executable(unijit_bounded_memory_benchmark bounded_memory.cpp)
target_link_libraries(unijit_bounded_memory_benchmark PRIVATE UniJIT::unijit)
target_compile_features(unijit_bounded_memory_benchmark PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(unijit_core_benchmark PRIVATE /W4 /permissive-)
  target_compile_options(
    unijit_cfg_float64_benchmark PRIVATE /W4 /permissive-)
  target_compile_options(
    unijit_cfg_simd_benchmark PRIVATE /W4 /permissive-)
  target_compile_options(
    unijit_bounded_memory_benchmark PRIVATE /W4 /permissive-)
else()
  target_compile_options(unijit_core_benchmark PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(
    unijit_cfg_float64_benchmark PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(
    unijit_cfg_simd_benchmark PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(
    unijit_bounded_memory_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
