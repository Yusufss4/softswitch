cmake_minimum_required(VERSION 3.20)
project(softswitch VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

add_library(softswitch_core
  src/mac_table.cpp
)
target_include_directories(softswitch_core PUBLIC include)

add_executable(softswitch src/main.cpp)
target_link_libraries(softswitch PRIVATE softswitch_core)

option(SOFTSWITCH_BUILD_TESTS "Build unit tests" ON)
if(SOFTSWITCH_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(softswitch_tests tests/test_mac_table.cpp)
  target_link_libraries(softswitch_tests PRIVATE softswitch_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(softswitch_tests)
endif()
