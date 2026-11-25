add_library(strata_middleware INTERFACE)
add_library(strata::middleware ALIAS strata_middleware)

target_include_directories(strata_middleware INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(strata_middleware INTERFACE
    strata::safety_flags
    strata::hyp_abi
)
