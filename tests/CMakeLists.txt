add_executable(test_abi_and_types
    unit/test_abi_and_types.cpp
)
target_link_libraries(test_abi_and_types PRIVATE strata::middleware)

add_test(NAME strata.unit.abi_and_types COMMAND test_abi_and_types)

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
endif()
