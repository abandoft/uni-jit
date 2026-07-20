cmake_minimum_required(VERSION 3.20)
project(UniJITPackageConsumer LANGUAGES CXX)

find_package(UniJIT 0.1 REQUIRED CONFIG)

add_executable(unijit_package_consumer main.cpp)
target_link_libraries(unijit_package_consumer PRIVATE UniJIT::unijit)
target_compile_features(unijit_package_consumer PRIVATE cxx_std_17)
