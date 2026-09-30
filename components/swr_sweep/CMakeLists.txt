# The SWR sweep's state machine (swr_sweep.h). It talks to the radio only through
# radio_swr_io_t (radio_control.h), so it is host-tested against a fake radio.
idf_component_register(
    SRCS "swr_sweep.cpp"
    INCLUDE_DIRS "include"
    REQUIRES radio_control
)
