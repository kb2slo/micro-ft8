// The `web` filesystem: mount, hydrate from EMBED_TXTFILES, format. Nothing is served from it
// (app_serve.c); it is where the bundle API still writes. See web_fs.h and
// docs/architecture/sidekick.md (Web root).
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
        // embed_end - embed, not a stored size -- see web_seed_table.h. Minus
        // one for EMBED_TXTFILES' own appended NUL terminator (CMakeLists.txt's
        // EMBED_TXTFILES comment): that byte is real, placed by the linker
        // right after the file's actual bytes, so embed_end includes it --
        // but it is not part of the file's content and must not be written to
        // LittleFS or served over HTTP. Field-found 2026-09-21: pairing.js
        // parses fine as a file (curl showed intact content) but the served
        // copy carried this trailing '\0', which is invalid inside a JS
        // string/token and threw "SyntaxError: Invalid character: '\0'" in
        // every browser, breaking pairing (and the whole app.html script,
        // since a top-level parse error stops the entire file) under AP mode
        // exactly the same as it would have under station mode -- this was
        // never mode-specific, just never hit on a seed file parsed strictly
        // enough to notice before.
        const size_t len = (size_t)(s->embed_end - s->embed) - 1;
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

esp_err_t web_fs_format(void)
{
    // esp_littlefs_format() formats a mounted partition in place. The seeds
    // are not rewritten here: a restart follows, and web_fs_init() hydrates an
    // empty filesystem.
    const esp_err_t err = esp_littlefs_format("web");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    }
    return err;
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
    _Static_assert(PAIRING_ROUTES_WEB_FS == 1, "rehydrate, both modes");
    return pairing_http_register(server, &route, PAIRING_REQUIRED);
}
