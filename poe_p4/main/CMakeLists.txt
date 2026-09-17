idf_component_register(
    SRCS "main.cpp"
         "qmx_transport.cpp"
    INCLUDE_DIRS "."
    REQUIRES usb usb_host_cdc_acm esp_timer radio_control
)
