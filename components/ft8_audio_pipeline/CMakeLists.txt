idf_component_register(
    SRCS "ft8_audio_pipeline.cpp"
    INCLUDE_DIRS "include"
    REQUIRES decode_tx_state protocol debug_log ft8_lib
)
