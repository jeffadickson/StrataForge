# Middleware Core & Primitive Unit Tests
add_executable(test_abi_and_types unit/test_abi_and_types.cpp)
target_link_libraries(test_abi_and_types PRIVATE strata::middleware)
add_test(NAME strata.unit.abi_and_types COMMAND test_abi_and_types)

add_executable(test_core_and_fsm unit/test_core_and_fsm.cpp)
target_link_libraries(test_core_and_fsm PRIVATE strata::middleware)
add_test(NAME strata.unit.core_and_fsm COMMAND test_core_and_fsm)

add_executable(test_ipc_primitives unit/test_ipc_primitives.cpp)
target_link_libraries(test_ipc_primitives PRIVATE strata::middleware)
add_test(NAME strata.unit.ipc_primitives COMMAND test_ipc_primitives)

add_executable(test_zero_copy_data_plane unit/test_zero_copy_data_plane.cpp)
target_link_libraries(test_zero_copy_data_plane PRIVATE strata::middleware)
add_test(NAME strata.unit.zero_copy_data_plane COMMAND test_zero_copy_data_plane)

add_executable(test_hw_and_runtime unit/test_hw_and_runtime.cpp)
target_link_libraries(test_hw_and_runtime PRIVATE strata::middleware)
add_test(NAME strata.unit.hw_and_runtime COMMAND test_hw_and_runtime)

# Flight Services Unit & Fault-Injection Tests
add_executable(test_comms_ccsds_sdls unit/test_comms_ccsds_sdls.cpp)
target_link_libraries(test_comms_ccsds_sdls PRIVATE strata::services)
add_test(NAME strata.unit.comms_ccsds_sdls COMMAND test_comms_ccsds_sdls)

add_executable(test_cdh_services unit/test_cdh_services.cpp)
target_link_libraries(test_cdh_services PRIVATE strata::services)
add_test(NAME strata.unit.cdh_services COMMAND test_cdh_services)

add_executable(test_fdir_and_failover unit/test_fdir_and_failover.cpp)
target_link_libraries(test_fdir_and_failover PRIVATE strata::services)
add_test(NAME strata.unit.fdir_and_failover COMMAND test_fdir_and_failover)

add_executable(test_storage_and_autonomy unit/test_storage_and_autonomy.cpp)
target_link_libraries(test_storage_and_autonomy PRIVATE strata::services)
add_test(NAME strata.unit.storage_and_autonomy COMMAND test_storage_and_autonomy)

# Deterministic Seeded Rule-Based Fuzzer
add_executable(test_rule_fuzzer rule_fuzzer/test_rule_fuzzer.cpp)
target_link_libraries(test_rule_fuzzer PRIVATE strata::services)
add_test(NAME strata.fuzzer.rule_engine COMMAND test_rule_fuzzer)

# Single-Source-of-Truth Manifest Static Verification
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME strata.manifest.versal_dual_instrument
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/strata-gen/strata_gen.py
                ${CMAKE_SOURCE_DIR}/manifests/examples/versal_dual_instrument.toml
    )
    add_test(
        NAME strata.manifest.kintex_dual_instrument
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/strata-gen/strata_gen.py
                ${CMAKE_SOURCE_DIR}/manifests/examples/kintex_dual_instrument.toml
    )
    add_test(
        NAME strata.requirements.strictdoc_graph
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/check_sdoc.py
                ${CMAKE_SOURCE_DIR}/requirements
                ${CMAKE_SOURCE_DIR}
    )
endif()
