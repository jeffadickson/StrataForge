# STRATA-20 Flight C++20 Safety Profile (NASA NPR 7150.2D Class B / MISRA C++:2023 aligned)
# Applies zero-exception, zero-RTTI, strict warnings, and stack-usage emission.

add_library(strata_safety_flags INTERFACE)
add_library(strata::safety_flags ALIAS strata_safety_flags)

target_compile_features(strata_safety_flags INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(strata_safety_flags INTERFACE
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wundef
        -Wcast-align
    )

    if(STRATA_CLASS_B_STRICT)
        target_compile_options(strata_safety_flags INTERFACE -Werror)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(strata_safety_flags INTERFACE
            -fstack-usage
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
        )
    endif()

    if(STRATA_ENABLE_SANITIZERS AND NOT CMAKE_CROSSCOMPILING)
        target_compile_options(strata_safety_flags INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(strata_safety_flags INTERFACE
            -fsanitize=address,undefined
        )
    endif()
endif()
