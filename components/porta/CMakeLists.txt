# Sidekick UART link (Grove). Forced into components/ because both the ADV
# and poe_p4 idf.py projects compile it (STYLE prong 1 / docs/STYLE.md (Two boards, one source)).
# Board pins and embedded-sidekick version comparison are a seam
# (porta_board.h); porta_host_* actions stay defined by each board's main.
idf_component_register(
    SRCS "porta.cpp"
    INCLUDE_DIRS "include"
    REQUIRES porta_proto driver freertos log debug_log
)
