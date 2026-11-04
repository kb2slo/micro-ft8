# The HL2's openHPSDR Protocol 1 wire format (hl2_proto.h, row mg9c, docs/roadmap/hl2-support.md). No REQUIRES: a pure codec, host-tested.
idf_component_register(
    SRCS "hl2_proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES iq_frontend  # hl2_rx_frontend.h, header only
)
