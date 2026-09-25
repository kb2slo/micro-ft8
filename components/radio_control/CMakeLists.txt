# Backend-agnostic QMX/QDX CAT control (RFC 0005 §5's shared-code goal).
# No dependency on any board's audio pipeline or USB host lifecycle -- only
# on cat_transport.h's four-function seam, which each board (main/'s
# stream_uac.cpp for the ADV, poe_p4's own cat_transport.cpp) implements
# itself. That is what lets this file compile unchanged into more than one
# idf.py project (docs/STYLE.md's module-placement rule, the same
# reasoning porta_proto already uses).
idf_component_register(
    SRCS "radio_control.cpp"
         "radio_control_qmx.cpp"
         "radio_control_qdx.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log
)
