#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "adif_delete.h"
#include "adif_logger.h"
#include "autoseq.h"
#include "decode_pipeline.h"
#include "decode_tx_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_list.h"
#include "porta.h"
#include "porta_proto.h"
#include "protocol.h"
#include "qmx_transport.h"
#include "qso_browse.h"
#include "radio_control.h"
#include "rtc_core.h"
#include "station.h"
#include "storage_service.h"
#include "tx_scheduler.h"

#include <ctime>

// B67–B70: Station.txt + Connect/VFO + CAT TX + Operate reply/cancel/FT/tune.

static const char *TAG = "porta_host";

extern std::string g_call;

static StationSettings s_station;
static bool s_seeded = false;
static bool s_tune = false;

extern const ProtocolConfig *g_protocol;
extern volatile bool g_tx_view_dirty;
extern AdifLoggerDedupe s_adif_logger_dedupe;

// Same FT8 table as ADV main.cpp's g_bands initializer.
static const char *kBandNames[kStationBandCount] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m",
};
static const float kFt8DefaultKhz[kStationBandCount] = {
    1840.f, 3573.f, 5357.f, 7074.f, 10136.f, 14074.f,
    18100.f, 21074.f, 24915.f, 28074.f, 50313.f, 144174.f,
};

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
    // Never retune under a transmission: a TX cancel lands on the scheduler's
    // next tick, so the app's cancel-then-change-band can still find one on air.
    if (decode_tx_state_get_tx_active()) {
        ESP_LOGW(TAG, "VFO sync skipped (%s): TX active", reason ? reason : "");
        return "TX active";
    }
    radio_control_end_tx();  // RX mode first, as the ADV does (idempotent)
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

const char *porta_p4_sync_vfo(const char *reason) {
    ensure_seeded();
    return sync_vfo(reason);
}

static bool log_adif_entry(const std::string &dxcall, const std::string &dxgrid, int rst_sent,
                           int rst_rcvd) {
    ensure_seeded();
    AdifLogQsoInput in;
    in.dxcall = dxcall;
    in.dxgrid = dxgrid;
    in.rst_sent = rst_sent;
    in.rst_rcvd = rst_rcvd;
    in.station_callsign = s_station.call;
    in.my_gridsquare = grid_ft8_4(s_station.grid);
    in.mode = g_protocol ? g_protocol->name : "FT8";
    char freq_str[16];
    // station_freq_hz is Hz; ADIF wants MHz (e.g. 14.074).
    snprintf(freq_str, sizeof(freq_str), "%.3f", (double)station_freq_hz() / 1e6);
    in.freq_mhz = freq_str;
    // Comment macros (/Radio, /Grid) stay ADV-only for now; raw Station comment.
    in.comment = s_station.comment1;
    return adif_log_qso(in, &s_adif_logger_dedupe);
}

void porta_p4_adif_register(void) {
    autoseq_set_adif_callback(log_adif_entry);
    autoseq_set_recent_qso_callback(decode_pipeline_recently_logged);
    ESP_LOGI(TAG, "ADIF archive callback registered");
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

    // Same fields as the ADV's porta_queue_state_tick(): the armed TX's slot
    // parity and its resolved offset, 0 when nothing is armed. The app shows
    // that offset as "TX: … Hz" and places the passband cursor with it.
    int armed_offset_hz = 0;
    const bool armed = tx_scheduler_hud_snapshot(nullptr, &armed_offset_hz);
    porta_emit_slot_state((uint8_t)(tx_scheduler_target_parity() & 1), tx_scheduler_beacon_mode(),
                          (uint16_t)(armed ? armed_offset_hz : 0));

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

// ADV main.cpp porta_tx_hud_tick sibling — phone Operate panel.
// B72: poll PC;/SW; so power/SWR are real (ADV draw_tx_hud polls; this board
// has no on-device banner, so the Port A tick owns the poll).
void porta_p4_tx_hud_tick(void) {
    static uint32_t s_last_ms = 0;
    static bool s_last_active = false;

    const bool active = tx_scheduler_hud_active() || s_tune;
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
    if (!tx_scheduler_hud_snapshot(&tx_text, &offset_hz) && s_tune) {
        tx_text = "TUNE";
        offset_hz = (s_station.offset_src == 1) ? s_station.offset_hz : 1500;
    }

    radio_control_poll_tx_power_swr();
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
             "audio_rx=%d\nclock_set=%d\n",
             qmx_audio_ready() ? 1 : 0, radio_control_ready() ? 1 : 0, s_tune ? 1 : 0, band_name,
             freq_khz, qmx_audio_ready() ? 1 : 0, rtc_valid ? 1 : 0);
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
            return strcmp(why, "TX active") == 0 ? "band saved, VFO not moved (TX active)"
                                                 : "band saved, VFO not moved (CAT not ready)";
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

const char *porta_host_tune(bool on) {
    ensure_seeded();
    if (on && !radio_control_ready()) {
        return "CAT not ready";
    }
    s_tune = on;
    if (radio_control_ready()) {
        const int freq_hz = station_freq_hz();
        const int tune_hz =
            (s_station.offset_src == 1) ? s_station.offset_hz : 1500;  // 1=CURSOR
        if (on) {
            radio_control_reset_tx_power_swr();
        }
        if (radio_control_set_tune(s_tune, freq_hz, tune_hz) != ESP_OK) {
            return "CAT tune failed";
        }
        ESP_LOGI(TAG, "CAT tune: %s", s_tune ? "TX" : "RX");
    } else {
        ESP_LOGI(TAG, "CAT tune: off (no CAT)");
    }
    return nullptr;
}

const char *porta_host_beacon(uint8_t mode) {
    if (mode > 2) {
        return "unknown beacon mode";
    }
    tx_scheduler_set_beacon(mode);
    return nullptr;
}

static const char *apply_reply_touch(const UiRxLine &msg) {
    const AutoseqTouchResult touch =
        autoseq_on_touch(msg, decode_tx_state_get_tx_active());
    if (touch == AutoseqTouchResult::IgnoredInProgress) {
        g_tx_view_dirty = true;
        return "QSO in progress";
    }
    if (touch == AutoseqTouchResult::NoRoom) {
        return "TX queue full";
    }
    AutoseqTxEntry pending{};
    if (!decode_tx_state_get_tx_active() && autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
    }
    g_tx_view_dirty = true;
    return nullptr;
}

const char *porta_host_queue_cancel(uint16_t entry_id) {
    if (!autoseq_drop_by_entry_id(entry_id)) {
        return "unknown entry";
    }
    g_tx_view_dirty = true;
    ESP_LOGI(TAG, "queue cancel entry_id=%u", (unsigned)entry_id);
    return nullptr;
}

const char *porta_host_queue_reply(uint32_t decode_id) {
    UiRxLine msg;
    if (!find_decode_for_reply(decode_id, &msg)) {
        return "unknown decode";
    }
    const char *why = apply_reply_touch(msg);
    if (why) {
        ESP_LOGI(TAG, "queue reply decode_id=%lu: %s", (unsigned long)decode_id, why);
        return why;
    }
    ESP_LOGI(TAG, "queue reply decode_id=%lu ok", (unsigned long)decode_id);
    return nullptr;
}

const char *porta_host_file_list_begin(uint8_t kind, uint16_t skip, uint8_t take) {
    if (kind != PORTA_FILE_LIST_QSO_DAILY) {
        return "unknown kind";
    }
    if (take == 0) {
        return "take must be > 0";
    }
    // Sync list: P4 has no on-device Q/Delete drain to race, and LittleFS
    // day-file counts stay small. Burst-then-ACK matches ADV's Port A shape.
    std::vector<std::string> all_names;
    if (!storage_file_list(all_names)) {
        FileListFail fail = FileListFail::Failed;
        switch (storage_service_owner()) {
            case StorageOwner::TRANSITION:
                fail = FileListFail::Busy;
                break;
            case StorageOwner::UNAVAILABLE:
                fail = FileListFail::Unavailable;
                break;
            case StorageOwner::FIRMWARE:
                fail = FileListFail::Failed;
                break;
        }
        return file_list_fail_text(fail);
    }
    std::vector<std::string> files;
    std::vector<std::string> lines_unused;
    file_list_apply(FileListKind::QsoDaily, FileListFail::None, all_names, &files, &lines_unused);
    for (size_t i = skip; i < files.size() && (i - skip) < take; ++i) {
        porta_emit_file_name(files[i].c_str());
    }
    porta_emit_ack(PORTA_MSG_FILE_LIST);
    ESP_LOGI(TAG, "FILE_LIST qso daily names=%u skip=%u take=%u", (unsigned)files.size(),
             (unsigned)skip, (unsigned)take);
    return nullptr;
}

namespace {

std::string today_qso_file_name() {
    time_t now = static_cast<time_t>(rtc_now_ms() / 1000);
    struct tm t {};
    localtime_r(&now, &t);
    char name[20];
    std::snprintf(name, sizeof(name), "%04d%02d%02d.adi", (t.tm_year + 1900) % 10000,
                  (t.tm_mon + 1) % 100, t.tm_mday % 100);
    return name;
}

bool p4_reject_active_log_mutation(const char *basename) {
    if (!basename || basename[0] == '\0') {
        return false;
    }
    if (!decode_tx_state_get_tx_active() && !decode_tx_state_get_decode_in_progress()) {
        return false;
    }
    return today_qso_file_name() == basename;
}

}  // namespace

const char *porta_host_file_delete(const char *filename) {
    return adif_delete_daily(filename, p4_reject_active_log_mutation);
}

namespace {

constexpr int kQsoBrowseLinesPerTick = 16;

StorageStream *s_porta_read_stream = nullptr;
QsoBrowsePager s_porta_read_pager;
std::vector<QsoBrowseBand> s_porta_read_bands;
bool s_porta_read_active = false;

void fill_browse_bands(void) {
    ensure_seeded();
    s_porta_read_bands.clear();
    s_porta_read_bands.reserve(kStationBandCount);
    for (int i = 0; i < kStationBandCount; ++i) {
        const float khz =
            (s_station.band_freq_set[i] ? s_station.band_freq[i] : kFt8DefaultKhz[i]);
        s_porta_read_bands.push_back({kBandNames[i], khz});
    }
}

}  // namespace

const char *porta_host_file_read_begin(const char *filename, uint16_t skip, uint8_t take) {
    if (!filename || filename[0] == '\0' || take == 0) {
        return "malformed";
    }
    if (s_porta_read_stream) {
        storage_stream_close(s_porta_read_stream);
        s_porta_read_stream = nullptr;
        s_porta_read_active = false;
    }
    s_porta_read_stream = storage_stream_open(filename, StorageOpenMode::READ);
    if (!s_porta_read_stream) {
        return "open failed";
    }
    qso_browse_pager_reset(&s_porta_read_pager, skip, take);
    fill_browse_bands();
    s_porta_read_active = true;
    ESP_LOGI(TAG, "FILE_READ begin %s skip=%u take=%u", filename, (unsigned)skip,
             (unsigned)take);
    return nullptr;
}

void porta_p4_file_tick(void) {
    if (!s_porta_read_active || !s_porta_read_stream) {
        return;
    }
    char line[512];
    int n = 0;
    bool more = true;
    while (n < kQsoBrowseLinesPerTick) {
        if (!storage_stream_read_line(s_porta_read_stream, line, sizeof(line))) {
            more = false;
            break;
        }
        ++n;
        if (!qso_browse_pager_feed(&s_porta_read_pager, std::string(line),
                                   s_porta_read_bands.data(),
                                   static_cast<int>(s_porta_read_bands.size()))) {
            more = false;
            break;
        }
    }
    if (more) {
        return;
    }

    storage_stream_close(s_porta_read_stream);
    s_porta_read_stream = nullptr;
    s_porta_read_active = false;

    for (const QsoLogEntry &e : s_porta_read_pager.entries) {
        porta_qso_entry_row_t row = {};
        std::snprintf(row.time_on, sizeof(row.time_on), "%s", e.time_on.c_str());
        std::snprintf(row.band, sizeof(row.band), "%s", e.band.c_str());
        std::snprintf(row.call, sizeof(row.call), "%s", e.call.c_str());
        row.rst_sent = (int8_t)(e.has_rst_sent ? e.rst_sent : -99);
        row.rst_rcvd = (int8_t)(e.has_rst_rcvd ? e.rst_rcvd : -99);
        std::snprintf(row.grid, sizeof(row.grid), "%s", e.grid.c_str());
        std::snprintf(row.freq, sizeof(row.freq), "%s", e.freq.c_str());
        std::snprintf(row.my_grid, sizeof(row.my_grid), "%s", e.my_grid.c_str());
        std::snprintf(row.comment, sizeof(row.comment), "%s", e.comment.c_str());
        std::snprintf(row.mode, sizeof(row.mode), "%s", e.mode.c_str());
        std::snprintf(row.station_callsign, sizeof(row.station_callsign), "%s",
                      e.station_callsign.c_str());
        porta_emit_file_entry(row);
    }
    porta_emit_ack(PORTA_MSG_FILE_READ);
    ESP_LOGI(TAG, "FILE_READ done entries=%u", (unsigned)s_porta_read_pager.entries.size());
}
