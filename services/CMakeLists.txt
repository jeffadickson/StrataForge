# Reusable Flight Services (Class B TCB + Class C Isolated Services)
# Subdirectories will register targets as services are implemented:
#   - services/class_b/cdh
#   - services/class_b/comms
#   - services/class_b/fdir_slot_mgr
#   - services/class_c/storage
#   - services/class_c/autonomy_sandbox

add_library(strata_services INTERFACE)
add_library(strata::services ALIAS strata_services)
target_link_libraries(strata_services INTERFACE strata::middleware)
