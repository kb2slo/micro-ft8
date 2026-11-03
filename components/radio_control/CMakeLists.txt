# Backend-agnostic CAT control (docs/STYLE.md (Two boards, one source)).
# No dependency on any board's audio pipeline or USB host lifecycle -- only
# on cat_transport.h's four-function seam, which the board (poe_p4's
# qmx_transport.cpp) implements. That is what lets this file compile unchanged into more than one
# idf.py project (docs/STYLE.md's module-placement rule, the same
# reasoning porta_proto already uses).
idf_component_register(
    SRCS "radio_control.cpp"
         "cat_reply_parser.cpp"
         "radio_control_qmx.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_timer log dial_history rtc_core
)
