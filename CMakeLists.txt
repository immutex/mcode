cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED MCODE_VERSION)
    set(MCODE_VERSION 0.0.2)
endif()

project(mcode
    VERSION ${MCODE_VERSION}
    DESCRIPTION "Extensible C++23 coding-agent harness with a Luau extension layer"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(PlatformTarget)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MCODE_BUILD_TESTS "Build the test suite" ON)
option(MCODE_WARNINGS_AS_ERRORS "Promote compiler warnings to errors" OFF)
option(MCODE_ENABLE_LTO "Enable link-time optimization on Release" ON)

# Boost.Process v2 defaults to boost::filesystem, a compiled library that would
# duplicate std::filesystem. This must be defined in every TU.
add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:BOOST_PROCESS_USE_STD_FS>)

# Boost's MSVC autolink demands libraries mcode does not link; components are
# named explicitly instead (Boost::process, Boost::boost).
add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:BOOST_ALL_NO_LIB>)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(simdutf CONFIG REQUIRED)
find_package(unordered_dense CONFIG REQUIRED)
find_package(luau CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_subdirectory(src)

if(MCODE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Measurement spine (docs/26 track B). Links the real extension host so the
# numbers describe the shipped code, not a copy.
add_subdirectory(tools/bench)
