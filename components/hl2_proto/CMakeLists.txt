# The HL2's openHPSDR Protocol 1 wire format (hl2_proto.h, RFC 0014 S1). No REQUIRES: a pure codec, host-tested.
idf_component_register(
    SRCS "hl2_proto.cpp"
    INCLUDE_DIRS "include"
)
