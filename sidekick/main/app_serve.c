// What GET /<name> answers with: the web app from this image's pack, and the bootstrap pages from
// their embed. See app_serve.h and docs/architecture/sidekick.md (Web root).

#include "app_serve.h"

#include <string.h>

#include "esp_log.h"

#include "app_pack.h"
#include "pairing_http.h"
#include "pairing_http_cap.h"
#include "web_seed_table.h"

static const char *TAG = "app_serve";

// The pack, embedded by main/CMakeLists.txt as the web build wrote it. Its length is the linker's
// own, end minus start, never a size read when the build was configured (web_seed_table.h says
// what that cost once).
extern const uint8_t image_pack_start[] asm("_binary_app_pack_start");
extern const uint8_t image_pack_end[] asm("_binary_app_pack_end");

static app_pack_t s_image;
static bool       s_image_ok;

void app_serve_init(void)
{
    const size_t len = (size_t)(image_pack_end - image_pack_start);
    const char  *bad = app_pack_open(image_pack_start, len, &s_image);
    s_image_ok = bad == NULL;
    if (bad) {
        ESP_LOGE(TAG, "this image's app pack does not open (%s, %u bytes): the app will 404", bad, (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "app %s: %u assets, %u bytes, served from this image", s_image.build, (unsigned)s_image.count,
             (unsigned)s_image.len);
}

bool app_serve_seed(const char *name, const char **text, size_t *len)
{
    for (size_t i = 0; i < web_seed_files_count; ++i) {
        const web_seed_file_t *s = &web_seed_files[i];
        if (strcmp(s->name, name) == 0) {
            *text = s->embed;
            // EMBED_TXTFILES appends a NUL, which embed_end counts and the file does not hold
            // (web_seed_table.h). Served, it is an invalid character at the end of a script.
            *len = (size_t)(s->embed_end - s->embed) - 1;
            return true;
        }
    }
    return false;
}

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
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

// Serves GET /<name> when a more specific route did not match.
static esp_err_t get_static(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (!uri || uri[0] != '/') {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    const char  *q = strchr(uri, '?');
    const size_t len = q ? (size_t)(q - uri) : strlen(uri);
    char         name[APP_PACK_NAME_MAX];
    if (len < 2 || len > sizeof(name)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    memcpy(name, uri + 1, len - 1);
    name[len - 1] = '\0';

    // A name is looked up, never opened: one with a slash or dots in it simply matches nothing.
    const uint8_t *bytes = NULL;
    size_t         size = 0;
    const char    *text = NULL;
    if (s_image_ok && app_pack_find(&s_image, name, &bytes, &size)) {
        // The build ships an asset gzipped where that shrinks it, under its own name
        // (docs/architecture/webapp.md, Build), and refuses a plain one that starts with gzip's
        // magic, so the bytes say which they are. Every browser accepts gzip, so Accept-Encoding
        // is not consulted.
        if (size >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
    } else if (app_serve_seed(name, &text, &size)) {
        bytes = (const uint8_t *)text;
    } else {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    httpd_resp_set_type(req, mime_for(name));
    // What is served changes with the image; a browser must not keep a copy across an update.
    // no-store, not no-cache: there is no ETag to revalidate against, and it is one of the signals
    // a browser checks before keeping a page in its back-forward cache (app.html's pageshow
    // handler is the other half: a restore from that cache asks for nothing at all).
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // One call, with its length, straight from mapped flash: nothing is copied to the heap, so a
    // large page cannot fail for want of one contiguous block, as a whole-file read once did.
    return httpd_resp_send(req, (const char *)bytes, (ssize_t)size);
}

esp_err_t app_serve_register(httpd_handle_t server)
{
    static const httpd_uri_t route = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_static,
    };
    _Static_assert(PAIRING_ROUTES_APP_SERVE == 1, "GET /*");
    // Open: the same as reading any other page on the device (docs/architecture/system.md (Who may
    // key the transmitter)).
    return pairing_http_register(server, &route, PAIRING_OPEN);
}
