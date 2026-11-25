# Reusable Flight Services
# Assurance criticality (NASA Class B/C/D) is assigned per-partition in mission.toml.

add_library(strata_services INTERFACE)
add_library(strata::services ALIAS strata_services)

target_include_directories(strata_services INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cdh/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/comms/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fdir_slot_mgr/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/storage/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/autonomy_sandbox/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(strata_services INTERFACE strata::middleware)
