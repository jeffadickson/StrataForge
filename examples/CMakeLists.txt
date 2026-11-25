add_executable(dual_instrument_sil
    dual_instrument_sil/main.cpp
)
target_link_libraries(dual_instrument_sil PRIVATE strata::services)
