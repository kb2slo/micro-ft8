#pragma once

// web_fs.h -- the `web` filesystem (docs/architecture/sidekick.md (Web root)).
//
// Partition label `web` mounts at WEB_FS_ROOT, and the files under main/web/ are hydrated onto it
// at boot. Nothing is served from it: the app and the bootstrap pages are served from the image
// (app_serve.h). It is still mounted because the bundle API stages and promotes onto it
// (web_bundle.h), which a page from before the app shipped in the image may still call.
//
// Force re-hydrate is web_fs_rehydrate(), exposed as token-guarded POST /api/web/rehydrate.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_FS_ROOT "/web"

// Mount LittleFS on partition "web", format if empty/corrupt, then hydrate
// seed files from the firmware embed. Safe to call once at boot before any
// HTTP handler runs.
esp_err_t web_fs_init(void);

// True after a successful web_fs_init().
bool web_fs_ready(void);

// Erases the whole web filesystem and leaves it empty; the next web_fs_init() hydrates it again.
// Mounted or not.
esp_err_t web_fs_format(void);

// Rewrite every seed file from the firmware embed, even if the on-disk copy
// already matches. Used for recovery; ordinary boots use web_fs_init()'s
// compare-and-write.
esp_err_t web_fs_rehydrate(void);

// POST /api/web/rehydrate — PAIRING_REQUIRED.
esp_err_t web_fs_register_rehydrate(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
