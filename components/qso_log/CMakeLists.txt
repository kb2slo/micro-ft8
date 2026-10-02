# The ADIF QSO archive: live write, delete, and the Log tab's day-file browse
# and list pagers.
idf_component_register(
    SRCS "adif_logger.cpp" "adif_delete.cpp" "qso_browse.cpp" "file_list.cpp"
    INCLUDE_DIRS "include"
    REQUIRES adif storage_service rtc_core log
)
