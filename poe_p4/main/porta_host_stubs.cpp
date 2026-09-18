#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "autoseq.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "porta.h"
#include "qmx_transport.h"
#include "radio_control.h"
#include "rtc_core.h"
#include "station.h"
#include "storage_service.h"
#include "tx_scheduler.h"

// B67/B68: Station.txt + Connect/VFO + CAT tone TX. I29: LittleFS persist.

static const char *TAG = "porta_host";

extern std::string g_call;

static StationSettings s_station;
static bool s_seeded = false;

// Same FT8 table as ADV main.cpp's g_bands initializer.
static const char *kBandNames[kStationBandCount] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m",
};
static const float kFt8DefaultKhz[kStationBandCount] = {
    1840.f, 3573.f, 5357.f, 7074.f, 10136.f, 14074.f,
    18100.f, 21074.f, 24915.f, 28074.f, 50313.f, 144174.f,
};

static const char *clock_source_name(void) {
    switch (g_rtc_time_source) {
        case RtcTimeSource::ESP_RTC:
            return "esp_rtc";
        case RtcTimeSource::DS3231:
            return "ds3231";
        case RtcTimeSource::GPS:
            return "gps";
        case RtcTimeSource::MANUAL:
            return "manual";
        case RtcTimeSource::PHONE:
            return "phone";
        case RtcTimeSource::BROWSER:
            return "browser";
        case RtcTimeSource::SAVED:
        default:
            return "saved";
    }
}

static std::string grid_ft8_4(const std::string &grid) {
    if (grid.size() <= 4) {
        return grid;
    }
    return grid.substr(0, 4);
}

static void sync_autoseq_station(void) {
    autoseq_set_station(s_station.call, grid_ft8_4(s_station.grid));
    autoseq_set_skip_tx1(s_station.skip_tx1);
    autoseq_set_max_retry(s_station.autoseq_max_retry);
    AutoseqCqType cq = AutoseqCqType::CQ;
    if (s_station.cq_type >= 0 && s_station.cq_type <= 5) {
        cq = (AutoseqCqType)s_station.cq_type;
    }
    const std::string &ft =
        (cq == AutoseqCqType::FREETEXT || cq == AutoseqCqType::FD) ? s_station.free_text
                                                                  : s_station.cq_freetext;
    autoseq_set_cq_type(cq, ft);
}

static void station_seed_adv_defaults(void) {
    station_settings_init(&s_station);
    for (int i = 0; i < kStationBandCount; ++i) {
        s_station.band_freq[i] = kFt8DefaultKhz[i];
        s_station.band_freq_set[i] = true;
    }
    s_station.offset_hz = 1500;
    s_station.band_sel = 1;  // 80m
    s_station.cq_type = 0;
    s_station.cq_freetext = "FreeText";
    s_station.skip_tx1 = false;
    s_station.free_text = "TNX 73";
    s_station.call = "YOURCALL";
    s_station.grid = "CM97";
    s_station.offset_src = 0;  // RANDOM
    s_station.radio = 2;       // QMX (also station_settings_init)
    s_station.gps_baud = 115200;
    s_station.comment1 = "MicroFT8 /Radio";
    s_station.ignore_prefixes.clear();
    s_station.rxtx_log = true;
    s_station.active_bands = "80 40 30 20 17 15 12 10";
    s_station.autoseq_max_retry = 5;  // AUTOSEQ_MAX_RETRY
    s_station.protocol_ft4 = false;
    s_station.serialize_ft4_band_keys = false;
    g_call = s_station.call;
    s_seeded = true;
    sync_autoseq_station();
}

static void ensure_seeded(void) {
    if (!s_seeded) {
        station_seed_adv_defaults();
    }
}

static void persist_station(void) {
    if (!storage_service_firmware_available()) {
        ESP_LOGW(TAG, "Station persist skipped: storage not ready");
        return;
    }
    const std::string blob = station_serialize(s_station);
    if (!storage_file_write_atomic("Station.txt", blob)) {
        ESP_LOGE(TAG, "Station.txt write failed");
        return;
    }
    ESP_LOGI(TAG, "Station.txt saved (%u B)", (unsigned)blob.size());
}

static void load_station_from_storage(void) {
    station_seed_adv_defaults();
    if (!storage_service_firmware_available()) {
        ESP_LOGW(TAG, "storage not ready; using Station defaults");
        return;
    }
    std::string text;
    if (!storage_file_read_text("Station.txt", text) || text.empty()) {
        ESP_LOGI(TAG, "no Station.txt yet; using defaults");
        return;
    }
    station_parse(text, &s_station);
    s_station.serialize_ft4_band_keys = s_station.protocol_ft4;
    g_call = s_station.call;
    sync_autoseq_station();
    ESP_LOGI(TAG, "loaded Station.txt call=%s grid=%s", s_station.call.c_str(),
             s_station.grid.c_str());
}

static int station_freq_hz(void) {
    const int idx = s_station.band_sel;
    if (idx < 0 || idx >= kStationBandCount || !s_station.band_freq_set[idx]) {
        return 0;
    }
    return (int)(s_station.band_freq[idx] * 1000.0f + 0.5f);
}

// nullptr = VFO updated (or already at target with CAT up). Non-null = why not.
static const char *sync_vfo(const char *reason) {
    const int freq_hz = station_freq_hz();
    if (freq_hz <= 0) {
        return "no band frequency";
    }
    if (!radio_control_ready()) {
        ESP_LOGW(TAG, "VFO sync skipped (%s): CAT not ready (want %d Hz)",
                 reason ? reason : "", freq_hz);
        return "CAT not ready";
    }
    const esp_err_t err = radio_control_sync_frequency_mode(freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "VFO sync failed (%s) freq=%d: %s", reason ? reason : "", freq_hz,
                 esp_err_to_name(err));
        return "CAT sync failed";
    }
    ESP_LOGI(TAG, "VFO sync ok (%s) freq=%d", reason ? reason : "", freq_hz);
    return nullptr;
}

void porta_p4_station_boot(void) {
    load_station_from_storage();
}

int porta_p4_station_freq_hz(void) {
    ensure_seeded();
    return station_freq_hz();
}

int porta_p4_station_offset_hz(void) {
    ensure_seeded();
    return s_station.offset_hz;
}

int porta_p4_station_offset_src(void) {
    ensure_seeded();
    return s_station.offset_src;
}

// ADV's porta_queue_state_tick sibling: once a second, push SLOT_STATE and
// active QUEUE_ENTRY rows so Operate stays current without a snapshot call.
void porta_p4_companion_status_tick(void) {
    ensure_seeded();

    static uint32_t s_last_ms = 0;
    static uint16_t s_last_active_ids[AUTOSEQ_MAX_QUEUE];
    static int s_last_active_count = 0;

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_last_ms < 1000) {
        return;
    }
    s_last_ms = now_ms;

    uint8_t parity = 0;
    if (rtc_valid) {
        // FT8 15 s slots; FT4 would be 7.5 s — P4 is FT8-only until protocol_mode
        // actually switches the decode pipeline (later).
        parity = (uint8_t)((rtc_current_epoch_seconds() / 15) & 1);
    }
    porta_emit_slot_state(parity, tx_scheduler_beacon_mode(),
                          (uint16_t)s_station.offset_hz);

    uint16_t current_ids[AUTOSEQ_MAX_QUEUE];
    const int n = autoseq_active_count();
    int filled = 0;
    for (int i = 0; i < n && filled < AUTOSEQ_MAX_QUEUE; ++i) {
        QsoContext ctx;
        if (!autoseq_get_active_context(i, &ctx)) {
            continue;
        }
        current_ids[filled++] = ctx.entry_id;
        porta_emit_queue_entry(ctx.entry_id, (uint8_t)ctx.state, (uint8_t)ctx.retry_counter,
                               (uint8_t)ctx.retry_limit, ctx.dxcall.c_str());
    }
    for (int i = 0; i < s_last_active_count; ++i) {
        bool still_here = false;
        for (int j = 0; j < filled; ++j) {
            if (current_ids[j] == s_last_active_ids[i]) {
                still_here = true;
                break;
            }
        }
        if (!still_here) {
            porta_emit_queue_entry(s_last_active_ids[i], 6 /* IDLE */, 0, 0, "");
        }
    }
    memcpy(s_last_active_ids, current_ids, sizeof(uint16_t) * (size_t)filled);
    s_last_active_count = filled;
}

// ADV main.cpp porta_tx_hud_tick sibling — phone Operate panel. Power/SWR
// stay -1 until P4 grows CAT meter parsing (qmx_transport stubs meters today).
void porta_p4_tx_hud_tick(void) {
    static uint32_t s_last_ms = 0;
    static bool s_last_active = false;

    const bool active = tx_scheduler_hud_active();
    if (!active && !s_last_active) {
        return;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (active && (now_ms - s_last_ms < 500)) {
        return;
    }

    if (!active) {
        s_last_active = false;
        porta_emit_tx_hud(false, false, false, "", -1.f, -1.f, -1, 0, "");
        return;
    }

    s_last_ms = now_ms;
    s_last_active = true;

    const char *tx_text = "";
    int offset_hz = 0;
    (void)tx_scheduler_hud_snapshot(&tx_text, &offset_hz);

    float power_w = -1.f, swr = -1.f;
    (void)radio_control_get_tx_power_swr(&power_w, &swr);

    porta_emit_tx_hud(true, false, false, tx_text, power_w, swr, -1, offset_hz, "");
}

const char *porta_host_set_clock(uint32_t epoch_secs, uint16_t millis) {
    if (epoch_secs < 1700000000u) {
        return "implausible time";
    }
    const time_t now = rtc_current_epoch_seconds();
    const long drift = (long)((time_t)epoch_secs - now);
    if (rtc_valid && drift > -2 && drift < 2) {
        rtc_update_strings();
        g_rtc_time_source = RtcTimeSource::BROWSER;
        return nullptr;
    }
    rtc_seed_epoch((time_t)epoch_secs, esp_timer_get_time() / 1000 - (int64_t)millis,
                   RtcTimeSource::BROWSER);
    rtc_sync_to_esp_rtc();
    (void)rtc_write_external_from_soft("browser");
    ESP_LOGI(TAG, "clock set epoch=%lu (%+ld s)", (unsigned long)epoch_secs, drift);
    return nullptr;
}

const char *porta_host_tx_free(const char * /*text*/) {
    return "TX not wired on P4 yet";
}

void porta_host_tx_cancel(void) {
    tx_scheduler_cancel();
}

size_t porta_host_config_snapshot(char *out, size_t out_cap) {
    ensure_seeded();
    if (!out || out_cap < 2) {
        return 0;
    }

    std::string text = station_serialize(s_station);

    const int idx = s_station.band_sel;
    const char *band_name =
        (idx >= 0 && idx < kStationBandCount) ? kBandNames[idx] : "?";
    const int freq_khz =
        (idx >= 0 && idx < kStationBandCount && s_station.band_freq_set[idx])
            ? (int)(s_station.band_freq[idx] + 0.5f)
            : 0;

    char live[192];
    snprintf(live, sizeof(live),
             "streaming=%d\ncat_ready=%d\ntune=%d\nband_name=%s\nfreq_khz=%d\n"
             "audio_rx=%d\nclock_set=%d\nclock_source=%s\nbeacon=%u\n",
             qmx_audio_ready() ? 1 : 0, radio_control_ready() ? 1 : 0, 0, band_name, freq_khz,
             qmx_audio_ready() ? 1 : 0, rtc_valid ? 1 : 0, clock_source_name(),
             (unsigned)tx_scheduler_beacon_mode());
    text += live;

    if (text.size() + 1 > out_cap) {
        return 0;
    }
    memcpy(out, text.c_str(), text.size() + 1);
    return text.size();
}

const char *porta_host_config_set(const char *key, const char *value) {
    ensure_seeded();
    if (!key || !value) {
        return "malformed";
    }
    if (strcmp(key, "beacon") == 0) {
        tx_scheduler_set_beacon((uint8_t)atoi(value));
        ESP_LOGI(TAG, "config beacon=%u", (unsigned)tx_scheduler_beacon_mode());
        return nullptr;
    }
    if (!station_key_known(key)) {
        return "unknown key";
    }
    if (strlen(key) > 32 || strlen(value) > 200) {
        return "too long";
    }

    std::string line = std::string(key) + "=" + value + "\n";
    station_parse(line, &s_station);
    s_station.serialize_ft4_band_keys = s_station.protocol_ft4;
    g_call = s_station.call;

    if (strcmp(key, "call") == 0 || strcmp(key, "grid") == 0 || strcmp(key, "skiptx1") == 0 ||
        strcmp(key, "autoseq_max_retry") == 0 || strcmp(key, "cq_type") == 0 ||
        strcmp(key, "cq_ft") == 0 || strcmp(key, "free_text") == 0) {
        sync_autoseq_station();
    }

    persist_station();

    if (strcmp(key, "band_sel") == 0 || strcmp(key, "active_bands") == 0 ||
        strncmp(key, "band", 4) == 0 || strncmp(key, "ft4_band", 8) == 0) {
        const char *why = sync_vfo("porta config");
        // Match ADV: band_sel means "operate on this band now" — NAK if the
        // VFO did not move so the phone does not look saved while the radio
        // stayed put. Other band-ish keys stay silent on a sync miss.
        if (strcmp(key, "band_sel") == 0 && why) {
            ESP_LOGI(TAG, "config %s=%s (saved, VFO not moved: %s)", key, value, why);
            return "band saved, VFO not moved (CAT not ready)";
        }
    }

    ESP_LOGI(TAG, "config %s=%s", key, value);
    return nullptr;
}

const char *porta_host_connect(void) {
    // P4 brings USB host up at boot (qmx_transport_start); Connect is "push
    // the current station band to the QMX VFO", not "start the host stack".
    const char *why = sync_vfo("connect");
    if (why) {
        return why;
    }
    return nullptr;
}

const char *porta_host_tune(bool /*on*/) {
    return "tune not wired on P4 yet";
}

const char *porta_host_beacon(uint8_t mode) {
    tx_scheduler_set_beacon(mode);
    return nullptr;
}

const char *porta_host_queue_cancel(uint16_t /*entry_id*/) {
    return "queue not wired on P4 yet";
}

const char *porta_host_queue_reply(uint32_t /*decode_id*/) {
    return "queue not wired on P4 yet";
}

const char *porta_host_file_list_begin(uint8_t /*kind*/, uint16_t /*skip*/, uint8_t /*take*/) {
    return "file list not wired on P4 yet";
}

const char *porta_host_file_read_begin(const char * /*filename*/, uint16_t /*skip*/,
                                       uint8_t /*take*/) {
    return "file read not wired on P4 yet";
}
