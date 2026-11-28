# The heard ring: every decode in RAM, found by sender call (heard_ring.h). No dependencies: the caller
# supplies the memory, so it is host-tested as is.
idf_component_register(
    SRCS "heard_ring.cpp"
    INCLUDE_DIRS "include"
)
