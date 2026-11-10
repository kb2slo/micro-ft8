# Sidekick UART link (Grove). Board pins are a seam (porta_board.h);
# porta_host_* actions are defined by the board's main.
idf_component_register(
    SRCS "porta.cpp" "porta_out_queue.c" "porta_fw_rx.c"
    INCLUDE_DIRS "include"
    REQUIRES porta_proto driver freertos log debug_log app_update esp_app_format esp_partition esp_timer mbedtls
)
