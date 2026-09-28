add_library(mcode_mimalloc STATIC mcode/support/mimalloc_override.cxx)
target_link_libraries(mcode_mimalloc PUBLIC mimalloc-static)
mcode_configure_target(mcode_mimalloc)

add_library(mcode_core STATIC
    mcode/core/registry.cxx
    mcode/core/version.cxx

    mcode/support/logging.cxx
    mcode/support/json.cxx
    mcode/support/text.cxx
    mcode/support/toml.cxx
    mcode/support/config.cxx

    mcode/ext/hooks.cxx
    mcode/ext/lua_host.cxx

    mcode/model/types.cxx
    mcode/model/provider.cxx
    mcode/model/delta_applier.cxx

    mcode/platform/seams.cxx

    mcode/events/bus.cxx

    mcode/cli/exec.cxx

    mcode/eval/suite.cxx

    mcode/ext/api.cxx
    mcode/ext/loader.cxx
    mcode/ext/lua_json.cxx
    mcode/ext/manifest.cxx

    mcode/fs/workspace.cxx

    mcode/net/sse.cxx
    mcode/net/http_client.cxx

    mcode/proc/process.cxx

    mcode/agent/loop.cxx
)

target_include_directories(mcode_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

if(APPLE)
    set(MCODE_PLATFORM "macos-${CMAKE_SYSTEM_PROCESSOR}")
elseif(WIN32)
    set(MCODE_PLATFORM "windows-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(MCODE_PLATFORM "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(MSVC)
    set(MCODE_COMPILER "msvc-${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MCODE_COMPILER "clang-${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MCODE_COMPILER "gcc-${CMAKE_CXX_COMPILER_VERSION}")
else()
    set(MCODE_COMPILER "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/mcode/core/version.hxx.in
    ${CMAKE_CURRENT_BINARY_DIR}/mcode/core/version.hxx
    @ONLY
)

target_link_libraries(mcode_core
    PUBLIC
        mcode_mimalloc
        fmt::fmt
        spdlog::spdlog
        yyjson::yyjson
        simdutf::simdutf
        unordered_dense::unordered_dense
        luau::vm
        luau::compiler
    PRIVATE
        Boost::process
        Boost::boost
)

mcode_configure_target(mcode_core)

if(NOT WIN32 AND NOT APPLE)
    target_link_options(mcode_core PUBLIC "-Wl,--export-dynamic")
endif()

set(MCODE_BUNDLED_EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extensions")

add_executable(mcode main.cxx)
target_link_libraries(mcode PRIVATE mcode_core)

# The binary cannot live at `src/mcode`: that path is the SOURCE DIRECTORY, and on
# Linux and macOS the executable has no suffix, so the linker tries to open a
# directory for writing. Windows hid this behind `.exe` for the whole project's
# life. Runtime output goes to bin/ on every platform, so the path is uniform.
set_target_properties(mcode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The smoke test loads the real fixture extensions, so it needs their path. The
# fixture lives beside the tests rather than in the source tree's extensions/,
# because a broken fixture must not be loadable by a normal run.
target_compile_definitions(mcode PRIVATE
    MCODE_SMOKE_EXTENSIONS="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/extensions"
    MCODE_SMOKE_SHIPPED_EXTENSIONS="${MCODE_BUNDLED_EXT_DIR}")

mcode_configure_target(mcode)

if(EXISTS "${MCODE_BUNDLED_EXT_DIR}")
    add_custom_target(mcode_copy_extensions ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MCODE_BUNDLED_EXT_DIR}"
                "$<TARGET_FILE_DIR:mcode>/extensions"
        COMMENT "Copying bundled extensions next to the binary"
        VERBATIM
    )
    add_dependencies(mcode mcode_copy_extensions)
endif()
