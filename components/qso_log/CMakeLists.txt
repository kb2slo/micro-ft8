# The station's QSO log: the live write into the QSO store, and the QSOs tab's
# day-shaped reads of it (qso_log_store.h), with the row pager they still go through.
idf_component_register(
    SRCS "adif_logger.cpp" "qso_browse.cpp" "qso_log_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES adif qso_store storage_service rtc_core log esp_timer esp_hw_support
)
