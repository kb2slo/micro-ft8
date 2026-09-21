#pragma once

// Generated seed table (see main/CMakeLists.txt). web/ is the source of truth;
// this header is only the C shape of that table.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;      // path under the web FS root, e.g. "pairing.js"
    const char *embed;     // NUL-terminated EMBED_TXTFILES blob, start
    const char *embed_end; // one past EMBED_TXTFILES' own appended NUL --
                            // i.e. one past (last real byte + 1). Content
                            // length is embed_end - embed - 1; consumers
                            // must not serve/write the extra byte itself.
    // Size is embed_end - embed, computed where needed rather than stored.
    // Field-found 2026-09-20: a separately computed/cached size (this field
    // used to be `size_t size`, filled in by CMake's file(SIZE ...) at
    // *configure* time) can go stale -- CMake had no reason to reconfigure
    // just because an existing web/ file's content changed size, so this
    // silently stayed frozen through edit after edit while the embedded
    // blob itself kept getting correctly re-embedded on every build
    // (a separate, properly-tracked step) -- and hydrate_seeds() (web_fs.c)
    // wrote exactly that many, too few, bytes to the device's LittleFS every
    // time. _binary_X_end is the linker's own placement of the actual
    // embedded data; it cannot be stale relative to embed, because both
    // come from the same link.
} web_seed_file_t;

extern const web_seed_file_t web_seed_files[];
extern const size_t web_seed_files_count;

#ifdef __cplusplus
}
#endif
