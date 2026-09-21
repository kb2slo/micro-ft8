// LittleFS web root: mount, hydrate from EMBED_TXTFILES, load for HTTP.
// See web_fs.h and RFC 0004 §3.
//
// Seed membership comes from main/web/ via the generated web_seed_table — not
// a handwritten list in this file.

#include "web_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_littlefs.h"

#include "pairing_http.h"
#include "pairing_http_cap.h"
#include "web_seed_table.h"

static const char *TAG = "web_fs";

static bool s_ready;

static void abs_path(char *out, size_t out_len, const char *relpath)
{
    snprintf(out, out_len, "%s/%s", WEB_FS_ROOT, relpath);
}

// Relative path under the web root: no leading slash, no "..", no backslash.
static bool relpath_ok(const char *relpath)
{
    if (!relpath || relpath[0] == '\0' || relpath[0] == '/' || relpath[0] == '\\') {
        return false;
    }
    if (strstr(relpath, "..") != NULL || strchr(relpath, '\\') != NULL) {
        return false;
    }
    return true;
}

static bool file_matches_embed(const char *path, const char *embed, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != len) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char *buf = (char *)malloc(len);
    if (!buf) {
        fclose(f);
        return false;
    }
    const size_t n = fread(buf, 1, len, f);
    fclose(f);
    const bool ok = (n == len) && (memcmp(buf, embed, len) == 0);
    free(buf);
    return ok;
}

static esp_err_t write_seed_file(const char *path, const char *embed, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed", path);
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(embed, 1, len, f) != len) {
        ESP_LOGE(TAG, "write %s failed", path);
        fclose(f);
        return ESP_FAIL;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close %s failed", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t hydrate_seeds(bool force)
{
    int wrote = 0;
    for (size_t i = 0; i < web_seed_files_count; ++i) {
        const web_seed_file_t *s = &web_seed_files[i];
        // embed_end - embed, not a stored size -- see web_seed_table.h.
        const size_t len = (size_t)(s->embed_end - s->embed);
        char path[96];
        abs_path(path, sizeof(path), s->name);
        if (!force && file_matches_embed(path, s->embed, len)) {
            continue;
        }
        const esp_err_t err = write_seed_file(path, s->embed, len);
        if (err != ESP_OK) {
            return err;
        }
        ++wrote;
        ESP_LOGI(TAG, "%s %s (%u bytes)", force ? "rehydrated" : "hydrated", s->name,
                 (unsigned)len);
    }
    if (wrote == 0) {
        ESP_LOGI(TAG, "seed up to date (%u files)", (unsigned)web_seed_files_count);
    }
    return ESP_OK;
}

esp_err_t web_fs_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = WEB_FS_ROOT,
        .partition_label = "web",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info("web", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s (%u / %u bytes used)", WEB_FS_ROOT, (unsigned)used,
                 (unsigned)total);
    }

    err = hydrate_seeds(false);
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

bool web_fs_ready(void)
{
    return s_ready;
}

esp_err_t web_fs_rehydrate(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return hydrate_seeds(true);
}

static esp_err_t post_rehydrate(httpd_req_t *req)
{
    char discard[64];
    while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {
    }
    ESP_LOGI(TAG, "force rehydrate requested");
    const esp_err_t err = web_fs_rehydrate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "force rehydrate failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rehydrate failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "force rehydrate done");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_fs_register_rehydrate(httpd_handle_t server)
{
    static const httpd_uri_t route = {
        .uri = "/api/web/rehydrate",
        .method = HTTP_POST,
        .handler = post_rehydrate,
    };
    // B55: AP mode now registers this route too (RFC 0004 §1 -- it serves the
    // full application, not just static files), so both modes share the same
    // count.
    _Static_assert(PAIRING_ROUTES_WEB_FS_STATION == 2, "rehydrate + static, both modes");
    return pairing_http_register(server, &route, PAIRING_REQUIRED);
}

char *web_fs_load(const char *relpath, size_t *size_out)
{
    if (!s_ready || !relpath_ok(relpath)) {
        return NULL;
    }
    char path[96];
    abs_path(path, sizeof(path), relpath);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    // Bound runaway reads; seed pages are tens of KB and the app budget is
    // hundreds. A corrupt length should not allocate the partition size.
    if ((size_t)sz > 512u * 1024u) {
        ESP_LOGE(TAG, "%s too large (%ld)", path, sz);
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    if (size_out) {
        *size_out = (size_t)sz;
    }
    return buf;
}

static const char *mime_for(const char *relpath)
{
    const char *dot = strrchr(relpath, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

// B55: set once at registration time (web_fs_register_static_captive() vs.
// web_fs_register_static()) -- true under the AP's captive portal, where a
// missing file should bounce the client back to the portal rather than 404.
//
// Field-found 2026-09-20: httpd_register_err_handler(HTTPD_404_NOT_FOUND, ...)
// (wifi_prov.c's redirect_to_form) only fires when esp_http_server finds NO
// URI handler matching the request at all. Registering this file's "/*" GET
// route means there is *always* a match for any GET -- this handler itself --
// so a missing file calling httpd_resp_send_err() directly (as it always did)
// sends a bare 404 and redirect_to_form never runs. That is exactly what a
// live capture caught: iOS's captive-portal probe (GET /hotspot-detect.html)
// got a plain 404 instead of the expected redirect, so the sign-in sheet
// never popped -- matching docs/TEST_PLAN.md's long-standing "never once seen
// working" note for this exact row. redirect_to_form is not now dead code --
// a non-GET request to an unmatched path still has no handler at all (this
// route is GET-only) and still reaches it -- but the case that actually
// matters, a GET probe, has to be handled right here instead.
static bool s_redirect_missing_to_root = false;

// Field-found 2026-09-20, same session: an operator who reaches app.html and
// taps around still only ever sees the OS's "X" (disconnect) in the captive
// sheet, never "Done" (dismiss, stay joined). That is not a bug in our
// redirect -- it is the redirect working exactly as designed, forever, which
// is also exactly what stops the OS's own captive-portal probe from ever
// succeeding. Every real captive portal marks a session authenticated and
// then *stops* intercepting that specific probe, so the platform's next
// periodic recheck gets the exact response it is polling for and offers
// "Done" instead. Trigger: the first successful GET of app.html under this
// AP (the operator chose "Operate in AP mode") flips this, and the well-
// known probe paths below start answering for real instead of redirecting.
// Global, not per-client -- this device's AP allows at most two connections
// and is not a public hotspot, so a second client joining after the first
// already satisfied this will not see its own fresh captive-portal popup.
// A real limitation, accepted rather than solved with per-client tracking.
static bool s_captive_satisfied = false;

// Each OS polls a specific URL for a specific, exact response while its
// captive-portal sheet is open; anything else (including a 404) reads as
// "still captive". Text/bytes are copied from each vendor's own published
// captive-portal-detection behavior, not guessed.
static bool send_probe_success(httpd_req_t *req, const char *rel)
{
    if (strcmp(rel, "hotspot-detect.html") == 0 || strcmp(rel, "library/test/success.html") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        return true;
    }
    if (strcmp(rel, "generate_204") == 0 || strcmp(rel, "gen_204") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return true;
    }
    if (strcmp(rel, "ncsi.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Microsoft NCSI");
        return true;
    }
    if (strcmp(rel, "connecttest.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Microsoft Connect Test");
        return true;
    }
    if (strcmp(rel, "success.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "success\n");
        return true;
    }
    return false;
}

// Serves GET /<path> from the web FS when a more specific route did not match.
// Register last. Exact handlers (/api/..., /, /log) must be registered first
// and win because uri_match_wildcard does strcmp for patterns without '*'.
static esp_err_t get_static(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (!uri || uri[0] != '/') {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    // Strip query string if present.
    const char *q = strchr(uri, '?');
    size_t len = q ? (size_t)(q - uri) : strlen(uri);
    if (len < 2 || len >= 96) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    char rel[96];
    memcpy(rel, uri + 1, len - 1);
    rel[len - 1] = '\0';

    size_t body_len = 0;
    char *body = web_fs_load(rel, &body_len);
    if (!body) {
        if (s_redirect_missing_to_root) {
            if (s_captive_satisfied && send_probe_success(req, rel)) {
                return ESP_OK;
            }
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            return httpd_resp_send(req, NULL, 0);
        }
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    if (s_redirect_missing_to_root && strcmp(rel, "app.html") == 0) {
        s_captive_satisfied = true;
    }
    httpd_resp_set_type(req, mime_for(rel));
    // Seed updates on reflash/hydrate, or a desk_push bundle push; do not
    // keep a stale browser copy. no-cache (was here before) still permits a
    // conditional-revalidation cache entry in principle -- moot without an
    // ETag/Last-Modified for the browser to revalidate against, but no-store
    // says it plainly and is also one of the signals browsers check before
    // allowing a page into the back-forward cache, which no-cache does not
    // reliably block (see app.html's own pageshow handler for the other
    // half of that: a bfcache restore skips a network request altogether,
    // so no Cache-Control header sees it at all).
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, body, body_len);
    free(body);
    return err;
}

static esp_err_t register_static_common(httpd_handle_t server)
{
    static const httpd_uri_t route = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_static,
    };
    // Open: same as reading any other page on the device (RFC 0004 §7).
    return pairing_http_register(server, &route, PAIRING_OPEN);
}

esp_err_t web_fs_register_static(httpd_handle_t server)
{
    return register_static_common(server);
}

esp_err_t web_fs_register_static_captive(httpd_handle_t server)
{
    s_redirect_missing_to_root = true;
    return register_static_common(server);
}
