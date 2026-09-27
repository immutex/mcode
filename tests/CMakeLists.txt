find_package(Catch2 CONFIG REQUIRED)

add_executable(mcode_tests
    test_main_config.cxx
    test_json.cxx
    test_text.cxx
    test_registry.cxx
    test_sse.cxx
    test_workspace.cxx
)

target_link_libraries(mcode_tests
    PRIVATE
        mcode_core
        Catch2::Catch2WithMain
)

mcode_configure_target(mcode_tests)

include(Catch)
catch_discover_tests(mcode_tests)
