include_guard(GLOBAL)

function(mcode_set_runtime target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

function(mcode_set_platform_definitions target)
    target_compile_definitions(${target} PRIVATE
        $<$<PLATFORM_ID:Windows>:MCODE_OS_WINDOWS=1>
        $<$<PLATFORM_ID:Linux>:MCODE_OS_LINUX=1>
        $<$<PLATFORM_ID:Darwin>:MCODE_OS_MACOS=1>
    )

    if(WIN32)
        # Boost.Asio and Boost.Process warn and then assume Windows 7 without
        # this. ConPTY requires Windows 10 regardless.
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
endfunction()

function(mcode_set_dead_stripping target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-ffunction-sections;-fdata-sections>)
        target_link_options(${target} PRIVATE "-Wl,--gc-sections")
    endif()
endfunction()

function(mcode_configure_target target)
    mcode_set_warnings(${target})
    mcode_set_runtime(${target})
    mcode_set_platform_definitions(${target})
    mcode_set_dead_stripping(${target})
endfunction()
