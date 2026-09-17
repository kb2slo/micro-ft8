# The slot-loop/decode-pipeline TX-arming contract, as an owned interface
# rather than shared extern globals (RFC 0005 Phase 2). No dependency beyond
# <cstdint> -- whatever eventually shares the decode pipeline with poe_p4
# shares this unchanged too.
idf_component_register(
    SRCS "decode_tx_state.cpp"
    INCLUDE_DIRS "include"
)
