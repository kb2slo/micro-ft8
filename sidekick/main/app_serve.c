// What GET /<name> answers with: the web app from this image's pack, or from a pack pushed onto
// this image from the bench, and the bootstrap pages from their embed. See app_serve.h and
// docs/architecture/sidekick.md (Web root, The bench push).

#include "app_serve.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "app_pack.h"
#include "app_push.h"
#include "fw_update.h"
#include "http_worker.h"
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

// ---- the bench push (app_push.h) ----------------------------------------------------------
//
// A pushed pack lies in the spare firmware slot behind a header naming the image it was pushed
// onto. While that header names this image the push is served, and only the push: an asset it
// lacks is a 404 though the image has it, so a page is never a mix of two builds. It is mapped,
// like the image's own pack, and unmapped before anything else writes the slot.

static app_pack_t                  s_push;
static bool                        s_push_ok;
static bool                        s_push_mapped;
static esp_partition_mmap_handle_t s_push_map;

// Files are sent by the sender pool (http_worker.h), several at once and straight from mapped flash, while a
// push is written and dropped by other tasks (the push's own request, a firmware update). So a sender counts
// itself in before it sends from the push and out after, and the push is unmapped only once none is: a send
// from flash unmapped under it would fault. s_push_ok and s_push_readers change only under s_push_lock.
static SemaphoreHandle_t s_push_lock;
static int               s_push_readers;
// A send lasts at most HTTP_SEND_DEADLINE_MS, so a reader is out well within this.
#define PUSH_READERS_WAIT_MS (HTTP_SEND_DEADLINE_MS + 5000)

static void push_lock(void)
{
    if (!s_push_lock) {
        s_push_lock = xSemaphoreCreateMutex();  // first called at boot, before the server starts
    }
    xSemaphoreTake(s_push_lock, portMAX_DELAY);
}

static void push_unlock(void)
{
    xSemaphoreGive(s_push_lock);
}

static const esp_partition_t *spare_slot(void)
{
    return esp_ota_get_next_update_partition(NULL);
}

static void push_unmap(void)
{
    push_lock();
    s_push_ok = false;  // no sender starts on it from here
    push_unlock();
    if (!s_push_mapped) {
        return;
    }
    for (int waited = 0;; waited += 10) {
        push_lock();
        const int readers = s_push_readers;
        push_unlock();
        if (readers == 0) {
            break;
        }
        if (waited >= PUSH_READERS_WAIT_MS) {
            // Not reachable while every send has its deadline. Left mapped rather than fault a sender: no
            // longer served, and a later map takes a new handle.
            ESP_LOGE(TAG, "pushed app still being sent after %d ms: left mapped", waited);
            s_push_mapped = false;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    esp_partition_munmap(s_push_map);
    s_push_mapped = false;
}

// Maps the pack `head` describes and checks it where it lies: its hash, which is a read back of
// what was written, then as a pack. NULL with the push being served, else why not, unmapped.
static const char *push_map(const esp_partition_t *slot, const app_push_head_t *head)
{
    const void *ptr = NULL;
    if (esp_partition_mmap(slot, APP_PUSH_PACK_AT, head->pack_len, ESP_PARTITION_MMAP_DATA, &ptr, &s_push_map) !=
        ESP_OK) {
        return "map";
    }
    s_push_mapped = true;
    uint8_t digest[32];
    if (mbedtls_sha256((const unsigned char *)ptr, head->pack_len, digest, 0) != 0 ||
        memcmp(digest, head->pack_sha256, sizeof(digest)) != 0) {
        push_unmap();
        return "hash";
    }
    const char *bad = app_pack_open((const uint8_t *)ptr, head->pack_len, &s_push);
    if (bad) {
        push_unmap();
        return bad;
    }
    push_lock();
    s_push_ok = true;
    push_unlock();
    return NULL;
}

// At boot: a push onto this image, if the spare slot holds one.
static void push_load(void)
{
    const esp_partition_t *slot = spare_slot();
    uint8_t                record[APP_PUSH_RECORD_LEN];
    if (!slot || esp_partition_read(slot, 0, record, sizeof(record)) != ESP_OK) {
        return;
    }
    app_push_head_t       head;
    const esp_app_desc_t *desc = esp_app_get_description();
    const char           *why = app_push_head_read(record, desc->version, desc->app_elf_sha256, slot->size, &head);
    if (!why) {
        why = push_map(slot, &head);
    }
    if (!why) {
        ESP_LOGW(TAG, "serving a pushed app, %s, in this image's place (%u bytes)", s_push.build, (unsigned)s_push.len);
    } else if (strcmp(why, "none") != 0) {
        ESP_LOGW(TAG, "the spare slot holds a pushed app that is not served: %s", why);
    }
}

void app_serve_drop_push(void)
{
    push_unmap();
    const esp_partition_t *slot = spare_slot();
    uint8_t                magic[APP_PUSH_MAGIC_LEN];
    // Only a push's header is erased: anything else there is a firmware image, and not ours to touch.
    if (slot && esp_partition_read(slot, 0, magic, sizeof(magic)) == ESP_OK &&
        memcmp(magic, APP_PUSH_MAGIC, sizeof(magic)) == 0) {
        const esp_err_t err = esp_partition_erase_range(slot, 0, APP_PUSH_PACK_AT);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "pushed app dropped: this image's own is served again");
        } else {
            // It is unmapped, so not served now; with its header left, a restart would serve it again.
            ESP_LOGE(TAG, "could not erase the pushed app's header (%s): it comes back at the next restart",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t push_refuse(httpd_req_t *req, const char *status, const char *why)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"why\":\"%s\"}", why);
    return send_json(req, status, body);
}

#define PUSH_CHUNK 4096u

// Receives the request's body into the spare slot from APP_PUSH_PACK_AT, each flash sector erased
// as the write reaches it, hashing as it goes. NULL when all of it is written, else why not.
static const char *push_receive(httpd_req_t *req, const esp_partition_t *slot, uint8_t sha256_out[32])
{
    unsigned char *buf = malloc(PUSH_CHUNK);
    if (!buf) {
        return "flash";
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const char    *why = NULL;
    const uint32_t len = (uint32_t)req->content_len;
    const uint32_t sector = slot->erase_size;
    uint32_t       got = 0;
    uint32_t       erased_to = APP_PUSH_PACK_AT;
    while (got < len && !why) {
        const uint32_t want = len - got < PUSH_CHUNK ? len - got : PUSH_CHUNK;
        const int      n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) {
            why = "body";
            break;
        }
        const uint32_t end = APP_PUSH_PACK_AT + got + (uint32_t)n;
        if (end > erased_to) {
            const uint32_t to = (end + sector - 1) / sector * sector;
            if (esp_partition_erase_range(slot, erased_to, to - erased_to) != ESP_OK) {
                why = "flash";
                break;
            }
            erased_to = to;
        }
        if (esp_partition_write(slot, APP_PUSH_PACK_AT + got, buf, (size_t)n) != ESP_OK) {
            why = "flash";
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        got += (uint32_t)n;
    }
    if (!why) {
        mbedtls_sha256_finish(&sha, sha256_out);
    }
    mbedtls_sha256_free(&sha);
    free(buf);
    return why;
}

// POST /api/app/push, body a pack: webapp/tools/install_app.py's one request.
static esp_err_t post_push(httpd_req_t *req)
{
    const esp_partition_t *slot = spare_slot();
    const esp_app_desc_t  *desc = esp_app_get_description();
    const app_push_gate_t  gate = {
        .busy = fw_update_busy(),
        .pending_verify = fw_update_on_trial(),
        .version = desc->version,
        .slot_bytes = slot ? slot->size : 0,
        .pack_len = req->content_len > UINT32_MAX ? UINT32_MAX : (uint32_t)req->content_len,
    };
    const char *why = app_push_refusal(&gate);
    if (why) {
        // The body is not read. An answer sent ahead of an unread body can be lost to the reset
        // that closing brings, so a body that could be a pack is read off first; anything larger
        // is answered and the connection closed.
        if (req->content_len <= gate.slot_bytes) {
            char discard[256];
            while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {
            }
            return push_refuse(req, "409 Conflict", why);
        }
        push_refuse(req, "409 Conflict", why);
        return ESP_FAIL;
    }

    // From here the slot is being rewritten. Nothing is served from it meanwhile, and the header's
    // sector, which may hold the start of the firmware image this one replaced, is erased first
    // and written last: the slot holds a whole push or none.
    push_unmap();
    if (esp_partition_erase_range(slot, 0, APP_PUSH_PACK_AT) != ESP_OK) {
        return push_refuse(req, "500 Internal Server Error", "flash");
    }
    app_push_head_t head = {.pack_len = gate.pack_len};
    memcpy(head.image, desc->app_elf_sha256, sizeof(head.image));
    why = push_receive(req, slot, head.pack_sha256);
    if (why) {
        ESP_LOGW(TAG, "push failed: %s", why);
        if (strcmp(why, "body") == 0) {
            return ESP_FAIL;  // the client went away mid-body: nobody to answer
        }
        return push_refuse(req, "500 Internal Server Error", why);
    }
    why = push_map(slot, &head);
    if (why) {
        ESP_LOGW(TAG, "pushed pack refused: %s", why);
        return push_refuse(req, "400 Bad Request", "pack");
    }
    uint8_t record[APP_PUSH_RECORD_LEN];
    app_push_head_write(&head, record);
    if (esp_partition_write(slot, 0, record, sizeof(record)) != ESP_OK) {
        push_unmap();
        return push_refuse(req, "500 Internal Server Error", "flash");
    }
    ESP_LOGW(TAG, "pushed app %s: %u bytes, served in this image's place", s_push.build, (unsigned)s_push.len);
    char body[APP_PACK_BUILD_MAX + 32];
    snprintf(body, sizeof(body), "{\"ok\":true,\"build\":\"%s\"}", s_push.build);
    return send_json(req, "200 OK", body);
}

void app_serve_init(void)
{
    const size_t len = (size_t)(image_pack_end - image_pack_start);
    const char  *bad = app_pack_open(image_pack_start, len, &s_image);
    s_image_ok = bad == NULL;
    push_load();
    if (bad) {
        ESP_LOGE(TAG, "this image's app pack does not open (%s, %u bytes): the app will 404", bad, (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "app %s: %u assets, %u bytes, in this image", s_image.build, (unsigned)s_image.count,
             (unsigned)s_image.len);
}

const char *app_serve_build(void)
{
    return s_push_ok ? s_push.build : s_image_ok ? s_image.build : "";
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

static esp_err_t serve_from(httpd_req_t *req, const char *name, const app_pack_t *pack);

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
    // A push is served in the image's place, whole: never one build's page with another's script.
    push_lock();
    const bool        from_push = s_push_ok;
    const app_pack_t *pack = from_push ? &s_push : s_image_ok ? &s_image : NULL;
    if (from_push) {
        s_push_readers++;
    }
    push_unlock();
    const esp_err_t err = serve_from(req, name, pack);
    if (from_push) {
        push_lock();
        s_push_readers--;
        push_unlock();
    }
    return err;
}

// Sends `name` from `pack` (or the seed pages when there is none), counted in as a reader of a push by the
// caller.
static esp_err_t serve_from(httpd_req_t *req, const char *name, const app_pack_t *pack)
{
    const uint8_t *bytes = NULL;
    size_t         size = 0;
    const char    *text = NULL;
    if (pack && app_pack_find(pack, name, &bytes, &size)) {
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
    static const httpd_uri_t push = {
        .uri = "/api/app/push",
        .method = HTTP_POST,
        .handler = post_push,
    };
    static const httpd_uri_t route = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = get_static,
    };
    _Static_assert(PAIRING_ROUTES_APP_SERVE == 2, "POST /api/app/push, GET /*");
    // It replaces what every page of the station is: a write, like any other.
    const esp_err_t err = pairing_http_register(server, &push, PAIRING_REQUIRED);
    if (err != ESP_OK) {
        return err;
    }
    // Open: the same as reading any other page on the device (docs/architecture/system.md (Who may
    // key the transmitter)). Sent by the sender pool, at most HTTP_FILE_SENDERS at once, so a client slow to
    // take a page holds one sender and not the server, and events always have one (http_worker.h).
    return pairing_http_register_on_worker(server, &route, PAIRING_OPEN, HTTP_WORKER_FILES);
}
