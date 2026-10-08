find_package(Catch2 CONFIG REQUIRED)

add_executable(mcode_tests
    test_main_config.cxx
    test_json.cxx
    test_text.cxx
    test_registry.cxx
    test_sse.cxx
    test_http_client.cxx
    test_workspace.cxx
    test_snapshot.cxx
    test_tools.cxx
    test_tools_bash.cxx
    test_permissions.cxx
    test_floor.cxx
    test_permission_store.cxx
    test_model_types.cxx
    test_provider.cxx
    test_dogfood_providers.cxx
    test_render.cxx
    test_model_client.cxx
    test_capabilities.cxx
    test_config_and_platform.cxx
    test_mcp_config.cxx
    test_event_log.cxx
    test_repetition_guard.cxx
    test_message_json.cxx
    test_loop.cxx
    test_loop_history.cxx
    test_loop_events.cxx
    test_loop_e2e.cxx
    test_events.cxx
    test_eval.cxx
    test_loader.cxx
    test_loader_failures.cxx
    test_lua_host.cxx
    test_manifest.cxx
    test_hooks.cxx
    test_hooks_safety.cxx
    test_api_surface.cxx
    test_skills.cxx
    test_instruct_chain.cxx
    test_mcp.cxx
    test_tui_frame.cxx
    test_tui_opening.cxx
    test_tui_cell.cxx
    test_tui_markdown.cxx
    test_tui_editor.cxx
    test_tui_paste.cxx
    test_tui_notify.cxx
    test_tui_repl.cxx
    test_tui_render.cxx
    test_tui_stream.cxx
    test_tui_commit.cxx
    test_tui_scroll.cxx
    test_tui_status.cxx
    test_tui_transcript.cxx
    test_cli_slash.cxx
    test_sandbox.cxx
    test_sandbox_egress.cxx
)

add_executable(mcp_echo fixtures/mcp_echo.cxx)
target_include_directories(mcp_echo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
mcode_configure_target(mcp_echo)
set_target_properties(mcp_echo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_link_libraries(mcode_tests
    PRIVATE
        mcode_core
        Catch2::Catch2WithMain
)

# The eval tests need the fixture repo's real path: they run from the build tree.
target_compile_definitions(mcode_tests PRIVATE
    MCODE_FIXTURE_REPO="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixture-repo"
    MCODE_FIXTURE_EXTENSIONS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/extensions"
    MCODE_EXTENSIONS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../extensions"
    MCODE_MCP_ECHO="$<TARGET_FILE:mcp_echo>")

mcode_configure_target(mcode_tests)

include(Catch)
# ADD_TAGS_AS_LABELS is required, not cosmetic: the CI matrix runs
# `ctest -L platform` to exercise the OS seams per platform, and without this the
# Catch2 tags never become ctest labels -- so the step found no tests at all and
# failed on two of three platforms while the suite itself was green.
catch_discover_tests(mcode_tests ADD_TAGS_AS_LABELS)

# The ConPTY screen gate: drives the real product under a pty and reads the
# screen a terminal would show.
#
# The unit suite cannot see placement. `visible_text` strips the positioning, so
# a header that scrolled off the top and a header that stayed produce the same
# string -- which is how a header that never appeared passed 118 `tui`
# assertions. The in-process half of this invariant is the screen model asserted
# by `test_tui_opening.cxx` and the `[screen]` case in `test_tui_commit.cxx`,
# which run on every platform; this is the half that needs a genuine console
# (`GetConsoleMode`/`ReadConsoleInputA` only engage on one).
#
# Windows only: ConPTY is a Win32 facility and pywinpty does not build
# elsewhere. The dependencies are Python packages, not Conan ones, so
# `scripts/bootstrap.ps1` and the Windows CI leg install them. A missing
# dependency FAILS the test with the install command rather than skipping it: a
# gate that reports success when it did not run is worse than no gate.
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter QUIET)

    if(NOT Python3_Interpreter_FOUND)
        message(WARNING
            "The tui-screen gate is not registered: no Python 3 interpreter. "
            "Install Python 3 and re-configure to get the ConPTY screen gate.")
    else()
        add_test(
            NAME tui-screen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tui_check/tui_screen.py"
                --exe "$<TARGET_FILE:mcode>"
        )

        set_tests_properties(tui-screen PROPERTIES
            LABELS "platform;tui"
            TIMEOUT 120
            ENVIRONMENT "PYTHONUNBUFFERED=1"
        )
    endif()
endif()
