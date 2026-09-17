# Header-only, dependency-free (RFC 0005 §5) -- same shape as porta_proto:
# no REQUIRES, so anything that needs these plain structs can have them
# without pulling in a display stack, a USB stack, or ESP-IDF itself.
idf_component_register(
    INCLUDE_DIRS "include"
)
