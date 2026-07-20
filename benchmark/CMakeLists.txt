add_executable(unijit_core_benchmark core_arithmetic.cpp)
target_link_libraries(unijit_core_benchmark PRIVATE UniJIT::unijit)
target_compile_features(unijit_core_benchmark PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(unijit_core_benchmark PRIVATE /W4 /permissive-)
else()
  target_compile_options(unijit_core_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
