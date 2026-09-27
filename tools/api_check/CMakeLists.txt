cmake_minimum_required(VERSION 3.20)

# Standalone build for the API-definition checker.
#
# Deliberately NOT part of the main project build: it needs Luau.Analysis, which
# links the compiler and the AST parser, and the shipped binary must not carry
# any of that. Build it on demand:
#
#   cmake -S tools/api_check -B tools/api_check/build -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release -DLUAU_SOURCE_DIR=<path-to-luau>
#   cmake --build tools/api_check/build
#
# CI runs it against extensions/ as part of the API freeze check.

project(mcode_api_check LANGUAGES CXX)

# Luau is built with LUAU_STATIC_CRT=ON below, so this tool must match. Mixing
# /MT and /MD across the same link is a LNK2038 RuntimeLibrary mismatch.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(NOT LUAU_SOURCE_DIR)
    message(FATAL_ERROR "Set -DLUAU_SOURCE_DIR to a Luau checkout.")
endif()

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)

add_subdirectory("${LUAU_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/luau" EXCLUDE_FROM_ALL)

add_executable(mcode_api_check main.cxx)
target_compile_features(mcode_api_check PRIVATE cxx_std_17)
target_link_libraries(mcode_api_check PRIVATE Luau.Analysis Luau.Compiler Luau.Ast Luau.VM Luau.Common)

if(MSVC)
    target_compile_options(mcode_api_check PRIVATE /W4 /permissive-)
    target_compile_options(Luau.VM PRIVATE /wd4244 /wd4267)
endif()
