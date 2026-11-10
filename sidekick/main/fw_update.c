// Firmware updates over the air. See fw_update.h.

#include "fw_update.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "fw_policy.h"
#include "pairing_http.h"
#include "pairing_http_cap.h"
#include "wifi_prov.h"

static const char *TAG = "fw_update";

#define NVS_NAMESPACE "sidekick"
#define NVS_KEY_LAST  "fw_last"

#define DOWNLOAD_CHUNK      4096
#define DOWNLOAD_TIMEOUT_MS 15000
// Long enough for a page polling once a second to read "restarting" before
// the server goes away.
#define RESTART_DELAY_MS 2500

typedef enum {
    FW_IDLE = 0,
    FW_DOWNLOADING,
    FW_VERIFYING,
    FW_RESTARTING,
    FW_FAILED,
} fw_state_t;

static const char *const STATE_NAMES[] = {"idle", "downloading", "verifying", "restarting", "failed"};

// Everything GET /api/fw/status reports. Written by the update task and read
// by the httpd task, under s_lock.
static struct {
    fw_state_t   state;
    fw_request_t req;
    uint32_t     got;
    const char  *why;     // a failure's code; "" otherwise
    int          detail;  // an HTTP status or esp_err_t beside it, 0 if none
} s_st = {.why = ""};

static SemaphoreHandle_t s_lock;
static volatile bool     s_cancel;
static volatile bool     s_client_seen;
static bool              s_pending_verify;  // this image is on trial
static bool              s_have_last;
static fw_record_t       s_last;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_state(fw_state_t state)
{
    lock();
    s_st.state = state;
    unlock();
}

static void fail(const char *why, int detail)
{
    ESP_LOGW(TAG, "update failed: %s (%d)", why, detail);
    lock();
    s_st.state = FW_FAILED;
    s_st.why = why;
    s_st.detail = detail;
    unlock();
}

static bool running_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void fw_update_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_pending_verify = running_pending_verify();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char   text[FW_RECORD_MAX];
    size_t len = sizeof(text);
    if (nvs_get_str(h, NVS_KEY_LAST, text, &len) == ESP_OK && fw_record_parse(text, &s_last)) {
        s_have_last = true;
        ESP_LOGI(TAG, "last update: %s to %s from %s: %s", fw_board_name(s_last.board), s_last.target, s_last.from,
                 fw_last_name(fw_last_result(&s_last, esp_app_get_description()->version, s_pending_verify)));
    }
    nvs_close(h);
}

static bool store_record(const fw_record_t *rec)
{
    char text[FW_RECORD_MAX];
    if (!fw_record_format(rec, text, sizeof(text))) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(h, NVS_KEY_LAST, text) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void fw_update_tick(uint32_t uptime_s, uint32_t beacons_sent)
{
    if (!s_pending_verify) {
        return;
    }
    switch (fw_confirm_decide(uptime_s, beacons_sent, wifi_prov_is_online() || s_client_seen)) {
        case FW_CONFIRM_KEEP:
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                s_pending_verify = false;
                ESP_LOGI(TAG, "new image kept: beaconing, and on the network");
            } else {
                ESP_LOGE(TAG, "could not mark the image valid; it will roll back on restart");
            }
            break;
        case FW_CONFIRM_ROLL_BACK:
            ESP_LOGE(TAG, "the new image did not get on the network in %u s; restarting into the one before it",
                     (unsigned)FW_CONFIRM_TIMEOUT_S);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
            break;
        default:
            break;
    }
}

static void digest_to_hex(const unsigned char dig[32], char out[FW_SHA256_HEX + 1])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = HEX[dig[i] >> 4];
        out[2 * i + 1] = HEX[dig[i] & 0xF];
    }
    out[FW_SHA256_HEX] = '\0';
}

// Connects to the URL's host and sends the request. Returns the socket, or
// -1 having called fail().
static int http_open(const fw_request_t *req)
{
    char        host[FW_URL_MAX];
    uint16_t    port;
    const char *path;
    if (!fw_url_split(req->url, host, sizeof(host), &port, &path)) {
        fail("download", ESP_ERR_INVALID_ARG);
        return -1;
    }
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo      *res = NULL;
    const int             gai = getaddrinfo(host, port_text, &hints, &res);
    if (gai != 0 || !res) {
        // No DNS answer: most often a network with no internet.
        fail("dns", gai);
        return -1;
    }
    const int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        fail("download", errno);
        return -1;
    }
    const struct timeval tv = {.tv_sec = DOWNLOAD_TIMEOUT_MS / 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        fail("connect", errno);
        close(fd);
        return -1;
    }
    // HTTP/1.0: the reply is the bytes and then a close, never chunked.
    char      head[FW_URL_MAX * 2 + 96];
    const int n = snprintf(head, sizeof(head),
                           "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: micro-ft8-sidekick/%s\r\nConnection: close\r\n\r\n",
                           path, host, esp_app_get_description()->version);
    if (n <= 0 || (size_t)n >= sizeof(head) || send(fd, head, (size_t)n, 0) != n) {
        fail("download", errno);
        close(fd);
        return -1;
    }
    return fd;
}

// Downloads req into the OTA handle, hashing as it goes. Returns true with
// the image written and its hash matching; otherwise it has called fail().
// `buf` is DOWNLOAD_CHUNK bytes.
static bool download_into(const fw_request_t *req, esp_ota_handle_t ota, unsigned char *buf)
{
    const int fd = http_open(req);
    if (fd < 0) {
        return false;
    }
    bool                   ok = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    // The head, which fits one buffer, and whatever of the body came with it.
    size_t  have = 0;
    int     head_len = 0;
    int     status = 0;
    int64_t content_len = -1;
    while (head_len == 0) {
        const int n = have < DOWNLOAD_CHUNK ? recv(fd, buf + have, DOWNLOAD_CHUNK - have, 0) : 0;
        if (n <= 0) {
            fail("download", n < 0 ? errno : 0);
            goto done;
        }
        have += (size_t)n;
        head_len = fw_http_head_parse((const char *)buf, have, &status, &content_len);
    }
    if (head_len < 0) {
        // Not HTTP as we asked for it: a captive portal, or a proxy in the way.
        fail("download", ESP_ERR_INVALID_RESPONSE);
        goto done;
    }
    if (status != 200) {
        fail("http", status);
        goto done;
    }
    if (content_len >= 0 && content_len != (int64_t)req->size) {
        fail("size", (int)content_len);
        goto done;
    }

    uint32_t got = 0;
    size_t   n = have - (size_t)head_len;  // body bytes already in buf
    memmove(buf, buf + head_len, n);
    for (;;) {
        if (n > req->size - got) {
            fail("size", (int)(got + n));  // more than the manifest said
            goto done;
        }
        if (n > 0) {
            mbedtls_sha256_update(&sha, buf, n);
            const esp_err_t err = esp_ota_write(ota, buf, n);
            if (err != ESP_OK) {
                // The first write checks the image's magic byte.
                fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image" : "flash", err);
                goto done;
            }
            got += (uint32_t)n;
            lock();
            s_st.got = got;
            unlock();
        }
        if (got == req->size) {
            break;
        }
        if (s_cancel) {
            fail("cancelled", 0);
            goto done;
        }
        const int r = recv(fd, buf, DOWNLOAD_CHUNK, 0);
        if (r < 0) {
            fail("download", errno);  // EAGAIN: nothing for the whole timeout
            goto done;
        }
        if (r == 0) {
            fail("size", (int)got);  // closed early
            goto done;
        }
        n = (size_t)r;
    }

    set_state(FW_VERIFYING);
    unsigned char dig[32];
    char          hex[FW_SHA256_HEX + 1];
    mbedtls_sha256_finish(&sha, dig);
    digest_to_hex(dig, hex);
    if (strcmp(hex, req->sha256_hex) != 0) {
        ESP_LOGW(TAG, "hash is %s, expected %s", hex, req->sha256_hex);
        fail("hash", 0);
        goto done;
    }
    ok = true;

done:
    mbedtls_sha256_free(&sha);
    close(fd);
    return ok;
}

static void update_task(void *arg)
{
    (void)arg;
    lock();
    const fw_request_t req = s_st.req;
    unlock();
    ESP_LOGI(TAG, "updating %s to %s: %" PRIu32 " bytes from %s", fw_board_name(req.board), req.version, req.size, req.url);

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    unsigned char         *buf = malloc(DOWNLOAD_CHUNK);
    esp_ota_handle_t       ota = 0;
    esp_err_t              err = (slot && buf) ? esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        fail("flash", err);
        goto out;
    }
    if (!download_into(&req, ota, buf)) {
        esp_ota_abort(ota);
        goto out;
    }
    // The image's own checksum and chip, then what it says it is. The running
    // image is untouched until esp_ota_set_boot_partition() below.
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        fail("image", err);
        goto out;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(slot, &desc) != ESP_OK ||
        strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        fail("image", 0);
        goto out;
    }
    if (strcmp(desc.version, req.version) != 0) {
        ESP_LOGW(TAG, "image says it is %s, expected %s", desc.version, req.version);
        fail("version", 0);
        goto out;
    }
    if (s_cancel) {
        fail("cancelled", 0);
        goto out;
    }

    fw_record_t rec = {.board = req.board};
    snprintf(rec.target, sizeof(rec.target), "%s", req.version);
    snprintf(rec.from, sizeof(rec.from), "%s", esp_app_get_description()->version);
    if (!store_record(&rec)) {
        // Not fatal: the update still happens, and only the report after it is lost.
        ESP_LOGW(TAG, "could not store the update record");
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        fail("flash", err);
        goto out;
    }
    set_state(FW_RESTARTING);
    ESP_LOGI(TAG, "image verified; restarting into %s", req.version);
    free(buf);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();

out:
    free(buf);
    vTaskDelete(NULL);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t get_status(httpd_req_t *req)
{
    const char *running = esp_app_get_description()->version;
    char        last[160] = "";
    if (s_have_last) {
        snprintf(last, sizeof(last), ",\"last\":{\"board\":\"%s\",\"version\":\"%s\",\"from\":\"%s\",\"result\":\"%s\"}",
                 fw_board_name(s_last.board), s_last.target, s_last.from,
                 fw_last_name(fw_last_result(&s_last, running, s_pending_verify)));
    }
    char body[512];
    lock();
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"board\":\"%s\",\"version\":\"%s\",\"got\":%" PRIu32 ",\"size\":%" PRIu32
             ",\"why\":\"%s\",\"detail\":%d,\"running\":\"%s\",\"online\":%s%s}",
             STATE_NAMES[s_st.state], fw_board_name(s_st.req.board), s_st.req.version, s_st.got, s_st.req.size,
             s_st.why, s_st.detail, running, wifi_prov_is_online() ? "true" : "false", last);
    unlock();
    // A client got an answer from this image, which counts as reachable even on its own access point.
    s_client_seen = true;
    return send_json(req, "200 OK", body);
}

static esp_err_t refuse(httpd_req_t *req, const char *status, const char *why)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"why\":\"%s\"}", why);
    return send_json(req, status, body);
}

static esp_err_t post_start(httpd_req_t *req)
{
    char body[FW_URL_MAX + 192];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return refuse(req, "400 Bad Request", "body");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return refuse(req, "400 Bad Request", "body");
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    fw_request_t parsed;
    const char  *bad = fw_request_parse(body, &parsed);
    if (bad) {
        return refuse(req, "400 Bad Request", bad);
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);

    lock();
    const fw_gate_t gate = {
        .online = wifi_prov_is_online(),
        .busy = s_st.state == FW_DOWNLOADING || s_st.state == FW_VERIFYING || s_st.state == FW_RESTARTING,
        .pending_verify = s_pending_verify,
        .slot_bytes = slot ? slot->size : 0,
    };
    const char *why = fw_start_refusal(&parsed, &gate);
    if (!why) {
        s_st.state = FW_DOWNLOADING;
        s_st.req = parsed;
        s_st.got = 0;
        s_st.why = "";
        s_st.detail = 0;
        s_cancel = false;
    }
    unlock();
    if (why) {
        return refuse(req, "409 Conflict", why);
    }
    if (xTaskCreate(update_task, "fw_update", 8192, NULL, 4, NULL) != pdPASS) {
        fail("flash", ESP_ERR_NO_MEM);
        return refuse(req, "500 Internal Server Error", "flash");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

// Stops a download. Past verification the restart is already on its way, and
// the page says so rather than offering this.
static esp_err_t post_cancel(httpd_req_t *req)
{
    lock();
    const bool running = s_st.state == FW_DOWNLOADING || s_st.state == FW_VERIFYING;
    unlock();
    if (!running) {
        return refuse(req, "409 Conflict", "idle");
    }
    s_cancel = true;
    return send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t fw_update_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    static const httpd_uri_t status = {
        .uri = "/api/fw/status", .method = HTTP_GET, .handler = get_status,
    };
    static const httpd_uri_t start = {
        .uri = "/api/fw/start", .method = HTTP_POST, .handler = post_start,
    };
    static const httpd_uri_t cancel = {
        .uri = "/api/fw/cancel", .method = HTTP_POST, .handler = post_cancel,
    };
    const struct {
        const httpd_uri_t *uri;
        pairing_policy_t   policy;
    } routes[] = {
        { &status, PAIRING_OPEN },
        { &start, PAIRING_REQUIRED },
        { &cancel, PAIRING_REQUIRED },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == PAIRING_ROUTES_FW_UPDATE, "fw_update route count");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t err = pairing_http_register(server, routes[i].uri, routes[i].policy);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
