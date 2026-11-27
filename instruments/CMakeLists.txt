add_library(strata_instruments INTERFACE)
add_library(strata::instruments ALIAS strata_instruments)

target_include_directories(strata_instruments INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(strata_instruments INTERFACE
    strata::middleware
    strata::services
    strata::safety_flags
)
