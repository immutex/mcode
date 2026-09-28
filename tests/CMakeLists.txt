find_package(Catch2 CONFIG REQUIRED)

add_executable(mcode_tests
    test_main_config.cxx
    test_json.cxx
    test_text.cxx
    test_registry.cxx
    test_sse.cxx
    test_http_client.cxx
    test_workspace.cxx
    test_model_types.cxx
    test_provider.cxx
    test_dogfood_providers.cxx
    test_config_and_platform.cxx
    test_event_log.cxx
    test_loop.cxx
    test_events.cxx
    test_eval.cxx
    test_loader.cxx
    test_loader_failures.cxx
    test_manifest.cxx
    test_hooks.cxx
)

target_link_libraries(mcode_tests
    PRIVATE
        mcode_core
        Catch2::Catch2WithMain
)

# The eval tests need the fixture repo's real path: they run from the build tree.
target_compile_definitions(mcode_tests PRIVATE
    MCODE_FIXTURE_REPO="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixture-repo"
    MCODE_FIXTURE_EXTENSIONS="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/extensions"
    MCODE_EXTENSIONS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../extensions")

mcode_configure_target(mcode_tests)

include(Catch)
# ADD_TAGS_AS_LABELS is required, not cosmetic: the CI matrix runs
# `ctest -L platform` to exercise the OS seams per platform, and without this the
# Catch2 tags never become ctest labels -- so the step found no tests at all and
# failed on two of three platforms while the suite itself was green.
catch_discover_tests(mcode_tests ADD_TAGS_AS_LABELS)
