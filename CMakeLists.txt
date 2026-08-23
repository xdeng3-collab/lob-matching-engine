cmake_minimum_required(VERSION 3.16)
project(lob_matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob INTERFACE -Wall -Wextra)

enable_testing()

add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching PRIVATE lob)
target_include_directories(test_matching PRIVATE tests)
add_test(NAME matching COMMAND test_matching)

add_executable(test_invariants tests/test_invariants.cpp)
target_link_libraries(test_invariants PRIVATE lob)
target_include_directories(test_invariants PRIVATE tests)
add_test(NAME invariants COMMAND test_invariants)

add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE lob)
target_compile_options(bench_book PRIVATE -O2)
