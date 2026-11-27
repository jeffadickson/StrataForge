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

# Bare-Metal Type-1 Hypervisor Core & Multi-Arch Backend Tests
add_executable(test_hypervisor unit/test_hypervisor.cpp)
target_link_libraries(test_hypervisor PRIVATE strata::middleware)
add_test(NAME strata.unit.hypervisor COMMAND test_hypervisor)

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

add_executable(test_extended_services unit/test_extended_services.cpp)
target_link_libraries(test_extended_services PRIVATE strata::services)
add_test(NAME strata.unit.extended_services COMMAND test_extended_services)

add_executable(test_pus_c_and_nv_log unit/test_pus_c_and_nv_log.cpp)
target_link_libraries(test_pus_c_and_nv_log PRIVATE strata::services)
add_test(NAME strata.unit.pus_c_and_nv_log COMMAND test_pus_c_and_nv_log)

add_executable(test_crypto_primitives unit/test_crypto_primitives.cpp)
target_link_libraries(test_crypto_primitives PRIVATE strata::services)
add_test(NAME strata.unit.crypto_primitives COMMAND test_crypto_primitives)

add_executable(test_mock_instrument unit/test_mock_instrument.cpp)
target_link_libraries(test_mock_instrument PRIVATE strata::instruments)
add_test(NAME strata.unit.mock_instrument COMMAND test_mock_instrument)

# Deterministic Seeded Rule-Based Fuzzer
add_executable(test_rule_fuzzer rule_fuzzer/test_rule_fuzzer.cpp)
target_link_libraries(test_rule_fuzzer PRIVATE strata::services)
add_test(NAME strata.fuzzer.rule_engine COMMAND test_rule_fuzzer)

# Single-Source-of-Truth Manifest Static Verification, Multi-Domain Codegen & StrictDoc Graph (.NET 11)
if(DOTNET_EXECUTABLE)
    add_test(
        NAME strata.manifest.versal_dual_instrument
        COMMAND ${STRATA_TOOLS_CMD} gen
                ${CMAKE_SOURCE_DIR}/manifests/examples/versal_dual_instrument.toml
    )
    add_test(
        NAME strata.manifest.kintex_dual_instrument
        COMMAND ${STRATA_TOOLS_CMD} gen
                ${CMAKE_SOURCE_DIR}/manifests/examples/kintex_dual_instrument.toml
    )
    add_test(
        NAME strata.codegen.multi_domain_and_xtce
        COMMAND ${STRATA_TOOLS_CMD} test gen ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME strata.requirements.strictdoc_graph
        COMMAND ${STRATA_TOOLS_CMD} check-sdoc
                ${CMAKE_SOURCE_DIR}/requirements
                ${CMAKE_SOURCE_DIR}
    )
endif()

# Verilator Live SystemVerilog RTL Co-Simulation (STRATA-VER-003, STRATA-RTL-001..005)
find_package(verilator QUIET HINTS $ENV{VERILATOR_ROOT})
if(verilator_FOUND)
    add_library(strata_verilated_rtl STATIC)
    verilate(strata_verilated_rtl
        SOURCES
            ${CMAKE_SOURCE_DIR}/rtl/common/scsr_slave.sv
            ${CMAKE_SOURCE_DIR}/rtl/common/sg_dma_shaper.sv
            ${CMAKE_SOURCE_DIR}/rtl/versal/versal_platform_shell.sv
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/rtl/common
        TOP_MODULE versal_platform_shell
        PREFIX Vversal_platform_shell
        VERILATOR_ARGS -Wall -GMANIFEST_HASH_LO=32'h9243B827 -GMANIFEST_HASH_HI=32'h14A6F9D1
    )
    verilate(strata_verilated_rtl
        SOURCES
            ${CMAKE_SOURCE_DIR}/rtl/common/scsr_slave.sv
            ${CMAKE_SOURCE_DIR}/rtl/common/sg_dma_shaper.sv
            ${CMAKE_SOURCE_DIR}/rtl/kintex/kintex_platform_shell.sv
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/rtl/common
        TOP_MODULE kintex_platform_shell
        PREFIX Vkintex_platform_shell
        VERILATOR_ARGS -Wall -GMANIFEST_HASH_LO=32'h9243B827 -GMANIFEST_HASH_HI=32'h14A6F9D1
    )

    add_executable(test_verilator_scsr_dma verilator_hil/test_verilator_scsr_dma.cpp)
    target_include_directories(test_verilator_scsr_dma PRIVATE
        ${CMAKE_SOURCE_DIR}/middleware/include
        ${CMAKE_SOURCE_DIR}/hypervisor/include
    )
    target_link_libraries(test_verilator_scsr_dma PRIVATE strata_verilated_rtl)
    add_test(NAME strata.hil.verilator_scsr_dma COMMAND test_verilator_scsr_dma)

    add_executable(test_verilator_mission_loop verilator_hil/test_verilator_mission_loop.cpp)
    target_include_directories(test_verilator_mission_loop PRIVATE
        ${CMAKE_SOURCE_DIR}/middleware/include
        ${CMAKE_SOURCE_DIR}/hypervisor/include
        ${CMAKE_SOURCE_DIR}/services/fdir_slot_mgr/include
    )
    target_link_libraries(test_verilator_mission_loop PRIVATE strata_verilated_rtl)
    add_test(NAME strata.hil.verilator_mission_loop COMMAND test_verilator_mission_loop)
endif()

# Ground Operations CLI, StrataGround GDS/I&T, Bare-Metal Cross Proofs & Live QEMU EL2 (.NET 11)
if(DOTNET_EXECUTABLE)
    add_test(
        NAME strata.ops.ground_cli_roundtrip
        COMMAND ${STRATA_TOOLS_CMD} test ops ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME strata.ground.gds_end_to_end
        COMMAND ${STRATA_TOOLS_CMD} test ground ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME strata.cross.baremetal_aarch64_and_r5
        COMMAND ${STRATA_TOOLS_CMD} check-cross ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME strata.qemu.aarch64_el2_execution
        COMMAND ${STRATA_TOOLS_CMD} check-qemu-el2 ${CMAKE_SOURCE_DIR}
    )
endif()

# End-to-End Multi-Partition Mission Flight Integration, Live UDP Ground Loop & Static Safety Audit
if(TARGET versal_mission_flight_sim AND TARGET kintex_mission_flight_sim AND TARGET live_ground_sil)
    add_test(NAME strata.integration.versal_mission_flight_sim COMMAND versal_mission_flight_sim)
    add_test(NAME strata.integration.kintex_mission_flight_sim COMMAND kintex_mission_flight_sim)
    if(DOTNET_EXECUTABLE)
        add_test(
            NAME strata.ground.live_udp_flight_loop
            COMMAND ${STRATA_TOOLS_CMD} test live-udp
                    ${CMAKE_SOURCE_DIR}
                    $<TARGET_FILE:live_ground_sil>
        )
        add_test(
            NAME strata.audit.flight_safety_stack_and_heap
            COMMAND ${STRATA_TOOLS_CMD} check-safety
                    ${CMAKE_BINARY_DIR}
                    --max-flight-stack-bytes 4096
                    --skip-elf-on-asan
                    --elf $<TARGET_FILE:dual_instrument_sil>
                    --elf $<TARGET_FILE:versal_mission_flight_sim>
                    --elf $<TARGET_FILE:kintex_mission_flight_sim>
                    --elf $<TARGET_FILE:live_ground_sil>
        )
    endif()
endif()
