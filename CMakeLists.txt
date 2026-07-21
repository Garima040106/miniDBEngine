cmake_minimum_required(VERSION 3.20)
project(tinylsm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(src)

option(TINYLSM_BUILD_TESTS "Build the test suite" ON)
if(TINYLSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
