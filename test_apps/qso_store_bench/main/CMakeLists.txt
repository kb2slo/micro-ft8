idf_component_register(SRCS "bench_main.cpp" "bench_core.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES qso_store adif esp_timer)

# An operator's own ADIF log in place of the synthetic ones: `-D BENCH_LOG=/path/to/log.adi` (README.md). Read
# from where it is, never copied into the tree: a logbook holds other stations' names and addresses.
if(BENCH_LOG)
    target_add_binary_data(${COMPONENT_TARGET} "${BENCH_LOG}" BINARY RENAME_TO bench_log)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE BENCH_LOG=1)
endif()
