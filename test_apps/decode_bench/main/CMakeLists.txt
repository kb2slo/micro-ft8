idf_component_register(SRCS "bench_main.c" "bench_core.c"
                            "../../../components/decode_pipeline/decode_subtract.c"
                       INCLUDE_DIRS "."
                       REQUIRES ft8_lib esp_timer)

target_include_directories(${COMPONENT_TARGET} PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/../../../components/decode_pipeline/include")

# Corpus slots come from tools/make_slots.py and are never committed (RFC 0006 P1).
set(slot_dir "${CMAKE_CURRENT_LIST_DIR}/../data")
foreach(index 0 1 2 3 4)
    if(NOT EXISTS "${slot_dir}/slot_${index}.raw")
        message(FATAL_ERROR "Missing ${slot_dir}/slot_${index}.raw: run tools/make_slots.py first")
    endif()
    target_add_binary_data(${COMPONENT_TARGET} "${slot_dir}/slot_${index}.raw" BINARY)
endforeach()
target_add_binary_data(${COMPONENT_TARGET} "${slot_dir}/slots.txt" TEXT)
