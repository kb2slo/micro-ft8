idf_component_register(SRCS "bench_main.cpp" "bench_core.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES qso_store adif esp_timer)
