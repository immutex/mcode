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
)

target_link_libraries(mcode_tests
    PRIVATE
        mcode_core
        Catch2::Catch2WithMain
)

mcode_configure_target(mcode_tests)

include(Catch)
catch_discover_tests(mcode_tests)
