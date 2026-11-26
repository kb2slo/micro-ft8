#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "adif_logger.h"
#include "amp_band_prime.h"
#include "app_name.h"
#include "autoseq.h"
#include "decode_pipeline.h"
#include "decode_sync.h"
#include "debug_log.h"
#include "decode_tx_state.h"
#include "dial_history.h"
#include "qmx_iq_channel.h"
#include "station_channel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hl2_transport.h"
#include "porta.h"
#include "porta_proto.h"
#include "swr_trip.h"
#include "porta_host.h"
#include "qmx_transport.h"
#include "queue_event.h"
#include "qso_browse.h"
#include "radio_select.h"
#include "radio_control.h"
#include "tx_ready.h"
#include "rtc_core.h"
#include "station.h"
#include "qso_log_store.h"
#include "storage_service.h"
#include "swr_sweep.h"
#include "tx_scheduler.h"

#include <ctime>

// B67–B70: Station.txt + Connect/VFO + CAT TX + Operate reply/cancel/FT/tune.

static const char *TAG = "porta_host";

extern std::string g_call;

static StationSettings s_station;
static bool s_seeded = false;
static bool s_tune = false;
static std::atomic<uint32_t> s_amp_prime_end_ms{0};

extern AdifLoggerDedupe s_adif_logger_dedupe;

// FT8 band names, in Station.txt band-index order.
static const char *kBandNames[kStationBandCount] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m",
};
static const float kFt8DefaultKhz[kStationBandCount] = {
    1840.f, 3573.f, 5357.f, 7074.f, 10136.f, 14074.f,
    18100.f, 21074.f, 24915.f, 28074.f, 50313.f, 144174.f,
};
// FT4's dials, as webapp/app/bands.js has them: where an FT4 channel beside FT8 listens
// (docs/architecture/decode.md, FT4).
static const float kFt4DefaultKhz[kStationBandCount] = {
    1843.f, 3575.f, 5357.f, 7047.5f, 10140.f, 14080.f,
    18104.f, 21140.f, 24919.f, 28180.f, 50318.f, 144170.f,
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
    autoseq_set_tries(s_station.tries);
    autoseq_set_cq_modifier(s_station.cq_mod);
}

// The station call: g_call for this loop, and a copy for the decode task,
// which never reads g_call (docs/architecture/main_board.md, P4).
static void set_station_call(void) {
    g_call = s_station.call;
    decode_pipeline_set_my_call(g_call);
}

static void station_seed_adv_defaults(void) {
    station_settings_init(&s_station);
    for (int i = 0; i < kStationBandCount; ++i) {
        s_station.band_freq[i] = kFt8DefaultKhz[i];
        s_station.band_freq_set[i] = true;
    }
    s_station.offset_hz = 1500;
    s_station.band_sel = 1;  // 80m
    s_station.cq_mod.clear();
    s_station.skip_tx1 = false;
    s_station.call = "YOURCALL";
    s_station.grid = "CM97";
    s_station.offset_src = 0;  // RANDOM
    s_station.radio = 2;       // QMX (also station_settings_init)
    s_station.gps_baud = 115200;
    s_station.comment1 = MICROFT8_APP_NAME " /Radio";
    s_station.active_bands = "80 40 30 20 17 15 12 10";
    s_station.tries = AUTOSEQ_TRIES;
    set_station_call();
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
    set_station_call();
    sync_autoseq_station();
    ESP_LOGI(TAG, "loaded Station.txt call=%s grid=%s", s_station.call.c_str(),
             s_station.grid.c_str());
}

// The operating band's FT8 dial, its bandN= frequency.
static int station_ft8_dial_hz(void) {
    const int idx = s_station.band_sel;
    if (idx < 0 || idx >= kStationBandCount || !s_station.band_freq_set[idx]) {
        return 0;
    }
    return (int)(s_station.band_freq[idx] * 1000.0f + 0.5f);
}

// Band `idx`'s FT4 dial minus its FT8 dial in `st`: its ft4band<N>, else FT4's default dial, against its band<N>, else
// FT8's default.
static int32_t band_ft4_minus_ft8_hz(const StationSettings &st, int idx) {
    if (idx < 0 || idx >= kStationBandCount) {
        return 0;
    }
    const double ft8_khz = st.band_freq_set[idx] ? st.band_freq[idx] : kFt8DefaultKhz[idx];
    const double ft4_khz = st.ft4_freq_set[idx] ? st.ft4_freq[idx] : kFt4DefaultKhz[idx];
    return (int32_t)lround((ft4_khz - ft8_khz) * 1000.0);
}

static int32_t station_ft4_minus_ft8_hz(void) {
    return band_ft4_minus_ft8_hz(s_station, s_station.band_sel);
}

// The radio's window holds FT8 and FT4 on this band: a QMX's I/Q while FT4's channel fits it (qmx_iq_channel.h).
// An HL2 receives one channel today (docs/architecture/hl2.md).
static bool station_both_fit(int radio, int32_t ft4_minus_ft8_hz) {
    return radio == kStationRadioQmx && qmx_iq_channel_refusal(ft4_minus_ft8_hz) == nullptr;
}

// The operating band's plan: the radio's dial and the channels heard there (station_channel.h).
static StationPlan station_current_plan(void) {
    return station_plan((uint32_t)station_ft8_dial_hz(), station_ft4_minus_ft8_hz(),
                        station_listen(s_station, s_station.band_sel),
                        station_both_fit(s_station.radio, station_ft4_minus_ft8_hz()));
}

// Where the station puts the radio's dial: FT8's, or FT4's while the band listens to FT4 alone.
static int station_freq_hz(void) {
    return (int)station_current_plan().radio_dial_hz;
}

static void amp_band_prime_event(const amp_band_prime_event_t *event, void * /*user*/) {
    s_amp_prime_end_ms.store((uint32_t)(esp_timer_get_time() / 1000));
    if (event->kind == AMP_BAND_PRIME_DONE) {
        ESP_LOGI(TAG, "amplifier band prime: %d ms SWR Tune pulse complete",
                 AMP_BAND_PRIME_PULSE_MS);
        return;
    }
    const char *why = "cancelled";
    switch (event->abort_reason) {
        case AMP_BAND_PRIME_ABORT_CANCELLED:
            why = "cancelled";
            break;
        case AMP_BAND_PRIME_ABORT_CAT_LOST:
            why = "CAT failed";
            break;
        case AMP_BAND_PRIME_ABORT_STALLED:
            why = "main loop stalled";
            break;
    }
    ESP_LOGW(TAG, "amplifier band prime aborted: %s", why);
}

// The band last primed, or tried where the radio cannot prime: a transmission due on another band primes
// first (porta_p4_amp_prime_before_tx()). AMP_BAND_PRIME_NONE at boot.
static std::atomic<int> s_primed_band{AMP_BAND_PRIME_NONE};

static void request_amp_band_prime() {
    if (!station_amp_prime_on_band_change(s_station)) {
        return;
    }
    if (!porta_p4_station_call_ok()) {
        ESP_LOGW(TAG, "amplifier band prime skipped: no callsign set");
        return;
    }
    if (s_tune || swr_sweep_busy() || amp_band_prime_busy()) {
        ESP_LOGW(TAG, "amplifier band prime skipped: another radio operation is active");
        return;
    }
    switch (amp_band_prime_request_start(radio_control_swr_io(), amp_band_prime_event, nullptr)) {
        case AMP_BAND_PRIME_START_OK:
            ESP_LOGI(TAG, "amplifier band prime queued");
            s_primed_band.store(s_station.band_sel);
            break;
        case AMP_BAND_PRIME_START_BUSY:
            ESP_LOGW(TAG, "amplifier band prime skipped: already busy");
            break;
        case AMP_BAND_PRIME_START_UNSUPPORTED:
            ESP_LOGW(TAG, "amplifier band prime skipped: radio has no SWR Tune mode");
            s_primed_band.store(s_station.band_sel);  // tried: never asked again for this band
            break;
    }
}

bool porta_p4_amp_prime_before_tx(void) {
    if (!amp_band_prime_needed(station_amp_prime_on_band_change(s_station), s_primed_band.load(), s_station.band_sel)) {
        return false;
    }
    const int idx = s_station.band_sel;
    ESP_LOGI(TAG, "amplifier band prime before the first TX on %s (band set while the radio was away)",
             (idx >= 0 && idx < kStationBandCount) ? kBandNames[idx] : "?");
    request_amp_band_prime();
    return amp_band_prime_busy();
}

int porta_p4_amp_primed_band(void) {
    return s_primed_band.load();
}

// A sync sets the radio in several commands and the radio answers each with its state (AI2;), so
// the pushed states in between are not the radio's own doing. The push tick waits out a sync and
// kRadioPushQuietMs after it, then sends the state the radio settled on.
static std::atomic<int> s_syncing{0};
static std::atomic<uint32_t> s_sync_end_ms{0};
static constexpr uint32_t kRadioPushQuietMs = 150;

static uint32_t uptime_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

struct SyncGuard {
    SyncGuard() { s_syncing.fetch_add(1); }
    ~SyncGuard() {
        s_sync_end_ms.store(uptime_ms());
        s_syncing.fetch_sub(1);
    }
};

// nullptr = VFO updated (or already at target with CAT up). Non-null = why not.
static const char *sync_vfo(const char *reason) {
    SyncGuard guard;
    const int freq_hz = station_freq_hz();
    if (freq_hz <= 0) {
        return "no band frequency";
    }
    if (radio_select_hl2_feeds_decode()) {
        // The HL2 has no CAT: its receiver's NCO follows the dial (hl2_transport.h). Receive only, so no
        // transmission can be under way to wait for.
        hl2_transport_set_dial((uint32_t)freq_hz);
        return nullptr;
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
    if (s_tune) {
        return "Tune active";
    }
    if (swr_sweep_busy()) {
        return "SWR sweep active";
    }
    if (amp_band_prime_busy()) {
        return "amp band prime active";
    }
    radio_control_end_tx();  // RX mode first (idempotent)
    const esp_err_t err = radio_control_sync_frequency_mode(freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "VFO sync failed (%s) freq=%d: %s", reason ? reason : "", freq_hz,
                 esp_err_to_name(err));
        return "CAT sync failed";
    }
    ESP_LOGI(TAG, "VFO sync ok (%s) freq=%d", reason ? reason : "", freq_hz);
    // What the radio says of itself, for the radio bar (docs/architecture/webapp.md, The radio bar); the reply
    // lands in the config snapshot. Not an error if the radio cannot say.
    (void)radio_control_query_state();
    // Once per connection: the version does not change, and the parser keeps it until the radio goes.
    char fw[16];
    if (!radio_control_get_version(fw, sizeof(fw))) {
        (void)radio_control_query_version();
        // The radio then sends its state when it changes; the read above and on panel open stay
        // for firmware that does not answer AI2;. There is no AI0; here: the P4 has no clean stop
        // to send it from, and the QMX forgets AI2; when it is power cycled.
        (void)radio_control_set_push(true);
    }
    return nullptr;
}

static volatile bool s_decode_failed = false;

void porta_p4_set_decode_failed(bool failed) {
    s_decode_failed = failed;
}

void porta_p4_station_boot(void) {
    load_station_from_storage();
}

const char *porta_p4_sync_vfo(const char *reason) {
    ensure_seeded();
    return sync_vfo(reason);
}

static bool log_adif_entry(const std::string &dxcall, const std::string &dxgrid, int rst_sent,
                           int rst_rcvd, RxChannel channel) {
    ensure_seeded();
    AdifStationSnapshot station;
    station.call = s_station.call;
    station.my_gridsquare = grid_ft8_4(s_station.grid);
    station.mode = channel.mode == RxMode::FT4 ? "FT4" : "FT8";
    // The QSO's own dial, its channel's, as WSJT-X logs it: FT4's beside FT8 is FT8's plus the band's FT4 offset.
    station.freq_mhz = adif_freq_mhz(channel.dial_hz);
    {
        const int idx = s_station.band_sel;
        station.band_name = (idx >= 0 && idx < kStationBandCount) ? kBandNames[idx] : "";
    }
    station.raw_comment = s_station.comment1;
    // The radio's own name for itself, so a QMX+ logs as one (adif_radio_name()).
    {
        radio_config_t rc;
        station.radio_name = adif_radio_name(radio_control_get_config(&rc) ? rc.model : "");
    }
    // No GPS on the P4 (docs/architecture/main_board.md) -- nothing to
    // override the configured grid with for "/Grid".
    station.grid_for_macro = s_station.grid;
    station.act_refs = s_station.act_refs;
    return adif_log_entry(dxcall, dxgrid, rst_sent, rst_rcvd, station, &s_adif_logger_dedupe);
}

StationDials porta_p4_station_dials(void) {
    ensure_seeded();
    StationDials d;
    d.plan = station_current_plan();
    d.reported_dial_hz = dial_history_at(rtc_now_ms());
    return d;
}

static bool station_listens_now(RxChannel channel) {
    return station_listens(porta_p4_station_dials(), channel);
}

void porta_p4_adif_register(void) {
    autoseq_set_adif_callback(log_adif_entry);
    decode_pipeline_set_channel_listened(station_listens_now);
    autoseq_set_recent_qso_callback(decode_pipeline_recently_logged);
    autoseq_set_heard_grid_callback(decode_pipeline_heard_grid);
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

int porta_p4_station_radio(void) {
    ensure_seeded();
    return s_station.radio;
}

bool porta_p4_tune_active(void) {
    return s_tune;
}

StationPlan porta_p4_station_plan(void) {
    ensure_seeded();
    return station_current_plan();
}

int32_t porta_p4_station_ft4_dial_offset_hz(void) {
    ensure_seeded();
    return station_current_plan().ft4_offset_hz;
}

bool porta_p4_station_amp_swr_guard(void) {
    ensure_seeded();
    return station_amp_swr_guard(s_station);
}

int porta_p4_station_tx_min_delay_ms(void) {
    ensure_seeded();
    return station_tx_min_delay_ms(s_station);
}

bool porta_p4_station_call_ok(void) {
    ensure_seeded();
    return tx_call_ok(s_station.call.c_str());
}

int porta_p4_station_offset_src(void) {
    ensure_seeded();
    return s_station.offset_src;
}

// Once a second, push SLOT_STATE and
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

    // The armed TX's slot
    // parity and its resolved offset, 0 when nothing is armed. The app shows
    // that offset as "TX: … Hz" and places the passband cursor with it.
    int armed_offset_hz = 0;
    const bool armed = tx_scheduler_hud_snapshot(nullptr, &armed_offset_hz);
    porta_emit_slot_state((uint8_t)(tx_scheduler_target_parity() & 1),
                          (uint16_t)(armed ? armed_offset_hz : 0),
                          tx_scheduler_armed_mode() == RxMode::FT4 ? PORTA_QUEUE_MODE_FT4 : PORTA_QUEUE_MODE_FT8);

    uint16_t current_ids[AUTOSEQ_MAX_QUEUE];
    const int n = autoseq_active_count();
    int filled = 0;
    for (int i = 0; i < n && filled < AUTOSEQ_MAX_QUEUE; ++i) {
        QsoContext ctx;
        if (!autoseq_get_active_context(i, &ctx)) {
            continue;
        }
        current_ids[filled++] = ctx.entry_id;
        const porta_queue_entry_event_t q = queue_entry_event(ctx);
        porta_emit_queue_entry(&q);
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
            const porta_queue_entry_event_t q = queue_entry_gone_event(s_last_active_ids[i]);
            porta_emit_queue_entry(&q);
        }
    }
    memcpy(s_last_active_ids, current_ids, sizeof(uint16_t) * (size_t)filled);
    s_last_active_count = filled;
}

// Where the main board means the radio to be now: during a transmission that retuned it (radio.md, TX timing on a QMX), the
// dial it retuned to; otherwise the station's. The config poll's want_hz and every RADIO_STATE carry it.
static int want_dial_hz(void) {
    const int retuned = radio_control_tx_retuned_hz();
    return retuned ? retuned : station_freq_hz();
}

// The radio pushes its state when it changes (AI2;); send what it settled on as one RADIO_STATE
// event, so the page's chip follows a knob turn at once. Coalesced to at most one per 100 ms, and
// held back while a sync is setting the radio ourselves.
void porta_p4_radio_push_tick(void) {
    static uint32_t s_last_ms = 0;
    static uint32_t s_seen = 0;

    const uint32_t now_ms = uptime_ms();
    if (now_ms - s_last_ms < 100) {
        return;
    }
    s_last_ms = now_ms;
    // A transmission that retuned the radio (radio.md, TX timing on a QMX) is ours too: hold back while it lasts and for the quiet
    // window after, the radio's answer to the retune back included. Seen on this 100 ms tick, so the window
    // runs from within 100 ms of the retune's end.
    static uint32_t s_retuned_seen_ms = 0;
    static bool s_retune_seen = false;
    if (radio_control_tx_retuned_hz() != 0) {
        s_retuned_seen_ms = now_ms;
        s_retune_seen = true;
        return;
    }
    if (s_syncing.load() > 0 || amp_band_prime_busy() ||
        now_ms - s_sync_end_ms.load() < kRadioPushQuietMs ||
        now_ms - s_amp_prime_end_ms.load() < kRadioPushQuietMs ||
        (s_retune_seen && now_ms - s_retuned_seen_ms < kRadioPushQuietMs)) {
        return;
    }
    const uint32_t changes = radio_control_state_changes();
    if (changes == s_seen) {
        return;
    }
    s_seen = changes;
    radio_state_t rs;
    if (!radio_control_get_state(&rs)) {
        return;  // a read of ours cleared it; its reply counts as a change and lands here again
    }
    // With the wanted dial of this same instant, never paired with an older one (webapp.md, The radio bar).
    porta_emit_radio_state((uint32_t)rs.freq_hz, rs.transmitting, (uint8_t)rs.mode, rs.split,
                           (uint32_t)want_dial_hz());
}

// Tune the P4 stopped on high SWR (swr_tune_over()): why, for the TX HUD, and until when it is shown.
static char s_tune_stop_reason[PORTA_TX_HUD_REASON_MAX + 1];
static uint32_t s_tune_stop_until_ms = 0;
static constexpr uint32_t kTuneStopShownMs = 5000;  // Tune has no slot to show it for the rest of, as a trip does

// Phone Operate panel TX HUD.
// B72: poll PC;/SW; so power/SWR are real (this board has no on-device
// banner, so the Port A tick owns the poll).
void porta_p4_tx_hud_tick(void) {
    static uint32_t s_last_ms = 0;
    static bool s_last_active = false;

    // A transmission the radio's SWR protection stopped stays on the banner for the rest of its slot.
    const char *abort_reason = tx_scheduler_hud_abort_reason();
    if (!abort_reason && !s_tune && s_tune_stop_reason[0] &&
        (int32_t)(s_tune_stop_until_ms - (uint32_t)(esp_timer_get_time() / 1000)) > 0) {
        abort_reason = s_tune_stop_reason;
    }
    const char *not_keyed = tx_scheduler_hud_not_keyed_reason();
    const bool active = tx_scheduler_hud_active() || s_tune || abort_reason || not_keyed;
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

    if (not_keyed && !abort_reason && !tx_scheduler_hud_active() && !s_tune) {
        porta_emit_tx_not_keyed(tx_text, offset_hz, not_keyed);
        return;
    }

    if (abort_reason && !tx_scheduler_hud_active() && !s_tune) {
        porta_emit_tx_hud(true, true, false, tx_text, -1.f, -1.f, -1, offset_hz, abort_reason);
        return;
    }

    radio_control_poll_tx_power_swr();
    if (s_tune && tx_scheduler_swr_tripped()) {
        // The lock stays set: the next Tune press clears it (tx_scheduler_swr_operator_start()).
        s_tune = false;
        radio_control_set_tune(false, 0, 0);
        tx_scheduler_swr_abort_note();
        ESP_LOGW(TAG, "CAT tune: stopped by the radio's SWR protection");
        porta_emit_tx_hud(true, true, false, tx_text, -1.f, -1.f, -1, offset_hz,
                          tx_scheduler_hud_abort_reason());
        return;
    }
    float power_w = -1.f, swr = -1.f;
    (void)radio_control_get_tx_power_swr(&power_w, &swr);
    if (s_tune && swr_tune_over(swr)) {
        // The radio's protection did not stop Tune into a bad antenna (radio.md, SWR protection trips on a QMX).
        s_tune = false;
        radio_control_set_tune(false, 0, 0);
        snprintf(s_tune_stop_reason, sizeof(s_tune_stop_reason), "Tune: SWR %.*f over %.1f", swr < 10.f ? 2 : 1,
                 (double)swr, (double)kSwrTuneLimit);
        s_tune_stop_until_ms = now_ms + kTuneStopShownMs;
        ESP_LOGW(TAG, "CAT tune: stopped at SWR %.2f, over %.1f", (double)swr, (double)kSwrTuneLimit);
        debug_log_line(std::string("Tune stopped: ") + s_tune_stop_reason);
        porta_emit_tx_hud(true, true, false, tx_text, power_w, swr, -1, offset_hz, s_tune_stop_reason);
        return;
    }

    porta_emit_tx_hud(true, false, false, tx_text, power_w, swr, -1, offset_hz, "");
}

// A backward step waiting for the next browser set to confirm it (clock_set_decide()); 0 for none.
static int64_t s_clock_held_step_ms = 0;

// A browser set is decided by clock_set_decide() (decode_sync.h; docs/architecture/time.md): it may have waited
// in a stalled sidekick and carry an old time. Every set it does not apply says so in the log.
const char *porta_host_set_clock(uint32_t epoch_secs, uint16_t millis) {
    if (epoch_secs < PORTA_CLOCK_FLOOR_SECS) {
        return "implausible time";
    }
    const int64_t now_ms = rtc_now_ms();
    const int64_t step_ms = (int64_t)epoch_secs * 1000 + millis - now_ms;
    int64_t apply_ms = 0;
    const bool phase_good = decode_pipeline_phase_good();
    char line[64];
    switch (clock_set_decide(rtc_is_set() ? 1 : 0, step_ms, phase_good ? 1 : 0, &s_clock_held_step_ms, &apply_ms)) {
        case CLOCK_SET_APPLY: {
            const int64_t new_ms = now_ms + apply_ms;
            rtc_seed_epoch((time_t)(new_ms / 1000), esp_timer_get_time() / 1000 - new_ms % 1000);
            rtc_sync_to_esp_rtc();
            ESP_LOGI(TAG, "clock set epoch=%lld (%+lld ms)", (long long)(new_ms / 1000), (long long)apply_ms);
            return nullptr;
        }
        case CLOCK_SET_IGNORE:
            return nullptr;
        case CLOCK_SET_DISREGARD:
            snprintf(line, sizeof(line), "CLOCK set %+lld ms disregarded: off the slots", (long long)step_ms);
            break;
        case CLOCK_SET_HOLD:
            snprintf(line, sizeof(line), "CLOCK set %+lld ms held for the next to confirm", (long long)step_ms);
            break;
        case CLOCK_SET_DROP:
            snprintf(line, sizeof(line), "CLOCK held step dropped: this set agrees with the clock");
            break;
    }
    ESP_LOGW(TAG, "%s", line);
    debug_log_line(line);
    return nullptr;
}

void porta_host_tx_cancel(void) {
    tx_scheduler_cancel();
    if (s_tune) {
        s_tune = false;
        (void)radio_control_set_tune(false, 0, 0);
    }
    amp_band_prime_request_cancel();
    // A sweep is a transmission too: the one stop control ends it (docs/architecture/radio.md, The SWR sweep). The ticking
    // task does the de-key on its next tick, at most 10 ms away.
    swr_sweep_request_cancel();
}

// An update holds transmit off from the operator's tap to its end (docs/architecture/main_board.md, Firmware
// updates): the transmission on the air stops, the CQ and the queued QSOs end, as on a band change, and
// radio_control refuses to key until the hold is released. The hold first: a cancel alone holds transmission off
// only to the end of its slot. Released, the queue is ended again, so nothing autoseq took up meanwhile (a station
// calling us) keys by itself: the operator starts again.
void porta_host_update_hold(bool on) {
    if (on) {
        radio_control_hold_tx(true);
    }
    porta_host_tx_cancel();
    autoseq_clear();
    if (!on) {
        radio_control_hold_tx(false);
    }
}

// Before a restart the board starts itself (components/porta: into a new image, or back from one that never heard
// the sidekick), whatever was or was not held before: autoseq re-arms at once after a cancel, so a slot of ours
// starting before the restart keyed the radio and the board reset keyed (console log, 2026-11-11: "Fetch TX" and
// "TX ready" right after the cancel).
void porta_host_stop_for_restart(void) {
    radio_control_hold_tx(true);
    porta_host_tx_cancel();
}

// Sweep events go to the phone as they happen. Runs on the main loop's task (swr_sweep_tick).
static void sweep_event_to_porta(const swr_sweep_event_t *event, void * /*user*/) {
    uint8_t kind = PORTA_SWEEP_DONE;
    uint8_t reason = PORTA_SWEEP_REASON_CANCELLED;
    switch (event->kind) {
        case SWR_SWEEP_EVENT_POINT:
            kind = PORTA_SWEEP_POINT;
            break;
        case SWR_SWEEP_EVENT_BAND_SKIPPED:
            kind = PORTA_SWEEP_BAND_SKIPPED;
            break;
        case SWR_SWEEP_EVENT_DONE:
            kind = PORTA_SWEEP_DONE;
            break;
        case SWR_SWEEP_EVENT_ABORTED:
            kind = PORTA_SWEEP_ABORTED;
            break;
    }
    switch (event->abort_reason) {
        case SWR_SWEEP_ABORT_CANCELLED:
            reason = PORTA_SWEEP_REASON_CANCELLED;
            break;
        case SWR_SWEEP_ABORT_CAT_LOST:
            reason = PORTA_SWEEP_REASON_CAT_LOST;
            break;
        case SWR_SWEEP_ABORT_STALLED:
            reason = PORTA_SWEEP_REASON_STALLED;
            break;
    }
    porta_emit_sweep(kind, event->range_index, event->freq_hz, event->swr_hundredths, reason);
}

const char *porta_host_sweep_start(const porta_sweep_range_t *ranges, size_t count) {
    ensure_seeded();
    const radio_swr_io_t *io = radio_control_swr_io();
    if (!io) {
        return "this radio cannot sweep";
    }
    if (!porta_p4_station_call_ok()) {
        return "No callsign set";
    }
    if (!radio_control_ready()) {
        return "CAT not ready";
    }
    if (decode_tx_state_get_tx_active()) {
        return "transmitting";
    }
    if (autoseq_cq_entry_id() != 0) {
        return "a CQ is queued";
    }
    if (autoseq_queue_size() > 0 || autoseq_has_active_qso()) {
        return "a QSO is queued";
    }
    if (s_tune) {
        return "tune is on";
    }
    if (amp_band_prime_busy()) {
        return "amp band prime is running";
    }
    const int restore_hz = station_freq_hz();
    if (restore_hz <= 0) {
        return "no band frequency";
    }
    swr_sweep_range_t plan[SWR_SWEEP_MAX_RANGES];
    if (count > SWR_SWEEP_MAX_RANGES) {
        return "too many ranges";
    }
    for (size_t i = 0; i < count; ++i) {
        plan[i].lo_khz = ranges[i].lo_khz;
        plan[i].hi_khz = ranges[i].hi_khz;
        plan[i].step_khz = ranges[i].step_khz;
    }
    switch (swr_sweep_request_start(plan, count, (uint32_t)restore_hz, io, sweep_event_to_porta, nullptr)) {
        case SWR_SWEEP_START_OK:
            return nullptr;
        case SWR_SWEEP_START_BAD_RANGE:
            return "bad range";
        case SWR_SWEEP_START_TOO_MANY:
            return "too many points";
        case SWR_SWEEP_START_BUSY:
            return "a sweep is already running";
        case SWR_SWEEP_START_UNSUPPORTED:
            return "this radio cannot sweep";
    }
    return "sweep refused";
}

size_t porta_host_config_snapshot(char *out, size_t out_cap, bool mask_secrets) {
    ensure_seeded();
    if (!out || out_cap < 2) {
        return 0;
    }

    std::string text = station_serialize(mask_secrets ? station_masked(s_station) : s_station);

    const int idx = s_station.band_sel;
    const char *band_name =
        (idx >= 0 && idx < kStationBandCount) ? kBandNames[idx] : "?";
    // Where the radio's dial is: FT8's, or FT4's while the band listens to FT4 alone.
    const int freq_khz = (station_freq_hz() + 500) / 1000;

    char live[320];
    snprintf(live, sizeof(live),
             "streaming=%d\ncat_ready=%d\ntune=%d\nswr_sweep=%d\nband_name=%s\nfreq_khz=%d\n"
             "audio_rx=%d\nclock_set=%d\ndecode_failed=%d\nhl2=%s\nhl2_overload=%d\n",
             radio_select_audio_ready() ? 1 : 0, radio_control_ready() ? 1 : 0, s_tune ? 1 : 0,
             swr_sweep_busy() ? 1 : 0,  // a sweep keys the radio: the radio bar shows it on every tab
             band_name,
             freq_khz, radio_select_audio_ready() ? 1 : 0, rtc_is_set() ? 1 : 0, s_decode_failed ? 1 : 0,
             hl2_transport_state_name(), hl2_transport_overloaded() ? 1 : 0);
    text += live;

    // The HL2's gateware, as its discovery reply gave it, while it is the radio (docs/architecture/hl2.md, Gateware).
    unsigned gw_major = 0, gw_minor = 0;
    if (strcmp(hl2_transport_state_name(), "off") != 0 && hl2_transport_gateware(&gw_major, &gw_minor)) {
        char gw[24];
        snprintf(gw, sizeof(gw), "hl2_gw=%u.%u\n", gw_major, gw_minor);
        text += gw;
    }

    // What the radio last said of itself (IF;), against what micro-FT8 means it to be: both in
    // Hz, so the page can tell a knob turned from a band changed. Absent until a reply arrives.
    char fw[16];
    if (radio_control_get_version(fw, sizeof(fw))) {
        text += std::string("radio_fw=") + fw + "\n";
        static char s_logged_fw[16] = "";
        if (strcmp(s_logged_fw, fw) != 0) {
            snprintf(s_logged_fw, sizeof(s_logged_fw), "%s", fw);
            ESP_LOGI(TAG, "radio firmware %s", fw);
        }
    }
    // The radio's own band and PTT settings, read at link-up, for Settings > Amplifier's check, and
    // the settings Radio & bands shows. Absent until the read has finished on this radio.
    radio_config_t radio_cfg;
    if (radio_control_get_config(&radio_cfg)) {
        char cbuf[320];
        radio_config_format(&radio_cfg, cbuf, sizeof(cbuf));
        text += cbuf;
    }
    char write_status[96];
    if (radio_control_config_write_status(RADIO_WRITE_PTT, write_status, sizeof(write_status))) {
        text += std::string("radio_ptt_write=") + write_status + "\n";
    }
    if (radio_control_config_write_status(RADIO_WRITE_SETTING, write_status, sizeof(write_status))) {
        text += std::string("radio_setting_write=") + write_status + "\n";
    }
    radio_state_t rs;
    if (radio_control_get_state(&rs)) {
        char rbuf[96];
        snprintf(rbuf, sizeof(rbuf), "radio_hz=%d\nradio_tx=%d\nradio_mode=%d\nradio_split=%d\nwant_hz=%d\n",
                 rs.freq_hz, rs.transmitting ? 1 : 0, rs.mode, rs.split ? 1 : 0,
                 want_dial_hz());
        text += rbuf;
    }

    // GET-only like the other live lines. Absent while firmware does not own storage, which the
    // app shows as unknown rather than as zero.
    size_t st_total = 0, st_used = 0;
    if (storage_service_info(&st_total, &st_used)) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "storage_total_kb=%u\nstorage_used_kb=%u\n",
                 (unsigned)(st_total / 1024), (unsigned)((st_used + 1023) / 1024));
        text += sbuf;
    }

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
    if (strcmp(key, "tries") == 0) {
        // Refused here rather than clamped by autoseq, so Settings > Station never
        // shows a count the engine is not using (docs/architecture/autoseq.md, Tries).
        const int tries = atoi(value);
        if (tries < 1 || tries > AUTOSEQ_TRIES_MAX_QSO) {
            return "tries is 1 to 20";
        }
    }

    if (strcmp(key, "cq_mod") == 0) {
        // Refused, not dropped as station_parse() would: anything else is not a
        // standard message, and Settings > Station must not show a modifier the
        // CQ is not sending (docs/architecture/autoseq.md, The CQ's modifier).
        std::string mod = value;
        const size_t a = mod.find_first_not_of(" \t");
        const size_t b = mod.find_last_not_of(" \t");
        mod = a == std::string::npos ? "" : mod.substr(a, b - a + 1);
        for (auto &c : mod) c = (char)toupper((unsigned char)c);
        if (!station_cq_mod_ok(mod)) {
            return "CQ modifier is 1 to 4 letters, or 3 digits";
        }
    }

    // The web app's own settings: refused, not dropped as station_parse() would, so Settings never
    // shows a value the station did not keep.
    if (const char *why = station_pref_refusal(key, value)) {
        return why;
    }

    if (strcmp(key, "tx_min_delay_ms") == 0) {
        // Refused, not clamped, like tries: the field must not show a delay the
        // scheduler is not using.
        char *end = nullptr;
        const long ms = strtol(value, &end, 10);
        if (end == value || *end != '\0' || ms < 0 || ms > kStationTxMinDelayMaxMs) {
            return "min TX delay is 0 to 500 ms";
        }
    }

    int listen_band = -1;
    if (sscanf(key, "listen%d", &listen_band) == 1) {
        char *end = nullptr;
        const long v = strtol(value, &end, 10);
        if (end == value || *end != '\0' || v < kStationListenFt8 || v > kStationListenBoth) {
            return "listen is 1 (FT8), 2 (FT4) or 3 (both)";
        }
    }
    // Refused, not dropped: the drawer must not show channels the station is not hearing
    // (docs/architecture/decode.md, Channels). Both cannot be turned on where they do not fit, and a frequency
    // write cannot move a band's two channels apart. A write to a pair already apart is let through, so a band tap's
    // two standard dials, written one line at a time, land whatever order the lines come in.
    {
        int band = -1;
        const bool listen_key = sscanf(key, "listen%d", &band) == 1;
        if (listen_key || sscanf(key, "band%d", &band) == 1 || sscanf(key, "ft4band%d", &band) == 1) {
            StationSettings after = s_station;
            station_parse(std::string(key) + "=" + value + "\n", &after);
            const int32_t apart_hz = band_ft4_minus_ft8_hz(after, band);
            const bool fit_before = station_both_fit(s_station.radio, band_ft4_minus_ft8_hz(s_station, band));
            if (band >= 0 && band < kStationBandCount && station_listen(after, band) == kStationListenBoth &&
                !station_both_fit(after.radio, apart_hz) && (listen_key || fit_before)) {
                if (after.radio != kStationRadioQmx) {
                    return "the HL2 hears one channel at a time";
                }
                static char s_why[112];
                snprintf(s_why, sizeof s_why,
                         "FT4's dial would be %+.1f kHz from FT8's on %s; a QMX hears both from -33 to +6 kHz",
                         apart_hz / 1000.0, kBandNames[band]);
                return s_why;
            }
        }
    }

    if (station_key_names_operating_band(key, s_station.band_sel)) {
        if (s_tune) {
            return "Tune is on";
        }
        if (swr_sweep_busy()) {
            return "SWR sweep is running";
        }
        if (amp_band_prime_busy()) {
            return "amp band prime is running";
        }
    }

    if (strcmp(key, "radio") == 0) {
        // The radio that is keyed must be the one that un-keys (radio_select.h).
        if (s_tune) {
            return "Tune is on";
        }
        if (swr_sweep_busy()) {
            return "SWR sweep is running";
        }
        if (amp_band_prime_busy()) {
            return "amp band prime is running";
        }
        if (decode_tx_state_get_tx_active()) {
            return "a transmission is on";
        }
    }

    std::string line = std::string(key) + "=" + value + "\n";
    const int band_was = s_station.band_sel;
    const int radio_was = s_station.radio;
    const bool act_refs_was_empty = s_station.act_refs.empty();
    station_parse(line, &s_station);
    set_station_call();

    // Activation start (act_refs empty -> non-empty) picks the CQ's modifier
    // for the operator -- SOTA if any SOTA reference is present (a mixed
    // activation defaults to SOTA), else POTA (architecture/logging.md, "The
    // CQ's modifier follows the activation"). Only this one transition derives it: a
    // later add/remove must never clobber a manual switch made through the
    // Settings > Station's own Type control.
    if (strcmp(key, "act_refs") == 0 && act_refs_was_empty && !s_station.act_refs.empty()) {
        s_station.cq_mod = act_refs_prefer_sota(s_station.act_refs) ? "SOTA" : "POTA";
    }

    if (strcmp(key, "call") == 0 || strcmp(key, "grid") == 0 || strcmp(key, "skiptx1") == 0 ||
        strcmp(key, "tries") == 0 || strcmp(key, "cq_mod") == 0 ||
        strcmp(key, "act_refs") == 0) {
        sync_autoseq_station();
    }

    persist_station();

    if (s_station.radio != radio_was) {
        radio_select_apply(s_station.radio);
    }

    if (strcmp(key, "band_sel") == 0 || strcmp(key, "active_bands") == 0 ||
        strncmp(key, "band", 4) == 0 || strncmp(key, "ft4band", 7) == 0 || strncmp(key, "listen", 6) == 0) {
        const char *why = sync_vfo("porta config");
        // A write that names the band actually being operated
        // needs the VFO to have moved to mean what it says. Other band-ish
        // keys stay silent on a sync miss.
        if (why && station_key_names_operating_band(key, s_station.band_sel)) {
            ESP_LOGI(TAG, "config %s=%s (saved, VFO not moved: %s)", key, value, why);
            if (strcmp(why, "TX active") == 0 || strcmp(why, "CAT not ready") == 0) {
                return station_vfo_refusal(key, strcmp(why, "TX active") == 0);
            }
            return why;
        }
        if (!why && strcmp(key, "band_sel") == 0 && s_station.band_sel != band_was) {
            request_amp_band_prime();
        }
    }

    // The console is not the place for the QRZ key either (station_key_secret()).
    ESP_LOGI(TAG, "config %s=%s", key,
             station_key_secret(key) ? station_mask_secret(value).c_str() : value);
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
    if (porta_link_test_active()) {
        return "link test";
    }
    ensure_seeded();
    if (on && swr_sweep_busy()) {
        return "a sweep is running";
    }
    if (on && amp_band_prime_busy()) {
        return "amp band prime is running";
    }
    if (on && !porta_p4_station_call_ok()) {
        return "No callsign set";
    }
    if (on && !radio_control_ready()) {
        return "CAT not ready";
    }
    if (on) {
        tx_scheduler_swr_operator_start(true);
        s_tune_stop_reason[0] = '\0';  // a new Tune is not the one stopped
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

// Settings > Amplifier's Set it (architecture/radio.md, Setting the PTT outputs). The write lands in
// the radio's EEPROM, so only when nothing keys the radio; its progress is radio_ptt_write.
const char *porta_host_radio_set_ptt(uint8_t keying) {
    if (porta_link_test_active()) {
        return "link test";
    }
    if (!radio_control_ready()) {
        return "CAT not ready";
    }
    if (decode_tx_state_get_tx_active() || s_tune || swr_sweep_busy() || amp_band_prime_busy()) {
        return "the radio is busy: transmitting, tuning or sweeping";
    }
    return radio_control_set_ptt_outputs(static_cast<radio_ptt_keying_t>(keying));
}

// Settings > Radio & bands' radio settings (architecture/radio.md, The radio's settings in Radio &
// bands): written to the radio, never stored here, so only when nothing keys the radio; its progress
// is radio_setting_write.
const char *porta_host_radio_set_setting(const char *key, const char *value) {
    const radio_setting_t setting = radio_setting_from_key(key);
    if (setting == RADIO_SETTING_COUNT) {
        return "unknown setting";
    }
    if (porta_link_test_active()) {
        return "link test";
    }
    if (!radio_control_ready()) {
        return "CAT not ready";
    }
    if (decode_tx_state_get_tx_active() || s_tune || swr_sweep_busy() || amp_band_prime_busy()) {
        return "the radio is busy: transmitting, tuning or sweeping";
    }
    return radio_control_set_setting(setting, value);
}

const char *porta_host_radio_read(void) {
    if (porta_link_test_active()) {
        return "link test";
    }
    if (!radio_control_ready()) {
        return "CAT not ready";
    }
    // The query is a CAT command between tones' neighbours; never during a transmission.
    if (decode_tx_state_get_tx_active() || s_tune) {
        return "TX active";
    }
    if (radio_control_query_state() != ESP_OK) {
        return "the radio cannot report its state";
    }
    return nullptr;
}

// Call CQ and a reply are refused unless the radio is ready (tx_ready.h): a
// queue entry with no radio is skipped slot after slot without using a try, so
// it would key by itself when a radio was plugged in, however much later.
// Then the radio must be on the station's dial: one moved by hand belongs to no channel (station_channel.h).
static const char *tx_refusal(void) {
    if (const char *why = tx_ready_refusal(porta_p4_station_call_ok(), radio_control_ready(),
                                           radio_select_audio_ready(), s_decode_failed)) {
        return why;
    }
    static char s_off_dial[40];
    return station_off_dial_refusal(porta_p4_station_dials(), s_off_dial, sizeof s_off_dial);
}

const char *porta_host_cq_call(uint32_t dial_hz, uint8_t mode) {
    if (porta_link_test_active()) {
        return "link test";
    }
    ensure_seeded();
    if (const char *why = tx_refusal()) {
        return why;
    }
    const RxMode rx_mode = mode == PORTA_QUEUE_MODE_FT4 ? RxMode::FT4 : RxMode::FT8;
    RxChannel channel;
    if (!station_channel(porta_p4_station_dials(), rx_mode, &channel)) {
        return "FT4 is not decoded";  // FT8's is listened to whenever the radio is on the station's dial
    }
    if (channel.dial_hz != dial_hz) {
        return "not a channel the station listens to";  // a page behind a band change
    }
    if (autoseq_cq_entry_id() != 0) {
        return "a CQ is already queued";
    }
    if (!tx_scheduler_call_cq(channel)) {
        return "TX queue full";
    }
    return nullptr;
}

const char *porta_host_queue_tries(uint16_t entry_id, uint8_t tries) {
    switch (autoseq_set_entry_tries(entry_id, tries)) {
        case AutoseqTriesResult::Ok:
            tx_scheduler_rearm();
            return nullptr;
        case AutoseqTriesResult::UnknownEntry:
            return "unknown entry";
        case AutoseqTriesResult::OutOfRange:
            return "tries out of range";
    }
    return "tries refused";
}

const char *porta_host_queue_parity(uint16_t entry_id, uint8_t parity) {
    if (parity > 1) {
        return "parity is 0 or 1";
    }
    switch (autoseq_set_cq_parity(entry_id, parity)) {
        case AutoseqParityResult::Ok:
            tx_scheduler_rearm();
            return nullptr;
        case AutoseqParityResult::UnknownEntry:
            return "unknown entry";
        case AutoseqParityResult::NotCq:
            return "only a CQ's parity can change";
    }
    return "parity refused";
}

void porta_host_link_test(bool active, bool tune) {
    if (!active) {
        if (s_tune) {
            porta_host_tune(false);
        }
        return;
    }
    porta_host_tx_cancel();
    autoseq_clear();
    if (tune) {
        porta_host_tune(true);
    }
}

static const char *apply_reply_touch(const UiRxLine &msg) {
    const AutoseqTouchResult touch =
        autoseq_on_touch(msg, decode_tx_state_get_tx_active());
    if (touch == AutoseqTouchResult::IgnoredInProgress) {
        return "QSO in progress";
    }
    if (touch == AutoseqTouchResult::NoRoom) {
        return "TX queue full";
    }
    AutoseqTxEntry pending{};
    if (!decode_tx_state_get_tx_active() && autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
    }
    return nullptr;
}

const char *porta_host_queue_cancel(uint16_t entry_id) {
    // Cancelling the QSO on the air stops the transmission too, not only its
    // later ones (operator decision, 2026-09-26).
    const bool on_air = decode_tx_state_get_tx_active() &&
                        autoseq_transmitting_entry_id() == entry_id;
    if (!autoseq_drop_by_entry_id(entry_id)) {
        return "unknown entry";
    }
    if (on_air) {
        tx_scheduler_cancel();
    }
    ESP_LOGI(TAG, "queue cancel entry_id=%u", (unsigned)entry_id);
    return nullptr;
}

const char *porta_host_queue_reply(uint32_t decode_id) {
    if (porta_link_test_active()) {
        return "link test";
    }
    if (const char *why = tx_refusal()) {
        return why;
    }
    UiRxLine msg;
    if (!find_decode_for_reply(decode_id, &msg)) {
        return "unknown decode";
    }
    tx_scheduler_swr_operator_start(false);
    const char *why = apply_reply_touch(msg);
    if (why) {
        ESP_LOGI(TAG, "queue reply decode_id=%lu: %s", (unsigned long)decode_id, why);
        return why;
    }
    ESP_LOGI(TAG, "queue reply decode_id=%lu ok", (unsigned long)decode_id);
    return nullptr;
}

// The day list is the store's days, named as the daily files were, so the wire and the QSOs tab are unchanged
// (docs/architecture/logging.md, The store on the main board). Sent whole, then ACKed.
const char *porta_host_file_list_begin(uint8_t kind, uint16_t skip, uint8_t take) {
    if (kind != PORTA_FILE_LIST_QSO_DAILY) {
        return "unknown kind";
    }
    if (take == 0) {
        return "take must be > 0";
    }
    if (!qso_log_store_ready()) {
        return "log unavailable";
    }
    const int64_t start_us = esp_timer_get_time();  // timed for the QSO store's before-and-after
    std::vector<std::string> days;
    qso_log_store_days(&days);
    for (size_t i = skip; i < days.size() && (i - skip) < take; ++i) {
        porta_emit_file_name(days[i].c_str());
    }
    porta_emit_ack(PORTA_MSG_FILE_LIST);
    ESP_LOGI(TAG, "FILE_LIST qso daily names=%u skip=%u take=%u in %lld us", (unsigned)days.size(),
             (unsigned)skip, (unsigned)take, (long long)(esp_timer_get_time() - start_us));
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
    // Refuse while transmitting, decoding or streaming audio: qmx_audio_ready()
    // feeds the same "streaming"/"audio_rx" config fields as
    // porta_host_config_snapshot() above.
    if (!decode_tx_state_get_tx_active() && !decode_tx_state_get_decode_in_progress() &&
        !qmx_audio_ready()) {
        return false;
    }
    return today_qso_file_name() == basename;
}

}  // namespace

// An empty name is every QSO; otherwise one day's, refused for today's while transmitting, decoding or streaming.
const char *porta_host_file_delete(const char *filename) {
    if (!filename || filename[0] == '\0') {
        return qso_log_store_clear();
    }
    if (p4_reject_active_log_mutation(filename)) {
        return "active log locked";
    }
    return qso_log_store_delete_day(qso_log_day_of(filename));
}

// The host half of the sidekick's factory reset. No restart: a P4 reset can leave the QMX
// unenumerated until it is replugged (row qdcm), and everything a reset has to change is the
// Station settings, which are reseeded in place. The radio is left where it is; the next band or
// frequency write moves it. Refuses before changing anything it can: transmitting, or no storage.
const char *porta_host_factory_reset(uint8_t flags) {
    if (decode_tx_state_get_tx_active()) {
        return "transmitting";
    }
    if (!storage_service_firmware_available()) {
        return "storage unavailable";
    }
    if (flags & PORTA_FACTORY_RESET_LOGS) {
        if (const char *why = qso_log_store_clear()) {
            return why;
        }
    }
    if (storage_file_exists("Station.txt") && !storage_file_remove("Station.txt")) {
        return "settings delete failed";
    }
    // Nothing queued under the old callsign or frequency should go out.
    porta_host_tx_cancel();
    autoseq_clear();
    station_seed_adv_defaults();
    ESP_LOGW(TAG, "factory reset: Station settings back to defaults%s",
             (flags & PORTA_FACTORY_RESET_LOGS) ? ", QSO logs deleted" : "");
    return nullptr;
}

namespace {

std::vector<QsoBrowseBand> s_porta_read_bands;

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

// One page of a day's QSOs from the store, sent at once, then ACKed. Each record goes through the same pager
// the daily files did, so its row is what the QSOs tab has always read (docs/architecture/logging.md, The
// four-place field chain).
const char *porta_host_file_read_begin(const char *filename, uint16_t skip, uint8_t take) {
    if (!filename || filename[0] == '\0' || take == 0) {
        return "malformed";
    }
    const uint32_t date = qso_log_day_of(filename);
    if (date == 0) {
        return "open failed";
    }
    if (!qso_log_store_ready()) {
        return "log unavailable";
    }
    const int64_t start_us = esp_timer_get_time();  // timed for the QSO store's before-and-after
    std::vector<std::string> records;
    qso_log_store_day_records(date, skip, take, &records);
    QsoBrowsePager pager;
    qso_browse_pager_reset(&pager, 0, take);
    fill_browse_bands();
    for (const std::string &fields : records) {
        qso_browse_pager_feed(&pager, fields + "<eor>", s_porta_read_bands.data(),
                              static_cast<int>(s_porta_read_bands.size()));
    }
    for (const QsoLogEntry &e : pager.entries) {
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
        std::snprintf(row.my_pota_ref, sizeof(row.my_pota_ref), "%s", e.my_pota_ref.c_str());
        std::snprintf(row.my_sota_ref, sizeof(row.my_sota_ref), "%s", e.my_sota_ref.c_str());
        porta_emit_file_entry(row);
    }
    porta_emit_ack(PORTA_MSG_FILE_READ);
    ESP_LOGI(TAG, "FILE_READ %s entries=%u skip=%u take=%u in %lld us", filename, (unsigned)pager.entries.size(),
             (unsigned)skip, (unsigned)take, (long long)(esp_timer_get_time() - start_us));
    return nullptr;
}
