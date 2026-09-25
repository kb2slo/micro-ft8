# Framing for the sidekick/main link (I28a, docs/architecture/sidekick.md (Port A)).
#
# No REQUIRES: the codec is deliberately dependency-free -- no ESP-IDF, no
# driver, nothing transport-specific. That is what lets the sidekick project
# compile the same file (docs/STYLE.md, Structure: "more than one idf.py
# project compiles it") and what keeps B47's UART-versus-I2C question open.
idf_component_register(
    SRCS "porta_proto.c"
    INCLUDE_DIRS "include"
)
