cmake_minimum_required(VERSION 3.25)

project(StrataForge
    VERSION 0.1.0
    DESCRIPTION "STRATA-20: Bare-metal C++20 flight instrument middleware and shared-FPGA hypervisor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(STRATA_BUILD_TESTS "Build StrataForge unit and SIL tests" ON)
option(STRATA_BUILD_EXAMPLES "Build StrataForge reference examples" ON)
option(STRATA_ENABLE_SANITIZERS "Enable ASan/UBSan on host SIL builds" OFF)
option(STRATA_CLASS_B_STRICT "Enforce Class B strict warnings-as-errors and stack-usage checks" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(strata_safety_flags)

add_subdirectory(hypervisor)
add_subdirectory(middleware)
add_subdirectory(services)
add_subdirectory(instruments)

if(STRATA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(STRATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
