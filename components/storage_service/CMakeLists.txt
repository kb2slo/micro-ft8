idf_component_register(
    SRCS "storage_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition freertos log joltwallet__littlefs
)
