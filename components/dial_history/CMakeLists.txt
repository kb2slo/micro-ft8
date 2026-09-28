# Which frequency the radio's dial held, and when (dial_history.h). No
# dependencies: callers pass the time, so it is host-tested as is.
idf_component_register(
    SRCS "dial_history.cpp"
    INCLUDE_DIRS "include"
)
