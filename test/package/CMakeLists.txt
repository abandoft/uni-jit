cmake_minimum_required(VERSION 3.20)
project(UniJITPackageConsumer LANGUAGES C CXX)

find_package(UniJIT 0.1 REQUIRED CONFIG)

add_executable(unijit_package_consumer main.cpp)
target_link_libraries(unijit_package_consumer PRIVATE UniJIT::unijit)
target_compile_features(unijit_package_consumer PRIVATE cxx_std_17)

add_executable(unijit_c_package_consumer embedding.c)
target_link_libraries(unijit_c_package_consumer PRIVATE UniJIT::unijit)
target_compile_features(unijit_c_package_consumer PRIVATE c_std_17)
set_target_properties(unijit_c_package_consumer PROPERTIES LINKER_LANGUAGE CXX)
