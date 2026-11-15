#pragma once

// app_pack.h -- the web app as one file: the reader (docs/roadmap/ota-updates.md, The app in the
// sidekick's image). webapp/tools/app_pack.py writes a pack from the built app; this reads one
// where it lies, copying nothing, so an asset is served straight from the bytes it was given.
// No ESP-IDF in it. Host-tested against the builder's own output (host_test_app_pack).
//
// Layout, little-endian, offsets from the pack's first byte:
//
//   0   8   "MFT8PACK"
//   8   2   format, APP_PACK_FORMAT
//   10  2   count of assets
//   12  4   length of the whole pack
//   16  48  build id, NUL-padded: what the page built into this pack calls itself
//   64      count entries of 64 bytes: name[56] NUL-padded, offset (4), size (4)
//   ...     the assets' bytes, as the build shipped them, gzipped or not
//
// A name is a file name, never a path: letters, digits, '.', '_' and '-', not starting with '.'.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PACK_MAGIC      "MFT8PACK"
#define APP_PACK_MAGIC_LEN  8u
#define APP_PACK_FORMAT     1u
#define APP_PACK_BUILD_MAX  48u  // NUL included
#define APP_PACK_NAME_MAX   56u  // NUL included
#define APP_PACK_HEAD_LEN   64u
#define APP_PACK_ENTRY_LEN  64u

typedef struct {
    const uint8_t *data;  // the pack, where the caller keeps it
    size_t         len;   // its own length, from its header
    uint16_t       count;
    char           build[APP_PACK_BUILD_MAX];
} app_pack_t;

// Checks all of `data` as a pack before anything is served from it: the header, every name, and
// that every asset lies inside the pack. `len` is how many bytes may be read; the pack may be
// shorter (a pack in a flash slot), never longer. Returns NULL with `out` filled, else which
// check failed, for a log line: "short", "magic", "format", "length", "build", "index", "name"
// or "bounds".
const char *app_pack_open(const uint8_t *data, size_t len, app_pack_t *out);

// The length a pack's header claims, so a caller that holds only the header knows how much to
// map. 0 if `head` (APP_PACK_HEAD_LEN bytes) is not a pack's.
size_t app_pack_claimed_len(const uint8_t *head);

// The asset called `name`: true with its bytes and size.
bool app_pack_find(const app_pack_t *pack, const char *name, const uint8_t **bytes, size_t *size);

// Asset `i` of `pack->count`, in the builder's order (sorted by name).
bool app_pack_entry(const app_pack_t *pack, uint16_t i, const char **name, const uint8_t **bytes, size_t *size);

#ifdef __cplusplus
}
#endif
