add_library(mcode_mimalloc STATIC mcode/support/mimalloc_override.cxx)
target_link_libraries(mcode_mimalloc PUBLIC mimalloc-static)
mcode_configure_target(mcode_mimalloc)

add_library(mcode_core STATIC
    mcode/core/registry.cxx
    mcode/core/version.cxx

    mcode/support/logging.cxx
    mcode/support/json.cxx
    mcode/support/json_internal.cxx
    mcode/support/json_write.cxx
    mcode/support/text.cxx
    mcode/support/toml.cxx
    mcode/support/toml_access.cxx
    mcode/support/config.cxx
    mcode/support/mcp_config.cxx
    mcode/support/time.cxx
    mcode/support/parse.cxx
    mcode/support/glob.cxx

    mcode/ext/hooks.cxx
    mcode/ext/lua_host.cxx
    mcode/ext/lua_host_execute.cxx
    mcode/ext/lua_host_glue.cxx

    mcode/model/types.cxx
    mcode/model/provider.cxx
    mcode/model/delta_applier.cxx
    mcode/model/render.cxx
    mcode/model/credentials.cxx
    mcode/model/capabilities.cxx
    mcode/model/http_client.cxx

    mcode/platform/seams.cxx
    mcode/platform/file_watcher.cxx
    mcode/platform/sandbox_windows.cxx
    mcode/platform/sandbox_linux.cxx
    mcode/platform/sandbox_macos.cxx

    mcode/events/bus.cxx

    mcode/cli/exec.cxx
    mcode/cli/skill_command.cxx

    mcode/skills/frontmatter.cxx
    mcode/skills/discovery.cxx
    mcode/skills/index.cxx
    mcode/skills/session_context.cxx

    mcode/instruct/chain.cxx

    mcode/eval/suite.cxx

    mcode/ext/api.cxx
    mcode/ext/api_events.cxx
    mcode/ext/api_invoke.cxx
    mcode/ext/api_internal.cxx
    mcode/ext/api_skill.cxx
    mcode/ext/api_mcp.cxx
    mcode/ext/api_mcp_store.cxx
    mcode/ext/api_gate.cxx
    mcode/ext/api_fs.cxx
    mcode/ext/api_cfg.cxx
    mcode/ext/api_cmd.cxx
    mcode/ext/api_timer.cxx
    mcode/ext/api_session.cxx
    mcode/ext/api_net.cxx
    mcode/ext/api_context.cxx
    mcode/ext/defer.cxx
    mcode/ext/notify.cxx
    mcode/ext/loader.cxx
    mcode/ext/lua_json.cxx
    mcode/ext/manifest.cxx

    mcode/fs/workspace.cxx
    mcode/fs/workspace_write.cxx

    mcode/perm/permission.cxx
    mcode/perm/rules.cxx
    mcode/perm/store.cxx
    mcode/perm/argv.cxx
    mcode/perm/floor.cxx
    mcode/perm/approval_terminal.cxx
    mcode/perm/approval_headless.cxx

    mcode/tools/session_reads.cxx
    mcode/tools/truncate.cxx
    mcode/tools/errors.cxx
    mcode/tools/tool_args.cxx
    mcode/tools/read_tool.cxx
    mcode/tools/write_tools.cxx
    mcode/tools/write_tools_common.cxx
    mcode/tools/write_tools_edit.cxx
    mcode/tools/search_tools.cxx
    mcode/tools/exec_tools.cxx
    mcode/tools/register.cxx

    mcode/net/sse.cxx
    mcode/net/http_client.cxx

    mcode/net/http_internal.cxx
    mcode/proc/process.cxx
    mcode/proc/session.cxx

    mcode/mcp/jsonrpc.cxx
    mcode/mcp/transport_stdio.cxx
    mcode/mcp/client.cxx
    mcode/mcp/supervisor.cxx
    mcode/mcp/source.cxx
    mcode/mcp/connect.cxx
    mcode/mcp/constants.cxx
    mcode/support/mcp_config.cxx

    mcode/agent/event_log.cxx
    mcode/agent/loop.cxx
    mcode/agent/loop_tools.cxx
    mcode/agent/loop_context.cxx

    mcode/tui/cell.cxx
    mcode/tui/tty.cxx
    mcode/tui/frame.cxx
    mcode/tui/theme.cxx
    mcode/tui/markdown.cxx
    mcode/tui/editor.cxx
    mcode/tui/approval_tui.cxx
    mcode/tui/palette.cxx
    mcode/tui/mention.cxx
    mcode/tui/render.cxx
    mcode/tui/render_view.cxx
    mcode/tui/transcript.cxx

    mcode/cli/repl.cxx
    mcode/cli/slash.cxx
)

target_include_directories(mcode_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

if(WIN32)
    target_sources(mcode_core PRIVATE mcode/tui/tty_windows.cxx)
else()
    target_sources(mcode_core PRIVATE mcode/tui/tty_posix.cxx)
endif()

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
        OpenSSL::SSL
        OpenSSL::Crypto
)

mcode_configure_target(mcode_core)

if(NOT WIN32 AND NOT APPLE)
    target_link_options(mcode_core PUBLIC "-Wl,--export-dynamic")
endif()

set(MCODE_BUNDLED_EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extensions")

add_executable(mcode main.cxx cli_commands.cxx cli_repl.cxx cli_repl_events.cxx
    cli_repl_plain.cxx cli_repl_view.cxx cli_session.cxx
    cli_approval.cxx smoke_cli_extensions.cxx smoke_luau.cxx)
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
