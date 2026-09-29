#include "host_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pairing_http.h"
#include "pairing_http_cap.h"
#include "porta_proto.h"
#include "web_page.h"
#include "wifi_prov.h"

#define PORTA_UART UART_NUM_1

static const char *TAG = "host";

// Copy a frame onto the Port A UART without blocking the httpd task forever.
// If the ADV is unplugged or not draining RX, uart_write_bytes() can stall
// indefinitely once the TX ring fills — that hung GET /api/config with no body.
static bool porta_send(const uint8_t *frame, size_t n)
{
    if (!frame || n == 0) {
        return false;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    size_t sent = 0;
    while (sent < n) {
        size_t free_sz = 0;
        if (uart_get_tx_buffer_free_size(PORTA_UART, &free_sz) != ESP_OK) {
            ESP_LOGW(TAG, "porta TX: free-size query failed");
            return false;
        }
        if (free_sz == 0) {
            if (xTaskGetTickCount() >= deadline) {
                ESP_LOGW(TAG, "porta TX: ring full (host not draining?)");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t chunk = n - sent;
        if (chunk > free_sz) {
            chunk = free_sz;
        }
        const int w = uart_write_bytes(PORTA_UART, (const char *)(frame + sent), chunk);
        if (w <= 0) {
            ESP_LOGW(TAG, "porta TX: write failed");
            return false;
        }
        sent += (size_t)w;
    }
    return true;
}

// Ring of recent events. 64 is a couple of FT8 slots' worth of decodes plus
// their log lines -- enough that a browser polling once a second never misses
// anything, and small enough to sit in RAM without thought (~6 KB).
#define RING_LEN 64

typedef enum {
    ENTRY_LOG = 0,
    ENTRY_DECODE = 1,
    ENTRY_QUEUE_ENTRY = 2,  // I28d, docs/architecture/webapp.md (Screens)
    ENTRY_SLOT_STATE = 3,
    ENTRY_TX_HUD = 4,       // I28d, docs/architecture/webapp.md (Screens)
    ENTRY_USB_STATUS = 5,   // host's own USB-C host port: device attached/detached
} entry_kind_t;

typedef struct {
    uint32_t seq;
    uint32_t epoch_secs;   // host's clock at the moment of the event, 0 if unset
    entry_kind_t kind;
    char     text[PORTA_EVENT_TEXT_MAX + 1];  // LOG, DECODE, TX_HUD
    int8_t   snr;                             // DECODE
    uint16_t offset_hz;                       // DECODE; SLOT_STATE's resolved_offset_hz;
                                               // TX_HUD's own offset_hz (what's on air)
    int16_t  dt_centis;                       // DECODE
    bool     is_cq;                           // DECODE
    bool     is_to_me;                        // DECODE
    bool     is_recent_qso;                   // DECODE
    uint32_t decode_id;                       // DECODE -- names it for QUEUE_REPLY
    uint32_t slot_idx;                        // DECODE: the UTC slot heard in, 0 unknown
    uint8_t  slot_len_ds;                     // DECODE: slot length, tenths of a second
    uint32_t dial_hz;                         // DECODE: dial at the signal's middle, 0 unknown
    uint16_t entry_id;                        // QUEUE_ENTRY
    uint8_t  state;                           // QUEUE_ENTRY (AutoseqState wire value)
    uint8_t  retry_count;                     // QUEUE_ENTRY
    uint8_t  retry_limit;                     // QUEUE_ENTRY
    char     dxcall[PORTA_CALLSIGN_MAX + 1];  // QUEUE_ENTRY
    uint8_t  slot_parity;                     // SLOT_STATE
    uint8_t  beacon_mode;                     // SLOT_STATE
    bool     tx_active;                       // TX_HUD
    bool     tx_aborted;                      // TX_HUD
    bool     tx_writes_blocked;               // TX_HUD
    int16_t  tx_power_dw;                     // TX_HUD, deciwatts
    int16_t  tx_swr_c;                        // TX_HUD, SWR * 100
    int8_t   tx_battery_pct;                  // TX_HUD
    char     tx_reason[PORTA_TX_HUD_REASON_MAX + 1];  // TX_HUD, why tx_aborted
    bool     usb_present;                     // USB_STATUS: true = attach, false = detach
    uint8_t  usb_kind;                        // USB_STATUS: UsbCKind wire value
    uint16_t usb_vid;                         // USB_STATUS
    uint16_t usb_pid;                         // USB_STATUS
} entry_t;

static entry_t s_ring[RING_LEN];
static uint32_t s_next_seq = 1;     // 0 is "before anything", so a new client gets all
static SemaphoreHandle_t s_lock;

// Ring-buffer gaps (get_events() below): the browser's `since` cursor landed
// behind the oldest event the ring still has, so at least one event -- worst
// case, a TX_HUD falling edge -- was silently skipped. Counted, not just
// logged, for the same reason: a number the operator can watch move is worth
// more than "check the serial log."
static volatile uint32_t s_ring_gap_events;

// The receive task writes and an httpd handler reads, on different tasks. The
// critical sections are a memcpy each, so a plain mutex is the right weight.
static void ring_push(const entry_t *e)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *slot = &s_ring[s_next_seq % RING_LEN];
    *slot = *e;
    slot->seq = s_next_seq++;
    xSemaphoreGive(s_lock);
}

// JSON string escaping. Decoded FT8 text is normally plain ASCII, but it comes
// off a radio: a corrupt decode can contain anything, and one stray quote or
// backslash would turn the whole response into a parse error in the browser
// rather than one odd-looking row.
static void json_escape(const char *in, char *out, size_t out_cap)
{
    size_t w = 0;
    for (size_t i = 0; in[i] && w + 7 < out_cap; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 0x20 || c == 0x7F) {
            w += (size_t)snprintf(&out[w], out_cap - w, "\\u%04x", c);
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

// GET /api/events?since=<seq>
//
// Polling rather than server-sent events: ESP-IDF's httpd dispatches handlers
// on a single task, so a long-lived streaming response would block every other
// request, the status page included. A poll cannot wedge the server, costs a
// second of latency on a 15-second cadence, and matches the polled PORTA design
// so the whole chain works the same way.
static esp_err_t get_events(httpd_req_t *req)
{
    uint32_t since = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"events\":[");

    bool first = true;
    char esc[PORTA_EVENT_TEXT_MAX * 6 + 1];
    // Worst case: text fully \u-escaped (6 bytes a char) plus a DECODE's
    // fixed fields, the largest (~165 bytes with v2's slot and dial).
    char row[PORTA_EVENT_TEXT_MAX * 6 + 224];
    uint32_t head;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    head = s_next_seq;
    // Oldest still in the ring. A client that has been away longer than the
    // ring simply resumes from here -- gaps are visible as a jump in seq rather
    // than silently filled with stale rows.
    uint32_t ring_oldest = (head > RING_LEN) ? (head - RING_LEN) : 0;
    // since==0 is a fresh client asking for everything, not a gap -- only
    // count it when a *returning* client's cursor has fallen behind what the
    // ring still has. A busy slot plus a backgrounded phone tab is enough to
    // do this in well under a minute; see this counter's own comment.
    if (since > 0 && since < ring_oldest) {
        s_ring_gap_events++;
    }
    uint32_t from = ring_oldest;
    if (since > from) {
        from = since;
    }
    for (uint32_t s = from; s < head; ++s) {
        entry_t e = s_ring[s % RING_LEN];
        if (e.seq != s) {
            continue;   // overwritten while we walked; skip rather than lie
        }
        xSemaphoreGive(s_lock);

        json_escape(e.text, esc, sizeof(esc));
        char esc_call[sizeof(esc)];
        int n;
        switch (e.kind) {
        case ENTRY_DECODE:
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":1,\"x\":\"%s\","
                         "\"id\":%" PRIu32 ",\"snr\":%d,\"hz\":%u,\"dt\":%.2f,\"cq\":%d,\"me\":%d,\"rq\":%d,"
                         "\"si\":%" PRIu32 ",\"sl\":%u,\"fq\":%" PRIu32 "}",
                         first ? "" : ",", e.seq, e.epoch_secs, esc, e.decode_id, e.snr,
                         e.offset_hz, e.dt_centis / 100.0, e.is_cq ? 1 : 0, e.is_to_me ? 1 : 0,
                         e.is_recent_qso ? 1 : 0, e.slot_idx, (unsigned)e.slot_len_ds, e.dial_hz);
            break;
        case ENTRY_QUEUE_ENTRY:
            json_escape(e.dxcall, esc_call, sizeof(esc_call));
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":2,\"eid\":%u,"
                         "\"st\":%u,\"rc\":%u,\"rl\":%u,\"x\":\"%s\"}",
                         first ? "" : ",", e.seq, e.epoch_secs, e.entry_id, e.state,
                         e.retry_count, e.retry_limit, esc_call);
            break;
        case ENTRY_SLOT_STATE:
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":3,"
                         "\"sp\":%u,\"bm\":%u,\"hz\":%u}",
                         first ? "" : ",", e.seq, e.epoch_secs, e.slot_parity, e.beacon_mode,
                         e.offset_hz);
            break;
        case ENTRY_TX_HUD: {
            char esc_reason[PORTA_TX_HUD_REASON_MAX * 6 + 1];
            json_escape(e.tx_reason, esc_reason, sizeof(esc_reason));
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":4,"
                         "\"active\":%d,\"aborted\":%d,\"wrblk\":%d,"
                         "\"pw\":%d,\"swr\":%d,\"batt\":%d,\"hz\":%u,\"reason\":\"%s\",\"x\":\"%s\"}",
                         first ? "" : ",", e.seq, e.epoch_secs,
                         e.tx_active ? 1 : 0, e.tx_aborted ? 1 : 0, e.tx_writes_blocked ? 1 : 0,
                         e.tx_power_dw, e.tx_swr_c, e.tx_battery_pct, e.offset_hz, esc_reason, esc);
            break;
        }
        case ENTRY_USB_STATUS:
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":5,"
                         "\"present\":%d,\"kind\":%u,\"vid\":%u,\"pid\":%u}",
                         first ? "" : ",", e.seq, e.epoch_secs,
                         e.usb_present ? 1 : 0, e.usb_kind, e.usb_vid, e.usb_pid);
            break;
        case ENTRY_LOG:
        default:
            n = snprintf(row, sizeof(row),
                         "%s{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":0,\"x\":\"%s\"}",
                         first ? "" : ",", e.seq, e.epoch_secs, esc);
            break;
        }
        if (n > 0) {
            httpd_resp_sendstr_chunk(req, row);
            first = false;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    xSemaphoreGive(s_lock);

    char tail[48];
    snprintf(tail, sizeof(tail), "],\"seq\":%" PRIu32 "}", head);
    httpd_resp_sendstr_chunk(req, tail);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Last reply the host sent to an action, so the browser learns whether its
// clock actually landed. One outstanding action at a time is all the polled
// design allows, so a single slot is enough and a verb is enough to match it.
// CONFIG_GET/SET reuse the same slot with verb = PORTA_MSG_CONFIG_*.
static volatile uint8_t s_last_reply_verb;
static volatile bool s_last_reply_ok;
static char s_last_reply_reason[PORTA_EVENT_TEXT_MAX + 1];

// How many times wait_action_reply() has had to resend a CONFIG_GET because
// nothing answered halfway through the wait -- see that function. Relayed to
// the browser alongside the CRC/frame counters (sidekick_facts_json()) so a
// flaky link shows up as a number, not a vague slowness the operator can't
// attribute to anything.
static volatile uint32_t s_config_get_retries;

// Port A receive decoder. porta_rx_task() owns it; the HTTP side reads its
// counters for /api/config. Plain 32-bit reads of diagnostic counters, so no
// lock: a count one frame stale is still the right answer to "how healthy".
static porta_decoder_t s_rx_dec;

// CONFIG_GET all: VALUE frames arrive before the ACK; gather into JSON here.
#define CONFIG_JSON_MAX 3072
static char s_config_json[CONFIG_JSON_MAX];
static size_t s_config_json_len;
static bool s_config_json_first;
static volatile bool s_config_gathering;

static void config_json_begin(void)
{
    s_config_gathering = true;
    s_config_json_first = true;
    s_config_json[0] = '{';
    s_config_json[1] = '\0';
    s_config_json_len = 1;
}

static void config_json_add(const char *key, const char *value)
{
    if (!s_config_gathering || !key || !value) {
        return;
    }
    // Static scratch: this runs on the porta_rx task (4 KB stack). Escaping
    // both key and value on the stack overflowed when the host replied.
    static char esc_k[PORTA_CONFIG_KEY_MAX * 6 + 1];
    static char esc_v[PORTA_CONFIG_VALUE_MAX * 6 + 1];
    static char piece[sizeof(esc_k) + sizeof(esc_v) + 8];
    json_escape(key, esc_k, sizeof(esc_k));
    json_escape(value, esc_v, sizeof(esc_v));
    const int n = snprintf(piece, sizeof(piece), "%s\"%s\":\"%s\"",
                           s_config_json_first ? "" : ",", esc_k, esc_v);
    if (n <= 0 || s_config_json_len + (size_t)n + 2 >= CONFIG_JSON_MAX) {
        return;
    }
    memcpy(s_config_json + s_config_json_len, piece, (size_t)n + 1);
    s_config_json_len += (size_t)n;
    s_config_json_first = false;
}

// Ends gathering when the ACK arrives. The object is left open: the reply
// closes it after appending the sidekick's own facts (send_config_reply()),
// so nothing here is still writing the buffer once the ACK is visible.
static void config_json_stop(void)
{
    s_config_gathering = false;
}

// The sidekick's facts about itself, as `"key":"value"` pairs with no braces.
// None depends on the main board, so every /api/config reply carries them,
// a failed one included: that is when "which firmware is this sidekick
// running" matters most. Built at reply time on the httpd task.
//   sk_crc_errors / sk_frames_ok  the main board -> sidekick direction, which
//                                 the main board cannot see; its own
//                                 counterparts are porta_crc_errors/frames_ok
//   sk_ring_gaps / sk_config_retries  the browser-facing side of this file
static size_t sidekick_facts_json(char *out, size_t cap)
{
    char num[4][12];
    snprintf(num[0], sizeof(num[0]), "%" PRIu32, s_rx_dec.crc_errors);
    snprintf(num[1], sizeof(num[1]), "%" PRIu32, s_rx_dec.frames_ok);
    snprintf(num[2], sizeof(num[2]), "%" PRIu32, s_ring_gap_events);
    snprintf(num[3], sizeof(num[3]), "%" PRIu32, s_config_get_retries);
    const struct {
        const char *key;
        const char *value;
    } facts[] = {
        { "sk_crc_errors", num[0] },
        { "sk_frames_ok", num[1] },
        { "sk_ring_gaps", num[2] },
        { "sk_config_retries", num[3] },
        { "sk_wifi_mode", wifi_prov_ap_active() ? "ap" : "station" },
        { "sk_network_name", wifi_prov_network_name() },
        { "sk_ip", wifi_prov_ip_str() },
        { "sk_uptime", wifi_prov_uptime_str() },
        { "sk_fw_version", wifi_prov_fw_version() },
    };
    static char esc[PORTA_CONFIG_VALUE_MAX * 6 + 1];  // httpd task only
    size_t w = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); ++i) {
        json_escape(facts[i].value, esc, sizeof(esc));
        const int n = snprintf(out + w, cap - w, "%s\"%s\":\"%s\"",
                               w ? "," : "", facts[i].key, esc);
        if (n < 0 || (size_t)n >= cap - w) {
            out[w] = '\0';  // drop the fact that did not fit, keep the rest valid
            break;
        }
        w += (size_t)n;
    }
    return w;
}

// Sends a /api/config reply with the sidekick's facts appended: the main
// board's config when it answered (why == NULL; s_config_json, left open by
// config_json_stop()), or {"ok":false,"why":...} when it did not.
static esp_err_t send_config_reply(httpd_req_t *req, const char *why)
{
    // Static: the httpd task serves one request at a time, and its stack is
    // better spent elsewhere.
    static char facts[1024];
    static char esc[PORTA_EVENT_TEXT_MAX * 6 + 1];
    static char head[sizeof(esc) + 32];
    const size_t facts_len = sidekick_facts_json(facts, sizeof(facts));
    httpd_resp_set_type(req, "application/json");
    if (why) {
        json_escape(why, esc, sizeof(esc));
        snprintf(head, sizeof(head), "{\"ok\":false,\"why\":\"%s\"%s", esc, facts_len ? "," : "");
        httpd_resp_sendstr_chunk(req, head);
    } else {
        httpd_resp_send_chunk(req, s_config_json, s_config_json_len);
        if (s_config_json_len > 1 && facts_len) {
            httpd_resp_sendstr_chunk(req, ",");
        }
    }
    // A zero-length chunk would end the response, so only send a real one.
    if (facts_len) {
        httpd_resp_send_chunk(req, facts, facts_len);
    }
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

// FILE_LIST / FILE_READ (I28d, docs/architecture/webapp.md (Screens)): the reply is a burst of
// FILE_DATA rows followed by ACK, same shape as CONFIG_GET-all above --
// gathered here into a complete JSON response body, so wait_action_reply()
// can send it as-is once the ACK for PORTA_MSG_FILE_LIST/PORTA_MSG_FILE_READ
// arrives. Each buffer starts as its own complete `{"ok":true,...` wrapper,
// same trick config_json_begin() uses, so there is nothing to assemble later.
#define FILE_LIST_JSON_MAX 2048
static char s_file_list_json[FILE_LIST_JSON_MAX];
static size_t s_file_list_json_len;
static bool s_file_list_json_first;
static volatile bool s_file_list_gathering;

static void file_list_json_begin(void)
{
    s_file_list_gathering = true;
    s_file_list_json_first = true;
    strcpy(s_file_list_json, "{\"ok\":true,\"files\":[");
    s_file_list_json_len = strlen(s_file_list_json);
}

static void file_list_json_add(const char *name)
{
    if (!s_file_list_gathering || !name) {
        return;
    }
    // Static scratch, same reasoning as file_entries_json_add() below and
    // config_json_add() above: porta_rx task, 4 KB stack. Smaller than
    // either of those today (~400 B), but the same shape of landmine --
    // fixed alongside them rather than left for the next field to grow.
    static char esc[PORTA_FILENAME_MAX * 6 + 1];
    json_escape(name, esc, sizeof(esc));
    static char piece[sizeof(esc) + 8];
    const int n = snprintf(piece, sizeof(piece), "%s\"%s\"",
                           s_file_list_json_first ? "" : ",", esc);
    if (n <= 0 || s_file_list_json_len + (size_t)n + 3 >= FILE_LIST_JSON_MAX) {
        return;
    }
    memcpy(s_file_list_json + s_file_list_json_len, piece, (size_t)n + 1);
    s_file_list_json_len += (size_t)n;
    s_file_list_json_first = false;
}

static void file_list_json_end(void)
{
    if (!s_file_list_gathering) {
        return;
    }
    if (s_file_list_json_len + 3 < FILE_LIST_JSON_MAX) {
        s_file_list_json[s_file_list_json_len++] = ']';
        s_file_list_json[s_file_list_json_len++] = '}';
        s_file_list_json[s_file_list_json_len] = '\0';
    }
    s_file_list_gathering = false;
}

#define FILE_ENTRIES_JSON_MAX 4096
static char s_file_entries_json[FILE_ENTRIES_JSON_MAX];
static size_t s_file_entries_json_len;
static bool s_file_entries_json_first;
static volatile bool s_file_entries_gathering;

static void file_entries_json_begin(void)
{
    s_file_entries_gathering = true;
    s_file_entries_json_first = true;
    strcpy(s_file_entries_json, "{\"ok\":true,\"entries\":[");
    s_file_entries_json_len = strlen(s_file_entries_json);
}

// rst_sent/rst_rcvd pass through -99 (no report) as-is -- the same sentinel
// the wire carries, decoded by the browser rather than reinterpreted here.
static void file_entries_json_add(const porta_qso_entry_row_t *e)
{
    if (!s_file_entries_gathering || !e) {
        return;
    }
    // Static scratch: this runs on the porta_rx task (4 KB stack), same as
    // config_json_add() above -- these buffers were stack-local (like that
    // function's were before its own fix) and, with mode/station_callsign
    // added on top of the original six fields, pushed this function's frame
    // to ~1.7 KB, enough on its own to overflow the task stack once the rest
    // of the call chain is counted. Field-found 2026-09-15: a live
    // GET /api/log/entries crashed porta_rx with a stack overflow.
    static char esc_band[PORTA_BAND_MAX * 6 + 1];
    static char esc_call[PORTA_CALLSIGN_MAX * 6 + 1];
    static char esc_grid[PORTA_GRID_MAX * 6 + 1];
    static char esc_freq[PORTA_FREQ_MAX * 6 + 1];
    static char esc_my_grid[PORTA_GRID_MAX * 6 + 1];
    static char esc_comment[PORTA_COMMENT_MAX * 6 + 1];
    static char esc_mode[PORTA_MODE_MAX * 6 + 1];
    static char esc_station_callsign[PORTA_CALLSIGN_MAX * 6 + 1];
    static char esc_my_pota_ref[PORTA_POTA_REF_MAX * 6 + 1];
    static char esc_my_sota_ref[PORTA_SOTA_REF_MAX * 6 + 1];
    json_escape(e->band, esc_band, sizeof(esc_band));
    json_escape(e->call, esc_call, sizeof(esc_call));
    json_escape(e->grid, esc_grid, sizeof(esc_grid));
    json_escape(e->freq, esc_freq, sizeof(esc_freq));
    json_escape(e->my_grid, esc_my_grid, sizeof(esc_my_grid));
    json_escape(e->comment, esc_comment, sizeof(esc_comment));
    json_escape(e->mode, esc_mode, sizeof(esc_mode));
    json_escape(e->station_callsign, esc_station_callsign, sizeof(esc_station_callsign));
    json_escape(e->my_pota_ref, esc_my_pota_ref, sizeof(esc_my_pota_ref));
    json_escape(e->my_sota_ref, esc_my_sota_ref, sizeof(esc_my_sota_ref));
    static char piece[sizeof(esc_band) + sizeof(esc_call) + sizeof(esc_grid) + sizeof(esc_freq) +
              sizeof(esc_my_grid) + sizeof(esc_comment) + sizeof(esc_mode) +
              sizeof(esc_station_callsign) + sizeof(esc_my_pota_ref) + sizeof(esc_my_sota_ref) + 160];
    const int n = snprintf(piece, sizeof(piece),
                           "%s{\"time\":\"%s\",\"band\":\"%s\",\"call\":\"%s\","
                           "\"rst_sent\":%d,\"rst_rcvd\":%d,\"grid\":\"%s\",\"freq\":\"%s\","
                           "\"my_grid\":\"%s\",\"comment\":\"%s\",\"mode\":\"%s\","
                           "\"station_callsign\":\"%s\",\"my_pota_ref\":\"%s\","
                           "\"my_sota_ref\":\"%s\"}",
                           s_file_entries_json_first ? "" : ",", e->time_on, esc_band,
                           esc_call, e->rst_sent, e->rst_rcvd, esc_grid, esc_freq,
                           esc_my_grid, esc_comment, esc_mode, esc_station_callsign,
                           esc_my_pota_ref, esc_my_sota_ref);
    if (n <= 0 || s_file_entries_json_len + (size_t)n + 3 >= FILE_ENTRIES_JSON_MAX) {
        return;
    }
    memcpy(s_file_entries_json + s_file_entries_json_len, piece, (size_t)n + 1);
    s_file_entries_json_len += (size_t)n;
    s_file_entries_json_first = false;
}

static void file_entries_json_end(void)
{
    if (!s_file_entries_gathering) {
        return;
    }
    if (s_file_entries_json_len + 3 < FILE_ENTRIES_JSON_MAX) {
        s_file_entries_json[s_file_entries_json_len++] = ']';
        s_file_entries_json[s_file_entries_json_len++] = '}';
        s_file_entries_json[s_file_entries_json_len] = '\0';
    }
    s_file_entries_gathering = false;
}

// Wait briefly for the host's ACK/NAK. One frame over a 115200 link answered
// from the main loop, so this is milliseconds -- but the browser should be
// told what happened rather than assuming success.
static esp_err_t wait_action_reply(httpd_req_t *req, uint8_t verb)
{
    // CONFIG_GET-all can enqueue many VALUE frames (one per tick on the host),
    // so allow longer than a single ACTION. CONNECT may wait on USB enum.
    // FILE_LIST/FILE_READ are genuinely async on the host (a background
    // listing, or a file read paced across many main-loop ticks -- see
    // porta_host_file_read_begin()'s comment in main.cpp), so they get the
    // same generous budget as CONFIG_GET-all rather than a single ACTION's.
    int tries = 40;
    switch (verb) {
    case PORTA_MSG_CONFIG_GET:
    case PORTA_MSG_FILE_LIST:
    case PORTA_MSG_FILE_READ:
        tries = 200;
        break;
    case PORTA_ACT_CONNECT:
        tries = 120;
        break;
    default:
        break;
    }

    // CONFIG_GET is a pure read -- resending it duplicates nothing, unlike an
    // ACTION verb (TUNE, QUEUE_REPLY, ...) where a duplicate could double up
    // a real side effect on the radio, so only this verb gets an automatic
    // retry. FILE_LIST/FILE_READ are excluded even though they share this
    // verb's longer budget: they are genuinely async on the host (a paced
    // background listing/read), so resending would restart that from
    // scratch rather than just re-ask for a lost ACK. Halfway through the
    // wait, not at the end: a lost request or a lost ACK/VALUE frame (CRC
    // failure, or the ADV's outbound queue dropping it under load -- exactly
    // what the link-health counters below exist to measure) then costs one
    // extra round-trip instead of the whole timeout.
    const int retry_at = tries / 2;
    for (int i = 0; i < tries && s_last_reply_verb != verb; ++i) {
        if (i == retry_at && verb == PORTA_MSG_CONFIG_GET) {
            s_config_get_retries++;
            config_json_begin();  // discard whatever the first attempt gathered
            uint8_t frame[PORTA_PROTO_MAX_FRAME];
            const size_t n = porta_proto_encode_config_get("", frame, sizeof(frame));
            porta_send(frame, n);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    httpd_resp_set_type(req, "application/json");
    if (s_last_reply_verb != verb) {
        s_config_gathering = false;
        s_file_list_gathering = false;
        s_file_entries_gathering = false;
        ESP_LOGW(TAG, "no ACK for verb 0x%02x", verb);
        if (verb == PORTA_MSG_CONFIG_GET) {
            return send_config_reply(req, "no reply from host");
        }
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"no reply from host\"}");
    }
    if (s_last_reply_ok) {
        if (verb == PORTA_MSG_CONFIG_GET) {
            return send_config_reply(req, NULL);
        }
        if (verb == PORTA_MSG_FILE_LIST) {
            return httpd_resp_send(req, s_file_list_json, s_file_list_json_len);
        }
        if (verb == PORTA_MSG_FILE_READ) {
            return httpd_resp_send(req, s_file_entries_json, s_file_entries_json_len);
        }
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (verb == PORTA_MSG_CONFIG_GET) {
        return send_config_reply(req, s_last_reply_reason);
    }
    char out[PORTA_EVENT_TEXT_MAX + 32];
    snprintf(out, sizeof(out), "{\"ok\":false,\"why\":\"%s\"}", s_last_reply_reason);
    return httpd_resp_sendstr(req, out);
}

// POST /api/time  body: epoch_ms
//
// The browser is the clock source in a headless build -- it is the only device
// present that knows the time without internet, which is the case that matters:
// a cold radio on a summit will not decode until UTC is right, and NTP is
// unavailable exactly there. The host decides whether to act; if it is already
// correct it says so and changes nothing.
static esp_err_t post_time(httpd_req_t *req)
{
    char body[48] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }

    const long long epoch_ms = strtoll(body, NULL, 10);
    if (epoch_ms <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time");
        return ESP_FAIL;
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_set_clock((uint32_t)(epoch_ms / 1000),
                                                  (uint16_t)(epoch_ms % 1000),
                                                  frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_SET_CLOCK);
}

// POST /api/tx/cancel — abort in-flight TX / clear an armed pending TX.
static esp_err_t post_tx_cancel(httpd_req_t *req)
{
    // Drain any body so a client that POSTs with one does not leave bytes on
    // the socket for the next request on this keep-alive connection.
    char discard[64];
    while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_tx_cancel(frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_TX_CANCEL);
}

// POST /api/radio/connect — STATUS → 2 (start UAC + CAT sync). Token required.
static esp_err_t post_radio_connect(httpd_req_t *req)
{
    char discard[64];
    while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_connect(frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_CONNECT);
}

// POST /api/radio/tune — body "1"/"0" or "on"/"off". Token required (keys TX).
static esp_err_t post_radio_tune(httpd_req_t *req)
{
    char body[16] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    while (received > 0) {
        const char c = body[received - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') {
            break;
        }
        body[--received] = '\0';
    }
    bool on = false;
    if (strcmp(body, "1") == 0 || strcmp(body, "on") == 0 || strcmp(body, "true") == 0) {
        on = true;
    } else if (strcmp(body, "0") == 0 || strcmp(body, "off") == 0 ||
               strcmp(body, "false") == 0) {
        on = false;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "want 0 or 1");
        return ESP_FAIL;
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_tune(on, frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_TUNE);
}

// POST /api/beacon  body: "0"/"1"/"2" or "off"/"even"/"odd" (case-insensitive)
// -- BeaconMode's own wire values, matching STATUS key 1's three-way cycle.
// Token required: this starts/stops transmitting.
static esp_err_t post_beacon(httpd_req_t *req)
{
    char body[16] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    while (received > 0) {
        const char c = body[received - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') {
            break;
        }
        body[--received] = '\0';
    }
    uint8_t mode;
    if (strcmp(body, "0") == 0 || strcasecmp(body, "off") == 0) {
        mode = 0;
    } else if (strcmp(body, "1") == 0 || strcasecmp(body, "even") == 0) {
        mode = 1;
    } else if (strcmp(body, "2") == 0 || strcasecmp(body, "odd") == 0) {
        mode = 2;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "want 0/1/2 or off/even/odd");
        return ESP_FAIL;
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_beacon(mode, frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_BEACON);
}

// POST /api/queue/cancel  body: entry_id (decimal). Not a queue position --
// see porta_queue_entry_event_t in porta_proto.h for why. Token required.
static esp_err_t post_queue_cancel(httpd_req_t *req)
{
    char body[16] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    body[received] = '\0';
    const unsigned long id = strtoul(body, NULL, 10);
    if (id == 0 || id > 0xFFFFu) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad entry_id");
        return ESP_FAIL;
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_queue_cancel((uint16_t)id, frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_QUEUE_CANCEL);
}

// POST /api/queue/reply  body: decode_id (decimal), naming a decode the
// browser was shown rather than re-sending its text. Token required: this
// can key the transmitter.
static esp_err_t post_queue_reply(httpd_req_t *req)
{
    char body[16] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    body[received] = '\0';
    const unsigned long id = strtoul(body, NULL, 10);
    if (id == 0 || id > 0xFFFFFFFFu) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad decode_id");
        return ESP_FAIL;
    }

    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_queue_reply((uint32_t)id, frame, sizeof(frame));
    s_last_reply_verb = 0;
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_QUEUE_REPLY);
}

static bool query_uint(httpd_req_t *req, const char *key, unsigned long dflt, unsigned long *out)
{
    *out = dflt;
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char val[16];
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    *out = strtoul(val, NULL, 10);
    return true;
}

// GET /api/log/files?skip=&take=  -- day-file names, newest first, paged.
// Open read: QSO log browsing is the same category docs/architecture/system.md (Who may key the transmitter) already opened up for
// the viewer and the event feed.
static esp_err_t get_log_files(httpd_req_t *req)
{
    unsigned long skip = 0, take = 30;
    query_uint(req, "skip", 0, &skip);
    query_uint(req, "take", 30, &take);
    if (take == 0 || take > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad take");
        return ESP_FAIL;
    }

    file_list_json_begin();
    s_last_reply_verb = 0;
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_file_list_req(PORTA_FILE_LIST_QSO_DAILY,
                                                       (uint16_t)skip, (uint8_t)take,
                                                       frame, sizeof(frame));
    if (!porta_send(frame, n)) {
        s_file_list_gathering = false;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_MSG_FILE_LIST);
}

// GET /api/log/entries?file=&skip=&take=  -- parsed QSO rows from one day
// file, paged the same way. Open read, same reasoning as get_log_files.
static esp_err_t get_log_entries(httpd_req_t *req)
{
    char query[96];
    char filename[PORTA_FILENAME_MAX + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    if (filename[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file");
        return ESP_FAIL;
    }
    unsigned long skip = 0, take = 6;
    query_uint(req, "skip", 0, &skip);
    query_uint(req, "take", 6, &take);
    if (take == 0 || take > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad take");
        return ESP_FAIL;
    }

    file_entries_json_begin();
    s_last_reply_verb = 0;
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_file_read_req(filename, (uint16_t)skip, (uint8_t)take,
                                                       frame, sizeof(frame));
    if (n == 0) {
        s_file_entries_gathering = false;
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad filename");
        return ESP_FAIL;
    }
    if (!porta_send(frame, n)) {
        s_file_entries_gathering = false;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_MSG_FILE_READ);
}

// DELETE /api/log/files[?file=] — clear all daily QSO logs, or one basename.
// Token required: destructive, same category as TX/config writes. Empty
// query matches the phone "Clear all ADI files" button (host LOCK still
// protects the active day file during TX/decode).
static esp_err_t delete_log_files(httpd_req_t *req)
{
    char filename[PORTA_FILENAME_MAX + 1] = {0};
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }

    s_last_reply_verb = 0;
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_file_delete(filename, frame, sizeof(frame));
    if (n == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad filename");
        return ESP_FAIL;
    }
    if (!porta_send(frame, n)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
    }
    return wait_action_reply(req, PORTA_ACT_FILE_DELETE);
}

// GET /api/config — Station.txt surface as JSON, plus the sidekick's own
// facts on every reply (send_config_reply()). Open read: call/grid are on
// the air; WiFi secrets are not in this object.
static esp_err_t get_config(httpd_req_t *req)
{
    config_json_begin();
    s_last_reply_verb = 0;
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_config_get("", frame, sizeof(frame));
    if (!porta_send(frame, n)) {
        s_config_gathering = false;
        return send_config_reply(req, "host link busy");
    }
    return wait_action_reply(req, PORTA_MSG_CONFIG_GET);
}

// PUT /api/config  body: one or more Station.txt lines (key=value\n).
// Token required: this changes the station.
//
// A key the host refuses does not stop the keys after it: the refusals are
// independent, and some mean "saved, but" (band_sel's "VFO not moved" while
// TX is active). Stopping there silently dropped every later key, so a save
// made mid-QSO never applied autoseq_max_retry. The first refusal is still
// reported. A link failure does stop, since later keys would fail the same way.
static esp_err_t put_config(httpd_req_t *req)
{
    char body[1024];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    int applied = 0;
    int refused = 0;
    char first_why[PORTA_EVENT_TEXT_MAX + 1] = "";
    char *line = body;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len > 0) {
            char *eq = strchr(line, '=');
            if (!eq || eq == line) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad line");
                return ESP_FAIL;
            }
            *eq = '\0';
            const char *key = line;
            const char *value = eq + 1;
            uint8_t frame[PORTA_PROTO_MAX_FRAME];
            const size_t n = porta_proto_encode_config_set(key, value, frame, sizeof(frame));
            if (n == 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key/value");
                return ESP_FAIL;
            }
            s_last_reply_verb = 0;
            if (!porta_send(frame, n)) {
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"host link busy\"}");
            }
            for (int i = 0; i < 40 && s_last_reply_verb != PORTA_MSG_CONFIG_SET; ++i) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            if (s_last_reply_verb != PORTA_MSG_CONFIG_SET) {
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"no reply from host\"}");
            }
            if (!s_last_reply_ok) {
                if (refused++ == 0) {
                    memcpy(first_why, s_last_reply_reason, sizeof(first_why));
                }
            } else {
                ++applied;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (refused > 0) {
        char out[PORTA_EVENT_TEXT_MAX + 64];
        snprintf(out, sizeof(out), "{\"ok\":false,\"why\":\"%s\"}", first_why);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, out);
    }
    if (applied == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Its own task rather than a poll in the beacon loop: events arrive
// continuously, and a second of latency would make a live viewer useless quite
// apart from overrunning the RX buffer.
static void porta_rx_task(void *arg)
{
    (void)arg;
    porta_decoder_init(&s_rx_dec);

    uint8_t buf[256];
    porta_frame_t frame;
    char text[PORTA_EVENT_TEXT_MAX + 1];
    porta_decode_event_t ev;
    porta_queue_entry_event_t qe;
    porta_slot_state_event_t ss;
    porta_tx_hud_event_t hud;
    porta_usb_status_event_t usb;
    char file_name[PORTA_FILENAME_MAX + 1];
    porta_qso_entry_row_t qrow;
    uint32_t reported_crc = 0;

    while (1) {
        const int n = uart_read_bytes(PORTA_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; ++i) {
            if (!porta_decoder_push(&s_rx_dec, buf[i], &frame)) {
                continue;
            }
            entry_t e = {0};
            if (porta_proto_parse_log(&frame, &e.epoch_secs, text)) {
                e.kind = ENTRY_LOG;
                strncpy(e.text, text, sizeof(e.text) - 1);
                // Tag "adv" so USB-C logs are not mistaken for sidekick ESP_LOG.
                ESP_LOGI("adv", "%s", e.text);
                ring_push(&e);
            } else if (porta_proto_parse_decode(&frame, &ev)) {
                e.kind = ENTRY_DECODE;
                e.epoch_secs = ev.epoch_secs;
                e.decode_id = ev.decode_id;
                strncpy(e.text, ev.text, sizeof(e.text) - 1);
                e.snr = ev.snr;
                e.offset_hz = ev.offset_hz;
                e.dt_centis = ev.dt_centis;
                e.is_cq = ev.is_cq;
                e.is_to_me = ev.is_to_me;
                e.is_recent_qso = ev.is_recent_qso;
                e.slot_idx = ev.slot_idx;
                e.slot_len_ds = ev.slot_len_ds;
                e.dial_hz = ev.dial_hz;
                ESP_LOGI("adv", "decode %+d dB %4u Hz %+.2f s %s%s%s%s",
                         e.snr, e.offset_hz, e.dt_centis / 100.0,
                         e.is_to_me ? "[me] " : "", e.is_cq ? "[cq] " : "",
                         e.is_recent_qso ? "[qso] " : "", e.text);
                ring_push(&e);
            } else if (porta_proto_parse_queue_entry(&frame, &qe)) {
                e.kind = ENTRY_QUEUE_ENTRY;
                e.epoch_secs = qe.epoch_secs;
                e.entry_id = qe.entry_id;
                e.state = qe.state;
                e.retry_count = qe.retry_count;
                e.retry_limit = qe.retry_limit;
                strncpy(e.dxcall, qe.dxcall, sizeof(e.dxcall) - 1);
                ring_push(&e);
            } else if (porta_proto_parse_slot_state(&frame, &ss)) {
                e.kind = ENTRY_SLOT_STATE;
                e.epoch_secs = ss.epoch_secs;
                e.slot_parity = ss.slot_parity;
                e.beacon_mode = ss.beacon_mode;
                e.offset_hz = ss.resolved_offset_hz;
                ring_push(&e);
            } else if (porta_proto_parse_tx_hud(&frame, &hud)) {
                e.kind = ENTRY_TX_HUD;
                e.epoch_secs = hud.epoch_secs;
                e.tx_active = hud.active;
                e.tx_aborted = hud.aborted;
                e.tx_writes_blocked = hud.writes_blocked;
                e.tx_power_dw = hud.power_dw;
                e.tx_swr_c = hud.swr_c;
                e.tx_battery_pct = hud.battery_pct;
                e.offset_hz = hud.offset_hz;
                strncpy(e.tx_reason, hud.reason, sizeof(e.tx_reason) - 1);
                strncpy(e.text, hud.text, sizeof(e.text) - 1);
                ring_push(&e);
            } else if (porta_proto_parse_usb_status(&frame, &usb)) {
                e.kind = ENTRY_USB_STATUS;
                e.epoch_secs = usb.epoch_secs;
                e.usb_present = usb.present;
                e.usb_kind = usb.kind;
                e.usb_vid = usb.vid;
                e.usb_pid = usb.pid;
                ESP_LOGI("adv", "USB %s: kind=%u vid=%04x pid=%04x",
                         usb.present ? "attach" : "detach", usb.kind, usb.vid, usb.pid);
                ring_push(&e);
            } else if (porta_proto_parse_file_name_row(&frame, file_name)) {
                file_list_json_add(file_name);
            } else if (porta_proto_parse_file_entry_row(&frame, &qrow)) {
                file_entries_json_add(&qrow);
            } else {
                char cfg_key[PORTA_CONFIG_KEY_MAX + 1];
                char cfg_val[PORTA_CONFIG_VALUE_MAX + 1];
                if (porta_proto_parse_config_value(&frame, cfg_key, cfg_val)) {
                    config_json_add(cfg_key, cfg_val);
                } else if (porta_proto_parse_ack(&frame, (uint8_t *)&s_last_reply_verb)) {
                    switch (s_last_reply_verb) {
                    case PORTA_MSG_CONFIG_GET:
                        // The sidekick's own facts are added by the HTTP side
                        // at reply time (send_config_reply()), not here.
                        config_json_stop();
                        break;
                    case PORTA_MSG_FILE_LIST:
                        file_list_json_end();
                        break;
                    case PORTA_MSG_FILE_READ:
                        file_entries_json_end();
                        break;
                    default:
                        break;
                    }
                    s_last_reply_ok = true;
                    s_last_reply_reason[0] = '\0';
                } else if (porta_proto_parse_nak(&frame, (uint8_t *)&s_last_reply_verb,
                                                 s_last_reply_reason)) {
                    s_config_gathering = false;
                    s_file_list_gathering = false;
                    s_file_entries_gathering = false;
                    s_last_reply_ok = false;
                    ESP_LOGW("adv", "refused action 0x%02x: %s",
                             s_last_reply_verb, s_last_reply_reason);
                } else {
                    ESP_LOGW(TAG, "frame type 0x%02x len %u (no handler yet)",
                             frame.type, frame.len);
                }
            }
        }
        if (s_rx_dec.crc_errors != reported_crc) {
            reported_crc = s_rx_dec.crc_errors;
            ESP_LOGW(TAG, "link: %" PRIu32 " CRC errors, %" PRIu32 " frames ok",
                     s_rx_dec.crc_errors, s_rx_dec.frames_ok);
        }
    }
}

void host_link_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xTaskCreate(porta_rx_task, "porta_rx", 4096, NULL, 5, NULL);
}

void host_link_register_uris(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    static const httpd_uri_t events = {
        .uri = "/api/events", .method = HTTP_GET, .handler = get_events,
    };
    static const httpd_uri_t settime = {
        .uri = "/api/time", .method = HTTP_POST, .handler = post_time,
    };
    static const httpd_uri_t tx_cancel = {
        .uri = "/api/tx/cancel", .method = HTTP_POST, .handler = post_tx_cancel,
    };
    static const httpd_uri_t config_get = {
        .uri = "/api/config", .method = HTTP_GET, .handler = get_config,
    };
    static const httpd_uri_t config_put = {
        .uri = "/api/config", .method = HTTP_PUT, .handler = put_config,
    };
    static const httpd_uri_t radio_connect = {
        .uri = "/api/radio/connect", .method = HTTP_POST, .handler = post_radio_connect,
    };
    static const httpd_uri_t radio_tune = {
        .uri = "/api/radio/tune", .method = HTTP_POST, .handler = post_radio_tune,
    };
    static const httpd_uri_t beacon = {
        .uri = "/api/beacon", .method = HTTP_POST, .handler = post_beacon,
    };
    static const httpd_uri_t queue_cancel = {
        .uri = "/api/queue/cancel", .method = HTTP_POST, .handler = post_queue_cancel,
    };
    static const httpd_uri_t queue_reply = {
        .uri = "/api/queue/reply", .method = HTTP_POST, .handler = post_queue_reply,
    };
    static const httpd_uri_t log_files = {
        .uri = "/api/log/files", .method = HTTP_GET, .handler = get_log_files,
    };
    static const httpd_uri_t log_files_delete = {
        .uri = "/api/log/files", .method = HTTP_DELETE, .handler = delete_log_files,
    };
    static const httpd_uri_t log_entries = {
        .uri = "/api/log/entries", .method = HTTP_GET, .handler = get_log_entries,
    };
    // The event feed is a read of radio data: open on principle, since anyone
    // may listen to what is on the air. Setting the host clock or keying the
    // transmitter changes the device, so those need the token. Station config
    // read is open (call/grid are on the air); writes are guarded. Connect /
    // tune are writes (UAC + CAT / TX tone). Beacon / queue cancel / queue
    // reply all change device state (docs/architecture/webapp.md (Screens)) -- guarded like
    // connect/tune. Log files/entries are QSO-log reads, the same category
    // docs/architecture/system.md (Who may key the transmitter) already opened up for the event feed. Log delete is destructive —
    // PAIRING_REQUIRED. (/log's own raw viewer.html is retired -- app.html's
    // Operate view is the same event feed, same design system.)
    const struct {
        const httpd_uri_t *uri;
        pairing_policy_t policy;
    } routes[] = {
        { &events, PAIRING_OPEN },
        { &settime, PAIRING_REQUIRED },
        { &tx_cancel, PAIRING_REQUIRED },
        { &config_get, PAIRING_OPEN },
        { &config_put, PAIRING_REQUIRED },
        { &radio_connect, PAIRING_REQUIRED },
        { &radio_tune, PAIRING_REQUIRED },
        { &beacon, PAIRING_REQUIRED },
        { &queue_cancel, PAIRING_REQUIRED },
        { &queue_reply, PAIRING_REQUIRED },
        { &log_files, PAIRING_OPEN },
        { &log_files_delete, PAIRING_REQUIRED },
        { &log_entries, PAIRING_OPEN },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == PAIRING_ROUTES_HOST_LINK,
                   "host_link route count");
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t err = pairing_http_register(server, routes[i].uri, routes[i].policy);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri->uri, esp_err_to_name(err));
        }
    }
}
