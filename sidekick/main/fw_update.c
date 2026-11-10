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
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "fw_policy.h"
#include "host_link.h"
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

// The main board's HELLO comes every 5 s; three missed and it is not there.
#define HOST_LINKED_MS 16000
// A transmission ends within a slot of being cancelled.
#define TX_STOP_WAIT_MS 20000
// The main board answers a chunk from its 10 ms loop, and a sector erase on the way is under 100 ms.
#define CHUNK_REPLY_MS 3000
#define CHUNK_TRIES    5
// FW_BEGIN opens the slot; FW_END reads the whole image back twice (our hash, then ESP-IDF's).
#define BEGIN_REPLY_MS 10000
#define END_REPLY_MS   30000

typedef enum {
    FW_IDLE = 0,
    FW_STOPPING,      // waiting for the main board's transmitter to stop
    FW_DOWNLOADING,
    FW_VERIFYING,
    FW_TRANSFERRING,  // sending a main board's image down Port A
    FW_RESTARTING,    // this sidekick is about to, or the main board is
    FW_DONE,          // a main board's update ended well (the sidekick's own ends in a restart)
    FW_FAILED,
} fw_state_t;

static const char *const STATE_NAMES[] = {"idle", "stopping", "downloading", "verifying", "transferring",
                                          "restarting", "done", "failed"};

static bool state_busy(fw_state_t s)
{
    return s != FW_IDLE && s != FW_DONE && s != FW_FAILED;
}

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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool host_linked(void)
{
    uint32_t age = 0;
    return host_link_host_hello(NULL, NULL, &age) && age < HOST_LINKED_MS;
}

static void set_got(uint32_t got)
{
    lock();
    s_st.got = got;
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

// Where a download is written: the spare OTA slot either way. The sidekick's
// own image goes through the OTA calls, which check it as an image for this
// chip. A main board's is for another chip, so it is written as plain bytes,
// each flash sector erased as the write reaches it.
typedef struct {
    const esp_partition_t *slot;
    bool                   raw;
    esp_ota_handle_t       ota;
    uint32_t               at;
    uint32_t               erased_to;
} sink_t;

static esp_err_t sink_write(sink_t *sink, const unsigned char *data, size_t len)
{
    if (!sink->raw) {
        return esp_ota_write(sink->ota, data, len);
    }
    const uint32_t end = sink->at + (uint32_t)len;
    if (end > sink->erased_to) {
        const uint32_t sector = sink->slot->erase_size;
        const uint32_t to = (end + sector - 1) / sector * sector;
        const esp_err_t err = esp_partition_erase_range(sink->slot, sink->erased_to, to - sink->erased_to);
        if (err != ESP_OK) {
            return err;
        }
        sink->erased_to = to;
    }
    const esp_err_t err = esp_partition_write(sink->slot, sink->at, data, len);
    if (err == ESP_OK) {
        sink->at = end;
    }
    return err;
}

// Downloads req into the sink, hashing as it goes. Returns true with the
// image written and its hash matching; otherwise it has called fail().
// `buf` is DOWNLOAD_CHUNK bytes.
static bool download_into(const fw_request_t *req, sink_t *sink, unsigned char *buf)
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
            const esp_err_t err = sink_write(sink, buf, n);
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

// The sidekick's own image: let ESP-IDF check it, then make it the boot image
// and restart. Does not return on success.
static void install_own(const fw_request_t *req, sink_t *sink)
{
    // The image's own checksum and chip, then what it says it is. The running
    // image is untouched until esp_ota_set_boot_partition() below.
    esp_err_t err = esp_ota_end(sink->ota);
    if (err != ESP_OK) {
        fail("image", err);
        return;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(sink->slot, &desc) != ESP_OK ||
        strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        fail("image", 0);
        return;
    }
    if (strcmp(desc.version, req->version) != 0) {
        ESP_LOGW(TAG, "image says it is %s, expected %s", desc.version, req->version);
        fail("version", 0);
        return;
    }
    if (s_cancel) {
        fail("cancelled", 0);
        return;
    }
    fw_record_t rec = {.board = req->board};
    snprintf(rec.target, sizeof(rec.target), "%s", req->version);
    snprintf(rec.from, sizeof(rec.from), "%s", esp_app_get_description()->version);
    if (!store_record(&rec)) {
        // Not fatal: the update still happens, and only the report after it is lost.
        ESP_LOGW(TAG, "could not store the update record");
    }
    err = esp_ota_set_boot_partition(sink->slot);
    if (err != ESP_OK) {
        fail("flash", err);
        return;
    }
    set_state(FW_RESTARTING);
    ESP_LOGI(TAG, "image verified; restarting into %s", req->version);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

// fw_transfer_run()'s hands: the staged image in the spare slot, and Port A.
static bool xfer_read(void *ctx, uint32_t offset, uint8_t *out, size_t len)
{
    return esp_partition_read((const esp_partition_t *)ctx, offset, out, len) == ESP_OK;
}

static bool xfer_begin(void *ctx, const porta_fw_begin_t *begin, porta_fw_reply_t *out)
{
    (void)ctx;
    return host_link_fw_begin(begin, BEGIN_REPLY_MS, out);
}

static bool xfer_chunk(void *ctx, uint32_t offset, const uint8_t *data, size_t len, porta_fw_reply_t *out)
{
    (void)ctx;
    return host_link_fw_chunk(offset, data, len, CHUNK_REPLY_MS, out);
}

static bool xfer_end(void *ctx, bool abort, porta_fw_reply_t *out)
{
    (void)ctx;
    return host_link_fw_end(abort, abort ? CHUNK_REPLY_MS : END_REPLY_MS, out);
}

static bool xfer_cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel;
}

static void xfer_progress(void *ctx, uint32_t offset)
{
    (void)ctx;
    set_got(offset);
}

// Sends the staged image down Port A. Returns true once the main board has
// accepted it and is restarting into it; otherwise it has called fail().
static bool transfer_to_host(const fw_request_t *req, const esp_partition_t *slot, unsigned char *buf)
{
    static const fw_transfer_io_t io = {xfer_read, xfer_begin, xfer_chunk, xfer_end, xfer_cancelled, xfer_progress};
    porta_fw_begin_t begin = {.size = req->size};
    fw_sha256_bytes(req->sha256_hex, begin.sha256);
    snprintf(begin.version, sizeof(begin.version), "%s", req->version);

    const uint32_t t0 = now_ms();
    int            detail = 0;
    const char    *why = fw_transfer_run(&io, (void *)slot, &begin, buf, &detail);
    if (why) {
        fail(why, detail);
        return false;
    }
    // The measured time, on the console and in the app's log.
    const uint32_t ms = now_ms() - t0;
    char           line[PORTA_EVENT_TEXT_MAX + 1];
    snprintf(line, sizeof(line), "SK firmware transfer: %" PRIu32 " bytes in %" PRIu32 ".%" PRIu32 " s", req->size, ms / 1000,
             ms % 1000 / 100);
    ESP_LOGI(TAG, "%s", line);
    host_link_log(line);
    return true;
}

// A main board's image: check it is one, send it, and wait for the board to
// come back saying it runs the new version.
static void install_on_host(const fw_request_t *req, const esp_partition_t *slot, unsigned char *buf)
{
    char version[FW_VERSION_MAX];
    if (esp_partition_read(slot, 0, buf, FW_IMAGE_HEAD) != ESP_OK || !fw_image_version(buf, FW_IMAGE_HEAD, version)) {
        fail("image", 0);
        return;
    }
    if (strcmp(version, req->version) != 0) {
        ESP_LOGW(TAG, "image says it is %s, expected %s", version, req->version);
        fail("version", 0);
        return;
    }
    set_got(0);
    set_state(FW_TRANSFERRING);
    host_link_fw_active(true);
    const bool sent = transfer_to_host(req, slot, buf);
    host_link_fw_active(false);
    if (!sent) {
        return;
    }
    set_state(FW_RESTARTING);
    const uint32_t t_end = now_ms();
    for (;;) {
        char     hello[PORTA_HELLO_VERSION_LEN + 1] = "";
        uint32_t age = 0;
        const bool seen = host_link_host_hello(NULL, hello, &age);
        switch (fw_host_result(now_ms() - t_end, seen, age, hello, req->version)) {
            case FW_HOST_UPDATED:
                ESP_LOGI(TAG, "main board is running %s", hello);
                set_state(FW_DONE);
                return;
            case FW_HOST_ROLLED_BACK:
                ESP_LOGW(TAG, "main board came back on %s", hello);
                fail("rolled_back", 0);
                return;
            case FW_HOST_SILENT:
                fail("no_host", 0);
                return;
            default:
                vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void update_task(void *arg)
{
    (void)arg;
    lock();
    const fw_request_t req = s_st.req;
    unlock();
    const bool for_host = req.board == FW_BOARD_MAIN;
    ESP_LOGI(TAG, "updating %s to %s: %" PRIu32 " bytes from %s", fw_board_name(req.board), req.version, req.size, req.url);

    // The main board was put into update mode before this task started
    // (post_start()); a transmission it was in the middle of takes a moment to end.
    const uint32_t t0 = now_ms();
    while (host_link_tx_active() && host_linked()) {
        if (now_ms() - t0 > TX_STOP_WAIT_MS) {
            fail("tx", 0);
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set_state(FW_DOWNLOADING);

    sink_t sink = {.slot = esp_ota_get_next_update_partition(NULL), .raw = for_host};
    unsigned char *buf = malloc(DOWNLOAD_CHUNK);
    esp_err_t      err = (sink.slot && buf) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && !for_host) {
        err = esp_ota_begin(sink.slot, OTA_WITH_SEQUENTIAL_WRITES, &sink.ota);
    }
    if (err != ESP_OK) {
        fail("flash", err);
    } else if (!download_into(&req, &sink, buf)) {
        if (!for_host) {
            esp_ota_abort(sink.ota);
        }
    } else if (for_host) {
        install_on_host(&req, sink.slot, buf);
    } else {
        install_own(&req, &sink);
    }
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
    // The main board's version is JSON-safe only if it is one an image can carry.
    char host_version[PORTA_HELLO_VERSION_LEN + 1] = "";
    host_link_host_hello(NULL, host_version, NULL);
    if (!fw_version_ok(host_version)) {
        host_version[0] = '\0';
    }
    const bool linked = host_linked();
    char       body[640];
    lock();
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"board\":\"%s\",\"version\":\"%s\",\"got\":%" PRIu32 ",\"size\":%" PRIu32
             ",\"why\":\"%s\",\"detail\":%d,\"running\":\"%s\",\"host\":\"%s\",\"host_linked\":%s,\"online\":%s%s}",
             STATE_NAMES[s_st.state], fw_board_name(s_st.req.board), s_st.req.version, s_st.got, s_st.req.size,
             s_st.why, s_st.detail, running, host_version, linked ? "true" : "false",
             wifi_prov_is_online() ? "true" : "false", last);
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
    const bool      linked = host_linked();
    const fw_gate_t gate = {
        .online = wifi_prov_is_online(),
        .busy = state_busy(s_st.state),
        .pending_verify = s_pending_verify,
        .slot_bytes = slot ? slot->size : 0,
        .host_linked = linked,
    };
    const char *why = fw_start_refusal(&parsed, &gate);
    if (!why) {
        s_st.state = FW_STOPPING;
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
    // Every update stops the radio first, the sidekick's own included: its restart takes the
    // phone's controls away while the main board could go on transmitting. With no main board
    // answering there is nothing to stop. It stays stopped until POST /api/fw/resume or its restart.
    if (linked) {
        char reason[PORTA_EVENT_TEXT_MAX + 1];
        if (!host_link_update_mode(true, reason, sizeof(reason))) {
            ESP_LOGW(TAG, "main board would not enter update mode: %s", reason);
            fail("host_mode", 0);
            return refuse(req, "409 Conflict", "host_mode");
        }
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
    const bool running = s_st.state == FW_STOPPING || s_st.state == FW_DOWNLOADING || s_st.state == FW_VERIFYING ||
                         s_st.state == FW_TRANSFERRING;
    unlock();
    if (!running) {
        return refuse(req, "409 Conflict", "idle");
    }
    s_cancel = true;
    return send_json(req, "200 OK", "{\"ok\":true}");
}

// Takes the main board out of update mode: the operator's Resume operating, after an update
// succeeded, failed or was cancelled. Never by itself.
static esp_err_t post_resume(httpd_req_t *req)
{
    lock();
    const bool busy = state_busy(s_st.state);
    unlock();
    if (busy) {
        return refuse(req, "409 Conflict", "busy");
    }
    char reason[PORTA_EVENT_TEXT_MAX + 1];
    if (!host_link_update_mode(false, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "main board would not leave update mode: %s", reason);
        return refuse(req, "409 Conflict", "no_host");
    }
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
    static const httpd_uri_t resume = {
        .uri = "/api/fw/resume", .method = HTTP_POST, .handler = post_resume,
    };
    const struct {
        const httpd_uri_t *uri;
        pairing_policy_t   policy;
    } routes[] = {
        { &status, PAIRING_OPEN },
        { &start, PAIRING_REQUIRED },
        { &cancel, PAIRING_REQUIRED },
        { &resume, PAIRING_REQUIRED },
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
