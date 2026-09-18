idf_component_register(
    SRCS "main.cpp"
         "storage/copy_to_sd.cpp"
         "storage/file_list_worker.cpp"
         "storage/station_save_worker.cpp"
         "audio/audio_source.cpp"
         "time/gps.cpp"
         "porta_board.cpp"
         "audio/stream_uac.cpp"
         "audio/dds_q15.cpp"
         "storage/station.cpp"
         "storage/station_save_queue.cpp"
         "band_config.cpp"
         "menu_model.cpp"
         "screen_model.cpp"
         "datetime_field.cpp"
         "usb_c_presence.cpp"
         "usb_c_presence_host.cpp"
         "storage/qso_browse.cpp"
         "storage/file_list.cpp"
         "storage/file_list_queue.cpp"
         "time/cts_time.cpp"
         "time/cts_ble.cpp"
         "time/external_rtc.cpp"
         "storage/storage_service.cpp"
    INCLUDE_DIRS "." "radio" "audio" "storage" "time"
    REQUIRES ui M5Unified M5GFX M5Cardputer board_cardputer_adv ft8_lib sidekick_flasher porta_proto porta radio_control decode_types
             decode_tx_state callsign_hashtable autoseq protocol debug_log ft8_audio_pipeline
             external_rtc storage_service board_power rtc_core adif decode_pipeline resample
             usb_host_uac usb_host_cdc_acm usb
             bt nvs_flash efuse esp_partition esp_hw_support
             driver esp_driver_i2c fatfs sdmmc joltwallet__littlefs
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
# components/porta references porta_board_* (defined only here). Without an
# explicit undefined, archive extraction can skip porta_board.cpp.obj when
# nothing in main/ calls those symbols directly (only porta_start/tick).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u porta_board_uart_pins"
    "-u porta_board_embedded_sidekick_version")
add_dependencies(${COMPONENT_LIB} microft8_build_identity)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_BINARY_DIR}/generated)
