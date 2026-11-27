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

# Discover .NET 11 SDK and build unified multithreaded C# utilities (`strata-tools` + XML docs)
find_program(DOTNET_EXECUTABLE dotnet
    HINTS "$ENV{HOME}/.dotnet" "$ENV{DOTNET_ROOT}"
    NO_DEFAULT_PATH
)
if(NOT DOTNET_EXECUTABLE)
    find_program(DOTNET_EXECUTABLE dotnet)
endif()

if(DOTNET_EXECUTABLE)
    get_filename_component(STRATA_DOTNET_ROOT "${DOTNET_EXECUTABLE}" DIRECTORY)
    set(STRATA_TOOLS_OUT_DIR "${CMAKE_BINARY_DIR}/dotnet_tools")
    set(STRATA_TOOLS_BIN "${STRATA_TOOLS_OUT_DIR}/strata-tools")
    set(STRATA_TOOLS_CMD ${CMAKE_COMMAND} -E env "DOTNET_ROOT=${STRATA_DOTNET_ROOT}" "${STRATA_TOOLS_BIN}")

    file(GLOB_RECURSE STRATA_DOTNET_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/dotnet/*.cs"
        "${CMAKE_SOURCE_DIR}/dotnet/*.csproj"
        "${CMAKE_SOURCE_DIR}/dotnet/Directory.Build.props"
    )

    add_custom_command(
        OUTPUT "${STRATA_TOOLS_BIN}"
        COMMAND ${CMAKE_COMMAND} -E env "DOTNET_ROOT=${STRATA_DOTNET_ROOT}"
                "${DOTNET_EXECUTABLE}" build
                "${CMAKE_SOURCE_DIR}/dotnet/StrataForge.Cli/StrataForge.Cli.csproj"
                -c Release -o "${STRATA_TOOLS_OUT_DIR}" --nologo
        DEPENDS ${STRATA_DOTNET_SOURCES}
        COMMENT "Building .NET 11 multithreaded C# utilities (strata-tools + XML docs)"
    )
    add_custom_target(strata_dotnet_tools ALL DEPENDS "${STRATA_TOOLS_BIN}")
endif()

if(STRATA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(STRATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
