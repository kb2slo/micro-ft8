# web/ is the single source of truth for the bootstrap pages (docs/architecture/sidekick.md (Web root)).
# Drop a file in web/ → it is embedded and served at /<filename> by the static
# handler (app_serve.c). No C table to edit. Templated routes (/, /update, …)
# still need handlers because their URLs are not the filenames.

set(WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/web")

# IDF processes this file once in script mode (requirements) where
# CONFIGURE_DEPENDS is illegal; again for the real project build where it is
# what makes "drop a file in web/" pick up without editing this list.
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB WEB_SEED_NAMES RELATIVE "${WEB_DIR}" "${WEB_DIR}/*")
else()
    file(GLOB WEB_SEED_NAMES CONFIGURE_DEPENDS RELATIVE "${WEB_DIR}" "${WEB_DIR}/*")
endif()
list(SORT WEB_SEED_NAMES)

set(WEB_EMBED_TXTFILES "")
set(WEB_SEED_EXTERNS "")
set(WEB_SEED_ENTRIES "")
set(WEB_SEED_COUNT 0)

foreach(name IN LISTS WEB_SEED_NAMES)
    if(IS_DIRECTORY "${WEB_DIR}/${name}")
        # skip directories
    elseif(name MATCHES "^\\." OR name MATCHES "~$")
        # skip editor junk
    else()
        list(APPEND WEB_EMBED_TXTFILES "web/${name}")
        string(MAKE_C_IDENTIFIER "${name}" sym)
        # No file(SIZE ...) here on purpose -- field-found 2026-09-20, that
        # was a configure-time-only read (same class of bug as git_version.
        # cmake's SHA/DIRTY) that went stale across edits and silently
        # truncated every seed page, identically for every client, no error
        # anywhere. Using EMBED_TXTFILES' own
        # _binary_X_end symbol instead means the size is embed_end - embed,
        # taken from the linker's actual placement of the data -- it cannot
        # be stale relative to the embed itself, because both come from the
        # same link, every time. See web_seed_table.h's own comment.
        string(APPEND WEB_SEED_EXTERNS
            "extern const char _binary_${sym}_start[] asm(\"_binary_${sym}_start\");\n"
            "extern const char _binary_${sym}_end[] asm(\"_binary_${sym}_end\");\n")
        string(APPEND WEB_SEED_ENTRIES
            "    {\"${name}\", _binary_${sym}_start, _binary_${sym}_end},\n")
        math(EXPR WEB_SEED_COUNT "${WEB_SEED_COUNT} + 1")
    endif()
endforeach()

if(WEB_SEED_COUNT EQUAL 0)
    message(FATAL_ERROR "sidekick/main/web/ has no seed files to embed")
endif()

set(WEB_SEED_GEN "${CMAKE_CURRENT_BINARY_DIR}/web_seed_table.c")
set(WEB_SEED_SRCS "main.c" "wifi_prov.c" "host_link.c" "event_state.c" "pairing_token.c" "pairing_http.c" "http_worker.c" "net_health.c"
                  "web_page.c"
                  "app_pack.c" "app_push.c" "app_serve.c"
                  "fw_policy.c" "fw_update.c")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(WRITE "${WEB_SEED_GEN}" "\
/* Generated from main/web/ — do not edit. */\n\
#include \"web_seed_table.h\"\n\
\n\
${WEB_SEED_EXTERNS}\n\
const web_seed_file_t web_seed_files[] = {\n\
${WEB_SEED_ENTRIES}\
};\n\
\n\
const size_t web_seed_files_count = ${WEB_SEED_COUNT};\n\
")
    list(APPEND WEB_SEED_SRCS "${WEB_SEED_GEN}")
endif()

idf_component_register(
    SRCS ${WEB_SEED_SRCS}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif lwip esp_event esp_http_server esp_http_client esp_app_format app_update porta_proto
             esp_driver_gpio esp_driver_uart esp_timer espressif__mdns mbedtls

    # Real files under web/, NUL-terminated by EMBED_TXTFILES. Placeholders are
    # {{name}}, not printf — CSS can say width:100%. The file list is the glob
    # above, not a second handwritten list here.
    EMBED_TXTFILES ${WEB_EMBED_TXTFILES}
)

# The web app, in this image (docs/architecture/sidekick.md (Web root)): the web
# build's pack, embedded as it is and served from where the linker put it.
#
# Nothing of the app is read when the build is configured. A target that runs
# on every build has the web build write the pack (webapp/tools/build.py
# --pack), and it rewrites the file only when its bytes change, so an
# unchanged app relinks nothing. The app is stamped with PROJECT_VER, the
# version this image reports: one string handed to both, so the app a sidekick
# serves and the sidekick itself say the same. The embed depends on that target, so the image
# cannot be linked with a pack older than the sources; and the pack's length
# at run time is the linker's own _binary_app_pack_end - _start (app_serve.c),
# for the reason the seed table's is (above). Needs Node and the minifier, once
# per clone: npm ci --prefix webapp/tools (CONTRIBUTING.md, Setup).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(APP_PACK "${CMAKE_CURRENT_BINARY_DIR}/app.pack")
    idf_build_get_property(python PYTHON)
    add_custom_target(app_pack
        COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/../../webapp/tools/build.py"
                --version "${PROJECT_VER}" --pack "${APP_PACK}"
        BYPRODUCTS "${APP_PACK}"
        COMMENT "Building the web app and its pack"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${APP_PACK}" BINARY DEPENDS app_pack)
endif()
