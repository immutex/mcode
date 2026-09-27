include_guard(GLOBAL)

function(mcode_set_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /wd4127)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(warnings
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(warnings
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
            -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
            -Wduplicated-cond -Wduplicated-branches -Wlogical-op
        )
    else()
        set(warnings "")
    endif()

    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${warnings}>)

    if(MCODE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/WX>>
            $<$<COMPILE_LANGUAGE:CXX>:$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>>
        )
    endif()
endfunction()
