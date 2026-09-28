cmake_minimum_required(VERSION 3.20)
project(fixture_repo LANGUAGES CXX)

add_library(fixture STATIC src/calculator.cxx)
target_include_directories(fixture PUBLIC include)

add_executable(fixture_tests tests/test_calculator.cxx)
target_link_libraries(fixture_tests PRIVATE fixture)
