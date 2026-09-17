# FT8's compressed-callsign hash table (RFC 0005 §5's shared-code goal): pure
# protocol support, used by both decode (resolving a hashed callsign) and TX
# (encoding our own messages) via ftx_message_decode()/encode()'s hash
# interface argument. No UI, no board-specific state -- only ft8_lib's own
# message.h/debug.h.
idf_component_register(
    SRCS "callsign_hashtable.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ft8_lib
)
