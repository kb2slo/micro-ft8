idf_component_register(
    SRCS "decode_pipeline.cpp"
    INCLUDE_DIRS "include"
    REQUIRES decode_types ft8_lib autoseq callsign_hashtable decode_tx_state
             debug_log protocol rtc_core board_power adif
)
