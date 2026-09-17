# FT8 auto-sequencer/QSO state machine (RFC 0005 §5's shared-code goal).
# Depends only on decode_types.h for UiRxLine -- no board, no radio backend,
# no display. ADIF logging is an injected callback (autoseq_set_adif_callback),
# not a direct call, so this component has no storage dependency either. That
# is what lets host_mock compile it unchanged on the host (host_mock's own
# INCLUDES point straight at this component's headers) and what will let
# poe_p4 share it unchanged once the decode pipeline that feeds it is ported.
idf_component_register(
    SRCS "autoseq.cpp"
    INCLUDE_DIRS "include"
    REQUIRES decode_types esp_common log esp_timer
)
