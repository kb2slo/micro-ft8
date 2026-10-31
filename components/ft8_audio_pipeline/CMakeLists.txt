idf_component_register(
    SRCS "ft8_audio_pipeline.cpp" "ft8_slot_deficit.c" "ft8_sample_ring.c" "ft8_block_activity.c"
    INCLUDE_DIRS "include"
    REQUIRES decode_tx_state protocol debug_log ft8_lib esp_timer
)
