find_package(Catch2 CONFIG REQUIRED)

add_executable(mcode_tests
    test_main_config.cxx
    test_json.cxx
    test_text.cxx
    test_registry.cxx
    test_sse.cxx
    test_workspace.cxx
    test_model_types.cxx
    test_provider.cxx
    test_dogfood_providers.cxx
    test_config_and_platform.cxx
    test_event_log.cxx
    test_events.cxx
    test_eval.cxx
)

target_link_libraries(mcode_tests
    PRIVATE
        mcode_core
        Catch2::Catch2WithMain
)

# The eval tests need the fixture repo's real path: they run from the build tree.
target_compile_definitions(mcode_tests PRIVATE
    MCODE_FIXTURE_REPO="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixture-repo")

mcode_configure_target(mcode_tests)

include(Catch)
catch_discover_tests(mcode_tests)
