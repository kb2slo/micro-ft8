#pragma once

// app_serve.h -- what GET /<name> answers with (docs/architecture/sidekick.md (Web root)).
//
// The web app is part of this image: the web build's pack (app_pack.h), embedded as it was built
// and served from where it lies in mapped flash, with no copy and no file. The bootstrap pages
// under main/web/ are served the same way, from their embed. Nothing is read from the `web`
// filesystem.
//
// On the bench a pack can be pushed onto the running image (POST /api/app/push, app_push.h); it
// lies in the spare firmware slot and is served in the image's place, on that image only.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opens the image's pack, and a push onto this image if the spare slot holds one. Once, before any
// HTTP handler runs. Not fatal when it fails: the
// bootstrap pages and the API still serve, and the log says why the app does not.
void app_serve_init(void);

// A bootstrap page from main/web/, by its file name: its text, NUL-terminated, and its length
// without the NUL. False for a name that is not one.
bool app_serve_seed(const char *name, const char **text, size_t *len);

// The build id of the app being served: this image's own version, or a pushed pack's stamp. ""
// when no pack opens. /api/config says it as sk_app, and a page stamped otherwise offers Reload.
const char *app_serve_build(void);

// Stops serving a push, if there is one, and erases its header, so the image's own app is served
// again, now and after a restart. Call before anything else writes the spare slot (a firmware
// update stages its image there), from the HTTP server's task, which is the only one that reads
// the mapping.
void app_serve_drop_push(void);

// POST /api/app/push (token required), and GET /* : the served pack's asset of that name, else a
// bootstrap page, else 404. Register last, after every exact route. Requires
// httpd_config.uri_match_fn = httpd_uri_match_wildcard.
esp_err_t app_serve_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
