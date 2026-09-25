#include "wifi_prov.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "host_link.h"
#include "mdns.h"
#include "pairing_http.h"
#include "pairing_http_cap.h"
#include "bundle_host.h"
#include "web_bundle.h"
#include "web_page.h"
#include "web_fs.h"

static const char *TAG = "wifi_prov";

#define NVS_NAMESPACE "sidekick"
#define NVS_KEY_SSID  "wifi_ssid"
#define NVS_KEY_PASS  "wifi_pass"
// B55: set by the station status page's "Enter AP mode" button (post_ap_mode,
// below), read and cleared once at the top of wifi_prov_start(). One-shot by
// construction -- consumed before anything else runs, so it can never leave
// the device stuck re-forcing AP mode after an operator has since rejoined a
// network through the portal's own WiFi form.
#define NVS_KEY_FORCE_AP "force_ap"

// WPA2 maxima, plus a NUL. Sized from the spec rather than guessed: an SSID is
// up to 32 bytes and a PSK up to 63 characters.
#define SSID_MAX 32
#define PASS_MAX 64

// Join attempts before giving up and raising the AP. Each retry costs a couple
// of seconds; the point is to fall back promptly rather than leave an operator
// staring at a device that looks dead but is quietly retrying forever. This is
// the *initial* boot-time join only -- an operator is typically present for
// that one, so fast feedback matters more than patience. See
// RECONNECT_MAX_RETRY for the very different case of losing an
// already-established connection.
#define JOIN_MAX_RETRY 5

// Retries before falling back to the AP after losing a connection that was
// already up and running -- deliberately much more patient than
// JOIN_MAX_RETRY, since the operator is very unlikely to be standing there
// watching a mid-session router reboot (which can easily take a minute or two)
// the way they are for an initial join. Not measured against a real router
// reboot's timing -- a reasonable guess, not a tuned number.
#define RECONNECT_MAX_RETRY 20

// Upper bound on the whole join attempt, retries included. This is a backstop,
// not the normal exit: every ordinary outcome sets one of the event bits. It
// exists because the alternative -- waiting forever -- turns any lost or
// unhandled event into a device that sits there with no AP, no station, and
// no way back in. The finished product has no serial console to explain that.
#define JOIN_TIMEOUT_MS (60 * 1000)

static EventGroupHandle_t s_events;
#define BIT_GOT_IP   BIT0
#define BIT_JOIN_FAIL BIT1

static int  s_retries;
static bool s_online;
// Field-found 2026-09-21: once joined, a sustained loss of the network (router
// reboot, moved out of range) retried JOIN_MAX_RETRY times and then just
// stopped -- BIT_JOIN_FAIL was set, but nothing was still waiting on it (that
// wait belongs to try_join()'s one call at boot, long since returned), so the
// device went permanently offline with no AP fallback until a manual power
// cycle. s_was_online, once true, is never reset -- it distinguishes "still
// trying the initial join" (existing behavior: give up, let wifi_prov_start()
// fall back to the AP) from "was connected, lost it" (new: fall back to the
// AP directly, since nothing is still waiting on BIT_JOIN_FAIL for this case).
static bool s_was_online;
static bool s_ap_active;
static char s_ssid[SSID_MAX + 1];
static httpd_handle_t s_httpd;

// Created at most once each. esp_netif_create_default_wifi_*() asserts on a
// duplicate if_key rather than returning an error, so calling it twice is a
// panic, not a failure to handle. That is exactly what happened on the
// stored-credentials-failed path: try_join() created the STA netif, the join
// failed, and start_ap() created it again for APSTA scanning.
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static volatile int s_ap_clients;

static void ensure_sta_netif(void)
{
    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        // ESP-IDF's own LWIP default (CONFIG_LWIP_LOCAL_HOSTNAME) is the
        // literal string "espressif" -- that's what a router's client list
        // shows unless this is set explicitly. Reuses the same name mDNS
        // already answers to rather than inventing a second one.
        esp_netif_set_hostname(s_netif_sta, WIFI_PROV_MDNS_HOST);
    }
}

static void ensure_ap_netif(void)
{
    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
    }
}

// ---------------------------------------------------------------------------
// Stored credentials
// ---------------------------------------------------------------------------

static bool creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = ssid_len;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &n) == ESP_OK) && ssid[0] != '\0';
    if (ok) {
        n = pass_len;
        // A missing password is legitimate: open networks exist.
        if (nvs_get_str(h, NVS_KEY_PASS, pass, &n) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return ok;
}

static bool creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, NVS_KEY_SSID, ssid) == ESP_OK &&
              nvs_set_str(h, NVS_KEY_PASS, pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// Clears the stored credentials. With WIFI_STORAGE_RAM there is nothing else
// to clear -- that is the point of setting it.
static bool creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    (void)nvs_erase_key(h, NVS_KEY_SSID);
    (void)nvs_erase_key(h, NVS_KEY_PASS);
    const bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// B55: true exactly once per set -- clears the flag itself before returning,
// so a crash-loop or a second boot for any other reason cannot re-force AP
// mode from a stale flag left behind by this call's own failure to run.
static bool force_ap_flag_get_and_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t flag = 0;
    const bool set = nvs_get_u8(h, NVS_KEY_FORCE_AP, &flag) == ESP_OK && flag != 0;
    if (set) {
        (void)nvs_erase_key(h, NVS_KEY_FORCE_AP);
        (void)nvs_commit(h);
    }
    nvs_close(h);
    return set;
}

// ---------------------------------------------------------------------------
// WiFi events
// ---------------------------------------------------------------------------

static uint8_t s_last_disconnect_reason;

// True only while we are deliberately trying to join. The AP runs in APSTA so
// it can scan, and bringing that up fires WIFI_EVENT_STA_START just like a
// real join does -- without this the fallback AP would auto-connect to the
// network that had just failed, retry it forever, and take the radio off
// channel each time while an operator is trying to use the provisioning page.
static bool s_join_active;

// The handful of reasons an operator can actually act on. Anything else falls
// through to the raw code, which is still better than silence.
static const char *disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        // On a 2.4 GHz-only part this is very often a 5 GHz network. An
        // iPhone Personal Hotspot defaults to 5 GHz on recent handsets and
        // needs "Maximize Compatibility" turned on to be visible at all.
        return "network not found (5 GHz? this radio is 2.4 GHz only)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong password";
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth expired";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    default:
        return "see reason code";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_join_active) {
            esp_wifi_connect();
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)data;
        s_online = false;
        s_last_disconnect_reason = d ? d->reason : 0;
        if (!s_join_active) {
            return;  // APSTA station side idling alongside the AP; not a join
        }
        // The reason code is the difference between "wrong password" and
        // "that network is not reachable from here", which look identical
        // from the operator's side and have completely different fixes.
        const int max_retry = s_was_online ? RECONNECT_MAX_RETRY : JOIN_MAX_RETRY;
        if (s_retries < max_retry) {
            s_retries++;
            ESP_LOGW(TAG, "Join failed (%s), retry %d/%d",
                     disconnect_reason_text(s_last_disconnect_reason),
                     s_retries, max_retry);
            esp_wifi_connect();
        } else if (s_was_online) {
            // Was up and running, not still doing the initial join -- nothing
            // is waiting on BIT_JOIN_FAIL for this case (try_join()'s own wait
            // returned long ago), so falling back has to be driven from here
            // directly rather than through wifi_prov_start()'s existing
            // fall-through. Same restart-with-force_ap path Forget/Switch-to-
            // AP already use, not a live in-place mode switch -- the network
            // is already unusable at this point, so the brief interruption
            // costs nothing real.
            ESP_LOGE(TAG, "Lost '%s' and could not rejoin after %d tries: %s -- falling back to the AP",
                     s_ssid, max_retry, disconnect_reason_text(s_last_disconnect_reason));
            wifi_prov_enter_ap_and_restart();
        } else {
            ESP_LOGE(TAG, "Join failed after %d tries: %s", max_retry,
                     disconnect_reason_text(s_last_disconnect_reason));
            xEventGroupSetBits(s_events, BIT_JOIN_FAIL);
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Online as " IPSTR " on '%s'", IP2STR(&e->ip_info.ip), s_ssid);
        s_retries = 0;
        s_online = true;
        s_was_online = true;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

// Scan results are rendered into the form rather than fetched by script: no
// JS means no second request, no JSON parser on the device, and it still works
// in whatever browser a phone opens a captive portal in.
//
// The scan is taken once when the AP comes up and then cached, NOT run per
// request. Scanning in APSTA mode takes the radio off the AP's channel for a
// few hundred ms per channel, which can drop a connected phone — and doing
// that while serving the very page the phone asked for would look like "the
// page doesn't load", which is close to undiagnosable from the operator's
// side. /rescan exists for when the list is genuinely stale, and accepts the
// hiccup deliberately.
#define SCAN_MAX_APS 20
#define SCAN_BUF_LEN 2560

static char s_scan_options[SCAN_BUF_LEN];
static int  s_scan_count;

// HTML-escape into `out`. An SSID is arbitrary bytes chosen by someone else;
// dropping it into a page unescaped is how a neighbour's network name becomes
// script running on the operator's phone.

// Renders <option> rows for the networks in range, strongest first (the
// driver already sorts by RSSI). Returns the number found; 0 means the
// caller should fall back to a plain text field rather than an empty list.
static int render_scan_options(char *out, size_t out_len)
{
    wifi_scan_config_t scan = {.show_hidden = false};
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed");
        return 0;
    }
    uint16_t found = SCAN_MAX_APS;
    static wifi_ap_record_t records[SCAN_MAX_APS];
    if (esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) {
        return 0;
    }

    size_t w = 0;
    int shown = 0;
    for (uint16_t i = 0; i < found; ++i) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;  // hidden network: nothing useful to show or select
        }
        // Skip duplicates — the same SSID on 2.4 and 5 GHz, or a mesh with
        // several APs, otherwise fills the list with the same name repeatedly.
        bool dup = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp(ssid, (const char *)records[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        char esc[SSID_MAX * 6 + 1];
        if (!web_html_escape(ssid, esc, sizeof(esc))) {
            continue;  // absurdly long SSID; skip it rather than truncate markup
        }
        int n = snprintf(out + w, out_len - w,
                         "<option value=\"%s\">%s%s</option>",
                         esc, esc, records[i].authmode == WIFI_AUTH_OPEN ? " (open)" : "");
        if (n < 0 || (size_t)n >= out_len - w) {
            break;  // page buffer full; show what fitted
        }
        w += n;
        shown++;
    }
    ESP_LOGI(TAG, "Scan: %u found, %d listed", found, shown);
    return shown;
}

// Dropdown value meaning "not in the list". A real network could in principle
// be named this; the consequence is benign, since picking Other and typing the
// same string still joins it.
#define SSID_OTHER "__other__"

// Our own error response instead of httpd_resp_send_err(), which hardcodes
// Content-Type: text/html with no charset -- the same Latin-1 guess that
// mangled the pages above -- and is not overridable, because it sets the type
// itself after the handler has run. Error strings are the easiest place for
// this to go unnoticed: they are written once, seen rarely, and are exactly
// where a non-ASCII character ends up when the text explains something.
//
// Returns ESP_FAIL so the caller keeps the connection-closing behaviour it had
// before. That matters for the "too long" path, which rejects a request whose
// body it has not drained; leaving the socket open would have the remainder
// parsed as the next request.
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    const web_sub_t subs[] = {{.key = "message", .value = msg}};
    web_page_send_file(req, "error.html", subs, 1);
    return ESP_FAIL;
}

// Minimal application/x-www-form-urlencoded field extractor. httpd_query_key_value
// works on this encoding and handles the key matching; the only thing it leaves
// is percent- and plus-decoding, which a WiFi password very much needs.
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

// Refresh the cached list. Safe to call with a client connected, but it may
// briefly interrupt them — see the note above.
static void scan_refresh(void)
{
    s_scan_count = render_scan_options(s_scan_options, sizeof(s_scan_options));
}

// The cache goes stale in the obvious way: the AP comes up, then the operator
// turns on their phone hotspot, and the list they see predates it. Field-hit
// on the first bench run. Rescanning while a client is connected risks
// dropping them (APSTA scan takes the radio off channel), so this refreshes
// only while nobody is associated -- which is exactly the window before
// someone opens the page.
static void scan_keepfresh_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_ap_active || s_ap_clients > 0) {
            continue;
        }
        scan_refresh();
    }
}

static esp_err_t get_rescan(httpd_req_t *req)
{
    scan_refresh();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_form(httpd_req_t *req)
{
    // Static rather than automatic: the options alone are SCAN_BUF_LEN, and
    // the httpd task's stack is 4 KB. Safe because esp_http_server serves from
    // a single task, which is also what lets s_scan_options be shared.
    static char networks[SCAN_BUF_LEN + 384];
    char failure[256] = "";

    if (s_last_disconnect_reason != 0) {
        char why[192];
        if (web_html_escape(disconnect_reason_text(s_last_disconnect_reason), why, sizeof(why))) {
            snprintf(failure, sizeof(failure), "<p class=failure>Last attempt failed: %s.</p>", why);
        }
    }

    if (s_scan_count > 0) {
        // The Other… entry is last in the list because it is the exception:
        // an escape hatch for hidden or out-of-range networks. onchange
        // restored 2026-09-20: the "password field/submit button invisible
        // in Safari" bug that briefly cost this its onchange handler turned
        // out to be unrelated -- a stale CMake-computed embed size (see
        // web_seed_table.h) silently truncating the served page for every
        // client, not this JS. Always-showing the manual field instead had
        // its own real cost: picking a real network left an empty "Network
        // name" field sitting there with no indication whether it mattered.
        snprintf(networks, sizeof(networks),
                 "<label>Network</label><select name=ssid required autofocus "
                 "onchange=\"var b=document.getElementById('oth');"
                 "b.hidden=this.value!='" SSID_OTHER "';"
                 "if(!b.hidden)document.getElementById('othname').focus()\">"
                 "%s"
                 "<option value=\"" SSID_OTHER "\">Other…</option></select>",
                 s_scan_options);
    } else {
        // No scan results: a plain text field, since an empty dropdown would
        // offer the operator a choice of nothing.
        snprintf(networks, sizeof(networks),
                 "<label>Network</label><input name=ssid maxlength=32 required autofocus>");
    }

    // Both are markup assembled here, not operator or scan text: the failure
    // reason is escaped just above, and each SSID in the options is escaped as
    // it is rendered.
    const web_sub_t subs[] = {
        {.key = "failure",  .value = failure,  .already_html = true},
        {.key = "networks", .value = networks, .already_html = true},
    };
    return web_page_send_file(req, "provision.html", subs, 2);
}

static esp_err_t post_provision(httpd_req_t *req)
{
    char body[SSID_MAX + PASS_MAX + 32];
    int received = 0;
    if (req->content_len >= (int)sizeof(body)) {
        return send_error(req, "400 Bad Request", "That form submission was too large.");
    }
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char ssid[SSID_MAX + 1] = {0};
    char pass[PASS_MAX + 1] = {0};
    // A typed name wins over the dropdown: if the operator bothered to type
    // one, the listed network is not the one they want.
    char manual[SSID_MAX + 1] = {0};
    if (httpd_query_key_value(body, "ssid_manual", manual, sizeof(manual)) == ESP_OK) {
        url_decode(manual);
    }
    // The dropdown is the answer unless it says "Other", in which case the
    // revealed box is. When no networks were found at all the form has no
    // dropdown and posts the typed name as `ssid` directly, which lands in the
    // same place.
    char sel[SSID_MAX + 1] = {0};
    if (httpd_query_key_value(body, "ssid", sel, sizeof(sel)) == ESP_OK) {
        url_decode(sel);
    }
    if (sel[0] != '\0' && strcmp(sel, SSID_OTHER) != 0) {
        strncpy(ssid, sel, sizeof(ssid) - 1);
    } else {
        strncpy(ssid, manual, sizeof(ssid) - 1);
    }
    if (ssid[0] == '\0') {
        // Plain text: send_error() escapes its message, so markup here would
        // show as literal tags.
        return send_error(req, "400 Bad Request",
                          "Network required — pick one from the list, or choose "
                          "Other… and type a name.");
    }
    (void)httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(pass);

    if (!creds_save(ssid, pass)) {
        return send_error(req, "500 Internal Server Error",
                          "Could not store the credentials in flash.");
    }
    ESP_LOGI(TAG, "Stored credentials for '%s'", ssid);

    // Answer before rebooting: the phone is on our AP, and the AP goes away
    // the moment we switch to station mode. Telling the operator what will
    // happen is the difference between "it worked" and "it hung".
    web_page_send_file(req, "saved.html", NULL, 0);

    // Restart rather than switching mode in place: a clean boot re-runs the
    // stored-credentials path, so there is one join path to get right instead
    // of two.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // not reached
}

// Log misses in both modes. Without this, a wrong path is silent on USB.
static esp_err_t log_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ESP_LOGI(TAG, "%s %s -> 404",
             req->method == HTTP_GET ? "GET" :
             req->method == HTTP_POST ? "POST" :
             req->method == HTTP_PUT ? "PUT" : "?",
             req->uri ? req->uri : "");
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

static void httpd_start_provisioning(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // Default is 7 (4 usable client sockets after esp_http_server reserves 3)
    // -- too tight for a phone browser, which opens several connections
    // loading one page (doc + favicon + speculative preconnect). Raised
    // alongside CONFIG_LWIP_MAX_SOCKETS (sdkconfig.defaults) with headroom
    // left in that shared pool for mDNS/DHCP. Picked without an on-device
    // heap reading -- TEST_PLAN.md Pending.
    cfg.max_open_sockets = 10;
    // Match the compile-time pairing slot table (pairing_http_cap.h).
    cfg.max_uri_handlers = PAIRING_HTTP_MAX_ROUTES_AP;
    // Crashed with a stack overflow in the field 2026-09-20 without this:
    // this httpd now runs the same host_link/web_bundle/bundle_host routes
    // httpd_start_status() always has, and that function's own comment
    // already explains why they need it (manifest + mbedtls + VFS blew the
    // IDF default 4 KB stack on begin/commit) -- this config just never got
    // the same bump when those routes were added here for B55.
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t form = {
        .uri = "/", .method = HTTP_GET, .handler = get_form,
    };
    static const httpd_uri_t provision = {
        .uri = "/provision", .method = HTTP_POST, .handler = post_provision,
    };
    static const httpd_uri_t rescan = {
        .uri = "/rescan", .method = HTTP_GET, .handler = get_rescan,
    };
    // The form and a rescan are open: they change nothing stored. Saving
    // credentials is a config write, so it needs the token like every other
    // write (docs/architecture/system.md (Who may key the transmitter)). There is no chicken-and-egg:
    // pairing_http_init() mints the token on first boot, before any route is
    // registered, and the button discloses it here as anywhere. Being in radio
    // range is no gate -- this AP is open, and it is how the station runs in
    // the field (docs/architecture/system.md (Constraints)), not only a setup step. provision.html waits
    // for the button before it enables Save.
    const struct {
        const httpd_uri_t *uri;
        pairing_policy_t policy;
    } core[] = {
        { &form, PAIRING_OPEN },
        { &rescan, PAIRING_OPEN },
        { &provision, PAIRING_REQUIRED },
    };
    _Static_assert(sizeof(core) / sizeof(core[0]) == PAIRING_ROUTES_WIFI_PROV_AP_CORE,
                   "AP core route count");
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); ++i) {
        pairing_http_register(s_httpd, core[i].uri, core[i].policy);
    }
    pairing_http_register_disclosure(s_httpd);
    // B55 (docs/architecture/system.md (Constraints)): AP mode is a real operating mode now, not only a
    // bootstrap-to-provisioning state, so it registers the same application
    // routes station mode does -- each module applies its own per-route
    // policy (pairing_http.h).
    host_link_register_uris(s_httpd);
    web_bundle_register(s_httpd);
    bundle_host_register(s_httpd);
    web_fs_register_rehydrate(s_httpd);
    // Same error handler and static route station mode uses -- a missing
    // file 404s plainly, same in both modes now. B74 follow-up, 2026-09-21:
    // no more captive-portal redirect-on-miss here (removed with the rest of
    // the captive-portal logic, see the removed configure_ap_dhcp() and
    // web_fs.c's removed s_captive_satisfied machinery) -- it never reliably
    // resolved to "Done" on iOS regardless of the DHCP option 114 experiment,
    // and the AP's own SSID (micro-ft8.local) already tells the operator
    // exactly where to navigate manually.
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, log_not_found);
    // Static GET /* last so exact /api/... routes win.
    web_fs_register_static(s_httpd);
}

// Advertised in both modes. On the provisioning AP it is a convenience next to
// the captive portal; once joined it is the only way to reach the sidekick
// without hunting for a DHCP lease.
static void mdns_bring_up(void)
{
    static bool started = false;
    if (started) {
        return;  // mdns_init() twice is an error, and both modes call this
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    started = true;
    mdns_hostname_set(WIFI_PROV_MDNS_HOST);
    mdns_instance_name_set("Micro-FT8 sidekick");
    // Advertising the service as well as the hostname is what makes the device
    // show up in network browsers, not just resolve when you already know the
    // name.
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://" WIFI_PROV_MDNS_HOST ".local/");
}

// ---------------------------------------------------------------------------
// Station-mode status page
// ---------------------------------------------------------------------------

// Two largest non-zero units (days/hours, hours/minutes, minutes/seconds, or
// bare seconds) -- same coarse "how long" shape as the app's own fmtAgo/
// fmtDuration (webapp/app/app.html), so uptime here and "ago" there read the
// same way to an operator.
static void format_uptime(int64_t total_seconds, char *buf, size_t buf_len)
{
    if (total_seconds < 0) {
        total_seconds = 0;
    }
    int64_t days = total_seconds / 86400;
    int64_t hours = (total_seconds / 3600) % 24;
    int64_t mins = (total_seconds / 60) % 60;
    int64_t secs = total_seconds % 60;
    if (days > 0) {
        snprintf(buf, buf_len, "%lldd %lldh", (long long)days, (long long)hours);
    } else if (hours > 0) {
        snprintf(buf, buf_len, "%lldh %lldm", (long long)hours, (long long)mins);
    } else if (mins > 0) {
        snprintf(buf, buf_len, "%lldm %llds", (long long)mins, (long long)secs);
    } else {
        snprintf(buf, buf_len, "%llds", (long long)secs);
    }
}

// status.html is retired: the network info and actions it held (SSID, IP,
// firmware, uptime, Forget WiFi, Enter AP mode, Restore seed pages) now live
// in app.html's Settings > Network section -- same design system, same
// pairing gate, one app instead of a second bespoke unstyled page. "/" in
// station mode just hands off to the app -- Operate (the default view), not
// straight into Settings; landing on the app's main screen is the more
// useful default for a bare "/" visit than jumping into a settings page
// (operator call, 2026-09-21). app.html's own #network hash deep-link still
// works for anything that wants to link straight to Settings > Network.
static esp_err_t get_root_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/app.html");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_update(httpd_req_t *req)
{
    const web_sub_t subs[] = {
        {.key = "bundle_host", .value = bundle_host_get()},
    };
    return web_page_send_file(req, "update.html", subs, 1);
}

static esp_err_t post_forget(httpd_req_t *req)
{
    // Called by fetch() now rather than a form submit, so the page draws its
    // own confirmation and this only has to answer before the restart.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_prov_forget_and_restart();
    return ESP_OK;  // not reached
}

// B55: deliberate operator entry into AP mode from the station status page.
// Same "answer before the restart" reasoning as post_forget() -- the network
// this request arrived on is about to go away for the sidekick either way.
static esp_err_t post_ap_mode(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_prov_enter_ap_and_restart();
    return ESP_OK;  // not reached
}

static void httpd_start_status(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // See the matching comment in httpd_start_provisioning(): default 7
    // (4 usable) is too tight for a real browser session. TEST_PLAN.md
    // Pending -- no on-device heap reading behind this number yet.
    cfg.max_open_sockets = 10;
    // Capacity tracks pairing_http_cap.h (station sum). Bundle handlers also
    // need headroom above the IDF default 4 KB stack (manifest + mbedtls +
    // VFS blew it on begin/commit).
    cfg.max_uri_handlers = PAIRING_HTTP_MAX_ROUTES_STATION;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = get_root_redirect,
    };
    static const httpd_uri_t update = {
        .uri = "/update", .method = HTTP_GET, .handler = get_update,
    };
    static const httpd_uri_t forget = {
        .uri = "/forget", .method = HTTP_POST, .handler = post_forget,
    };
    static const httpd_uri_t ap_mode = {
        .uri = "/ap-mode", .method = HTTP_POST, .handler = post_ap_mode,
    };
    // The redirect is a plain GET, open on principle (it hands off to app.html's
    // own reads, which are open the same way). Forget and ap-mode both tear
    // down the working network the operator is reachable on, so both are the
    // clearest case of a write that needs authorizing.
    const struct {
        const httpd_uri_t *uri;
        pairing_policy_t policy;
    } core[] = {
        { &root, PAIRING_OPEN },
        { &update, PAIRING_OPEN },
        { &forget, PAIRING_REQUIRED },
        { &ap_mode, PAIRING_REQUIRED },
    };
    _Static_assert(sizeof(core) / sizeof(core[0]) == PAIRING_ROUTES_WIFI_PROV_STATION_CORE,
                   "station core route count");
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); ++i) {
        pairing_http_register(s_httpd, core[i].uri, core[i].policy);
    }
    pairing_http_register_disclosure(s_httpd);
    // Same application routes httpd_start_provisioning() now registers for AP
    // mode (B55, docs/architecture/system.md (Constraints)) -- kept as a separate, near-duplicate block
    // rather than factored out, because the two modes' *core* three routes
    // and open/guarded policy genuinely differ and a shared helper would just
    // hide that at a call site instead of removing it.
    host_link_register_uris(s_httpd);
    web_bundle_register(s_httpd);
    bundle_host_register(s_httpd);
    web_fs_register_rehydrate(s_httpd);
    // Static GET /* last so exact /api/... routes win.
    web_fs_register_static(s_httpd);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, log_not_found);
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

static void ap_name(char *out, size_t out_len)
{
    // Operator-requested 2026-09-20: the SSID itself hints at the URL to
    // visit (WIFI_PROV_MDNS_HOST, the same hostname mDNS already answers to)
    // rather than a name with no connection to what to do next. No MAC
    // suffix -- operator call 2026-09-20, overriding this file's own earlier
    // multi-device-disambiguation concern; two sidekicks in range of each
    // other would show identical SSIDs with this change.
    snprintf(out, out_len, WIFI_PROV_MDNS_HOST ".local");
}

static void start_ap(void)
{
    char ssid[33];
    ap_name(ssid, sizeof(ssid));

    ensure_ap_netif();
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.max_connection = 2;
    // Open, deliberately. A fixed password shipped in a public repo is not
    // security, it is the appearance of it, and provisioning only runs when
    // the device has no credentials. Anyone in radio range during that window
    // could set them — a real exposure, judged acceptable for a bench device
    // and recorded rather than glossed. docs/architecture/sidekick.md (Firmware updates).
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA, not AP: scanning requires a station interface, and in plain AP
    // mode esp_wifi_scan_start() fails. The station side stays unconnected —
    // it exists so the provisioning page can list what is in range.
    ensure_sta_netif();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep (the default) is a known trigger for CCMP replay-detection
    // and block-ack churn on some APs/clients -- the radio goes quiet between
    // beacons, and the wake transition can desync replay-counter tracking.
    // This board has no battery reason to sleep the radio (USB/ADV powered),
    // so there is no cost to paying for reliability instead. Must come after
    // esp_wifi_start(), not esp_wifi_init() -- the driver requires it.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_join_active = false;
    s_ap_active = true;
    // B74 follow-up, 2026-09-21: no more captive-portal DHCP option 114 or
    // DNS-hijack server here -- standard DHCP (router + DNS both offered,
    // esp_netif's own default). The captive-portal dance never reliably
    // resolved to "Done" on iOS and was the actual source of the cellular-
    // retention trade-off B55/B69 were fighting; dropping it removes both
    // problems at once. An operator navigates to micro-ft8.local manually --
    // the AP's own SSID already says exactly that.
    mdns_bring_up();
    // Scan now, while nothing is connected: the off-channel time cannot drop
    // a client that has not arrived yet.
    scan_refresh();
    httpd_start_provisioning();
    xTaskCreate(scan_keepfresh_task, "scan_fresh", 3072, NULL, 3, NULL);
    ESP_LOGW(TAG, "Provisioning AP '%s' up — open http://192.168.4.1/", ssid);
}

static bool try_join(const char *ssid, const char *pass)
{
    ensure_sta_netif();
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    // Before esp_wifi_start(), not after. Starting the driver posts
    // WIFI_EVENT_STA_START, and the event loop dispatches it from its own
    // higher-priority task -- which can run before this one reaches the next
    // statement. The STA_START handler only calls esp_wifi_connect() when this
    // flag is set (that gate is what stops the fallback AP's station side
    // re-attacking a network that already refused it), so setting it late lost
    // the race: no association was ever started, no event ever arrived, and
    // the wait below sat forever with nothing in the log after "Joining".
    s_join_active = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    // See the AP path's comment above -- same fix, station side. This is the
    // mode a field sidekick actually runs in day to day.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Joining '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_JOIN_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(JOIN_TIMEOUT_MS));
    const bool joined = (bits & BIT_GOT_IP) != 0;
    if (!joined) {
        if (bits == 0) {
            ESP_LOGE(TAG, "Join timed out after %d s with no result",
                     JOIN_TIMEOUT_MS / 1000);
        }
        s_join_active = false;  // stop the handler re-arming behind us
    }
    return joined;
}

void wifi_prov_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // After NVS and before any route is registered: the token is read from
    // NVS, and pairing_http_register() is what installs the guard. Not
    // ESP_ERROR_CHECK'd -- a sidekick that cannot mint a token should still
    // come up serving decodes, which are open anyway, and refuse writes.
    if (pairing_http_init() != ESP_OK) {
        ESP_LOGE(TAG, "Pairing unavailable — every guarded route will refuse");
    }
    if (bundle_host_init() != ESP_OK) {
        ESP_LOGE(TAG, "Bundle host URL unavailable — using compile default if possible");
    }

    // Before any HTTP handler: mount the web FS and hydrate seed pages from
    // the firmware embed (docs/architecture/sidekick.md (Web root)). Not ESP_ERROR_CHECK'd — a sidekick
    // that cannot mount still beacons; pages will 500 until flash is fixed.
    if (web_fs_init() != ESP_OK) {
        ESP_LOGE(TAG, "Web FS unavailable — bootstrap pages will fail");
    }

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    // One copy of the credentials, and it is ours. The driver defaults to
    // WIFI_STORAGE_FLASH, which quietly persists whatever is handed to
    // esp_wifi_set_config() into its own nvs.net80211 namespace -- a second
    // store this module neither writes nor clears. That makes "forget the
    // network" unimplementable by design: clearing the sidekick keys would
    // leave the driver's copy behind and the device would keep reconnecting
    // to a network it had just been told to forget. RAM storage makes
    // creds_load()/creds_save() the single source of truth.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, NULL, NULL));

    // B55: a deliberate operator request (the station status page's "Enter AP
    // mode" button, POST /ap-mode) goes straight to the AP, skipping
    // try_join() entirely -- this is not the join-failed fallback below, and
    // must not be confused with it. Credentials are kept, not erased: the
    // portal's own WiFi-join form is already a complete way back to station
    // mode with no separate "undo" path needed.
    if (force_ap_flag_get_and_clear()) {
        ESP_LOGW(TAG, "AP mode requested by operator — skipping station join");
        start_ap();
        return;
    }

    char ssid[SSID_MAX + 1] = {0};
    char pass[PASS_MAX + 1] = {0};
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
        if (try_join(ssid, pass)) {
            // Joined: serve the status page so the operator can see what it
            // connected to and change their mind, without a serial console.
            mdns_bring_up();
            httpd_start_status();
            return;
        }
        // Stored credentials that no longer work (moved, password changed).
        // Fall through to the AP rather than looping: the operator needs a way
        // back in, and it is the same way they got in the first time.
        ESP_LOGW(TAG, "Stored credentials did not work — falling back to the AP");
        s_ssid[0] = '\0';
        esp_wifi_stop();
    } else {
        ESP_LOGI(TAG, "No stored credentials");
    }
    start_ap();
}

bool wifi_prov_is_online(void) { return s_online; }
const char *wifi_prov_ssid(void) { return s_ssid; }
bool wifi_prov_ap_active(void) { return s_ap_active; }

const char *wifi_prov_network_name(void)
{
    static char buf[SSID_MAX + 1];
    if (s_ap_active) {
        ap_name(buf, sizeof(buf));
        return buf;
    }
    return s_ssid[0] ? s_ssid : "(unknown)";
}

const char *wifi_prov_ip_str(void)
{
    static char buf[16];
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *netif = s_ap_active ? s_netif_ap : s_netif_sta;
    if (netif) {
        esp_netif_get_ip_info(netif, &ip);
    }
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    return buf;
}

const char *wifi_prov_uptime_str(void)
{
    static char buf[24];
    format_uptime(esp_timer_get_time() / 1000000, buf, sizeof(buf));
    return buf;
}

const char *wifi_prov_fw_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "?";
}

void wifi_prov_forget_and_restart(void)
{
    if (creds_erase()) {
        ESP_LOGW(TAG, "Credentials forgotten — restarting into provisioning");
    } else {
        ESP_LOGE(TAG, "Could not clear credentials");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void wifi_prov_enter_ap_and_restart(void)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_u8(h, NVS_KEY_FORCE_AP, 1) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (ok) {
        ESP_LOGW(TAG, "AP mode requested — restarting");
    } else {
        ESP_LOGE(TAG, "Could not set AP-mode flag — restarting anyway, will retry station join");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
