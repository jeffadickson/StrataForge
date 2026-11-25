# Reusable Flight Services
# Assurance criticality (NASA Class B/C/D) is assigned per-partition in mission.toml.
# Subdirectories will register targets as services are implemented:
#   - services/cdh
#   - services/comms
#   - services/fdir_slot_mgr
#   - services/storage
#   - services/autonomy_sandbox

add_library(strata_services INTERFACE)
add_library(strata::services ALIAS strata_services)
target_link_libraries(strata_services INTERFACE strata::middleware)
