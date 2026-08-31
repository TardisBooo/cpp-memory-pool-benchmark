cmake_minimum_required(VERSION 3.18)
project(cpp_memory_pool_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE include)

add_executable(memory_pool_benchmark src/main.cpp)
target_link_libraries(memory_pool_benchmark PRIVATE memory_pool)

enable_testing()
add_executable(memory_pool_tests tests/test_pool.cpp)
target_link_libraries(memory_pool_tests PRIVATE memory_pool)
add_test(NAME memory_pool_tests COMMAND memory_pool_tests)
