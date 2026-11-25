add_executable(dual_instrument_sil
    dual_instrument_sil/main.cpp
)
target_link_libraries(dual_instrument_sil PRIVATE strata::services)

# Build-time single-source-of-truth strata-gen emission -> End-to-End Versal Mission Simulator
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(STRATA_VERSAL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_versal")
    set(STRATA_VERSAL_ICD_HPP "${STRATA_VERSAL_GEN_DIR}/strata_mission_icd.hpp")
    set(STRATA_VERSAL_HYP_HPP "${STRATA_VERSAL_GEN_DIR}/strata_hyp_tables.hpp")

    add_custom_command(
        OUTPUT "${STRATA_VERSAL_ICD_HPP}" "${STRATA_VERSAL_HYP_HPP}"
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/strata-gen/strata_gen.py
                ${CMAKE_SOURCE_DIR}/manifests/examples/versal_dual_instrument.toml
                --seds ${CMAKE_SOURCE_DIR}/manifests/examples/hyperspec_sensor_seds.xml
                --emit ${STRATA_VERSAL_GEN_DIR}
        DEPENDS
                ${CMAKE_SOURCE_DIR}/tools/strata-gen/strata_gen.py
                ${CMAKE_SOURCE_DIR}/manifests/examples/versal_dual_instrument.toml
                ${CMAKE_SOURCE_DIR}/manifests/examples/hyperspec_sensor_seds.xml
        COMMENT "Generating synchronized STRATA-20 ICD, Hypervisor, SV, TCL, XTCE & JSON artifacts"
    )

    add_executable(versal_mission_flight_sim
        versal_mission_flight_sim/main.cpp
        "${STRATA_VERSAL_ICD_HPP}"
        "${STRATA_VERSAL_HYP_HPP}"
    )
    target_include_directories(versal_mission_flight_sim PRIVATE "${STRATA_VERSAL_GEN_DIR}")
    target_link_libraries(versal_mission_flight_sim PRIVATE strata::services)
endif()
