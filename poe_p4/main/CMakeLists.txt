idf_component_register(
    SRCS "main.cpp"
         "qmx_transport.cpp"
         "decode_seams.cpp"
         "porta_board.cpp"
         "porta_host_stubs.cpp"
         "tx_scheduler.cpp"
         # Station.txt parse/serialize (host_mock tests the same unit).
         "../../main/storage/station.cpp"
         # I29: LittleFS storage_service (Station.txt persist).
         "../../main/storage/storage_service.cpp"
         # B71: ADIF QSO archive writer.
         "../../main/storage/adif_logger.cpp"
         # B73: Port A / phone Clear-all ADI delete.
         "../../main/storage/adif_delete.cpp"
         # B71: Log tab FILE_LIST/READ.
         "../../main/storage/qso_browse.cpp"
         "../../main/storage/file_list.cpp"
    INCLUDE_DIRS "."
                 "../../main/storage"
    REQUIRES usb usb_host_cdc_acm usb_host_uac esp_timer radio_control swr_sweep
             autoseq callsign_hashtable protocol ft8_audio_pipeline decode_pipeline
             rtc_core resample porta porta_proto debug_log
             storage_service adif joltwallet__littlefs esp_partition
    # decode_seams.cpp defines this board's side of the seams
    # (log_rxtx_line, ...) that shared components bare-forward-declare
    # rather than REQUIRES --
    # nothing in main.cpp itself calls them, so the linker's normal
    # reference-driven archive extraction never has a reason to pull these
    # objects in. WHOLE_ARCHIVE (same mechanism ESP-IDF's own components use
    # for registration-pattern code) includes every object in this library
    # unconditionally instead of relying on scan-order luck.
    WHOLE_ARCHIVE
)
