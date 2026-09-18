idf_component_register(
    SRCS "main.cpp"
         "qmx_transport.cpp"
         "external_rtc_stub.cpp"
         "board_power_stub.cpp"
         "decode_seams.cpp"
         "porta_board.cpp"
         "porta_host_stubs.cpp"
         "tx_scheduler.cpp"
         # Shared Station.txt parse/serialize (same unit ADV + host_mock use).
         "../../main/storage/station.cpp"
         # I29: same LittleFS storage_service as ADV (Station.txt persist).
         "../../main/storage/storage_service.cpp"
         # B71: shared ADIF QSO archive writer (same unit ADV uses).
         "../../main/storage/adif_logger.cpp"
         # B73: Port A / phone Clear-all ADI delete (shared with ADV).
         "../../main/storage/adif_delete.cpp"
         # B71: Log tab FILE_LIST/READ (same parse/list units as ADV).
         "../../main/storage/qso_browse.cpp"
         "../../main/storage/file_list.cpp"
    INCLUDE_DIRS "."
                 "../../main/storage"
    REQUIRES usb usb_host_cdc_acm usb_host_uac esp_timer radio_control
             autoseq callsign_hashtable protocol ft8_audio_pipeline decode_pipeline
             rtc_core external_rtc board_power resample porta porta_proto debug_log
             storage_service adif fatfs sdmmc joltwallet__littlefs esp_partition
    # decode_seams.cpp/board_power_stub.cpp/external_rtc_stub.cpp define this
    # board's side of seams (ui_push_waterfall_row, board_power_halted, ...)
    # that shared components bare-forward-declare rather than REQUIRES --
    # nothing in main.cpp itself calls them, so the linker's normal
    # reference-driven archive extraction never has a reason to pull these
    # objects in. WHOLE_ARCHIVE (same mechanism ESP-IDF's own components use
    # for registration-pattern code) includes every object in this library
    # unconditionally instead of relying on scan-order luck.
    WHOLE_ARCHIVE
)
