# Station.txt parse/serialize. No ESP-IDF; host_mock builds the same unit.
idf_component_register(
    SRCS "station.cpp"
    INCLUDE_DIRS "include"
)
