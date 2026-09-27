cmake_minimum_required(VERSION 3.20)

# Measurement spine: load, memory, and dispatch benchmarks.
#
# Built as part of the main project so it links the real lua_host rather than a
# copy. Not installed, not shipped.

add_executable(mcode_bench main.cxx)
target_link_libraries(mcode_bench PRIVATE mcode_core)
mcode_configure_target(mcode_bench)

if(MSVC)
    set_property(TARGET mcode_bench PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
