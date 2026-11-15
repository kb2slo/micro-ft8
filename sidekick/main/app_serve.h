#pragma once

// app_serve.h -- what GET /<name> answers with (docs/architecture/sidekick.md (Web root)).
//
// The web app is part of this image: the web build's pack (app_pack.h), embedded as it was built
// and served from where it lies in mapped flash, with no copy and no file. The bootstrap pages
// under main/web/ are served the same way, from their embed. Nothing is read from the `web`
// filesystem.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opens the image's pack. Once, before any HTTP handler runs. Not fatal when it fails: the
// bootstrap pages and the API still serve, and the log says why the app does not.
void app_serve_init(void);

// A bootstrap page from main/web/, by its file name: its text, NUL-terminated, and its length
// without the NUL. False for a name that is not one.
bool app_serve_seed(const char *name, const char **text, size_t *len);

// GET /* : the pack's asset of that name, else a bootstrap page, else 404. Register last, after
// every exact route. Requires httpd_config.uri_match_fn = httpd_uri_match_wildcard.
esp_err_t app_serve_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
