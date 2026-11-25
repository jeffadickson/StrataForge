add_library(strata_hyp_abi INTERFACE)
add_library(strata::hyp_abi ALIAS strata_hyp_abi)

target_include_directories(strata_hyp_abi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(strata_hyp_abi INTERFACE strata::safety_flags)
