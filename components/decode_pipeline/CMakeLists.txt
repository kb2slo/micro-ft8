idf_component_register(
    SRCS "decode_pipeline.cpp" "decode_subtract.c" "decode_core_busy.c"
    INCLUDE_DIRS "include"
    REQUIRES decode_types ft8_lib autoseq callsign_hashtable decode_tx_state
             debug_log protocol rtc_core adif dial_history
)

# B89: decode loop at -O2, as ft8_lib (docs/architecture/decode.md, Build).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
