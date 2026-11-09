#include "radio_control_backend.h"
#include "cat_reply_parser.h"
#include "qmx_iq_link.h"
#include "qmx_menu_read.h"
#include "qmx_menu_write.h"
#include "radio_ta_format.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#include "radio_transport.h"

static const char* TAG = "RADIO_QMX";

static bool s_power_swr_poll_disabled;
static int s_power_swr_poll_misses;
static bool s_poll_sr;  // add SR; to the TX poll (radio_swr_lock_t::poll_with_meters)
static constexpr int kPowerSwrPollGiveUp = 4;

static bool qmx_ready(void) {
    return cat_cdc_ready();
}

static esp_err_t qmx_send_cmd(const char* cmd, uint32_t timeout_ms) {
    if (!cat_cdc_ready()) return ESP_ERR_INVALID_STATE;
    return cat_cdc_send(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), timeout_ms);
}

static esp_err_t qmx_sync_frequency_mode(int freq_hz) {
    const char* md = "MD6;";
    esp_err_t err = qmx_send_cmd(md, 200);
    if (err != ESP_OK) return err;

    err = qmx_send_cmd("FR0;", 200);
    if (err != ESP_OK) return err;

    err = qmx_send_cmd("FT0;", 200);
    if (err != ESP_OK) return err;

    char fa[32];
    snprintf(fa, sizeof(fa), "FA%011d;", freq_hz);
    err = qmx_send_cmd(fa, 200);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QMX sync ok freq=%d", freq_hz);
    }
    return err;
}

static esp_err_t qmx_begin_tx(int freq_hz, int tx_base_hz) {
    (void)freq_hz;
    (void)tx_base_hz;

    const char* md = "MD6;";
    esp_err_t err = qmx_send_cmd(md, 200);
    if (err != ESP_OK) return err;

    const char* tx = "TX;";
    err = qmx_send_cmd(tx, 200);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QMX TX start");
    }
    return err;
}

static esp_err_t qmx_set_tone_hz(float tone_hz) {
    char ta[32];
    radio_ta_format(tone_hz, ta, sizeof(ta));
    return qmx_send_cmd(ta, 10);
}

static esp_err_t qmx_end_tx(void) {
    cat_cdc_reset_parsed_meters();
    const char* rx = "RX;";
    esp_err_t err = qmx_send_cmd(rx, 200);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QMX TX stop");
    }
    return err;
}

// TA0; (a tone under 10 Hz) keys down; PTT stays asserted until RX;.
static esp_err_t qmx_key_up(void) {
    return qmx_send_cmd("TA0;", 10);
}

static esp_err_t qmx_set_tune(bool enable, int freq_hz, int /*tone_hz*/) {
    if (!enable) {
        esp_err_t err = qmx_end_tx();
        if (err != ESP_OK) return err;
        return qmx_send_cmd("MD6;", 200);
    }

    esp_err_t err = qmx_sync_frequency_mode(freq_hz);
    if (err != ESP_OK) return err;

    return qmx_send_cmd("MD8;", 200);
}

static esp_err_t qmx_on_audio_start(void) {
    ESP_LOGI(TAG, "QMX CAT backend initialized");
    return ESP_OK;
}

static esp_err_t qmx_set_time(int hour, int minute, int second) {
    char tm[32];
    snprintf(tm, sizeof(tm), "TM%02d%02d%02d;", hour, minute, second);
    esp_err_t err = qmx_send_cmd(tm, 200);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QMX CAT time set: %02d:%02d:%02d", hour, minute, second);
    }
    return err;
}

static void qmx_reset_tx_power_swr(void) {
    cat_cdc_reset_parsed_meters();
    s_power_swr_poll_disabled = false;
    s_power_swr_poll_misses = 0;
}

static void qmx_poll_tx_power_swr(void) {
    if (s_power_swr_poll_disabled) {
        return;
    }
    if (!cat_cdc_ready()) {
        s_power_swr_poll_misses++;
        if (s_power_swr_poll_misses >= kPowerSwrPollGiveUp) {
            s_power_swr_poll_disabled = true;
            ESP_LOGW(TAG, "TX power/SWR: CAT not ready, giving up this TX");
        }
        return;
    }

    const char* poll = s_poll_sr ? "PC;SW;SR;" : "PC;SW;";
    const esp_err_t err =
        cat_cdc_send(reinterpret_cast<const uint8_t*>(poll), strlen(poll), 10);
    if (err != ESP_OK) {
        s_power_swr_poll_disabled = true;
        ESP_LOGW(TAG, "TX power/SWR: CAT send failed (%s), giving up this TX",
                 esp_err_to_name(err));
        return;
    }

    int pc = -1;
    int sw = -1;
    if (cat_cdc_get_parsed_meters(&pc, &sw) && (pc >= 0 || sw >= 0)) {
        s_power_swr_poll_misses = 0;
        return;
    }
    s_power_swr_poll_misses++;
    if (s_power_swr_poll_misses >= kPowerSwrPollGiveUp) {
        s_power_swr_poll_disabled = true;
        ESP_LOGW(TAG, "TX power/SWR: no PC/SW replies (old firmware?), hiding RF line");
    }
}

static bool qmx_get_tx_power_swr(float* power_w, float* swr) {
    int pc = -1;
    int sw = -1;
    if (!cat_cdc_get_parsed_meters(&pc, &sw)) {
        return false;
    }
    if (power_w) {
        *power_w = pc >= 0 ? pc / 10.0f : -1.f;
    }
    if (swr) {
        *swr = sw >= 0 ? sw / 100.0f : -1.f;
    }
    return pc >= 0 || sw >= 0;
}

static esp_err_t qmx_swr_unkey(void) {
    return qmx_send_cmd("RX;", 50);
}

static esp_err_t qmx_swr_mode_digital(void) {
    return qmx_send_cmd("MD6;", 50);
}

static esp_err_t qmx_swr_set_frequency(int hz) {
    char fa[32];
    snprintf(fa, sizeof(fa), "FA%011d;", hz);
    return qmx_send_cmd(fa, 50);
}

static esp_err_t qmx_swr_query_frequency(void) {
    return qmx_send_cmd("FA;", 50);
}

static esp_err_t qmx_swr_key(void) {
    return qmx_send_cmd("MD8;", 50);
}

static esp_err_t qmx_swr_query_swr(void) {
    return qmx_send_cmd("SW;", 50);
}

static bool qmx_swr_frequency_reply(int* hz) {
    return cat_cdc_get_parsed_freq(hz);
}

static bool qmx_swr_swr_reply(int* hundredths) {
    int sw = -1;
    if (!cat_cdc_get_parsed_meters(nullptr, &sw) || sw < 0) {
        return false;
    }
    if (hundredths) {
        *hundredths = sw;
    }
    return true;
}

static esp_err_t qmx_swr_lock_query(void) {
    cat_cdc_reset_parsed_meters();
    return qmx_send_cmd("SR;", 50);
}

static bool qmx_swr_lock_reply(bool* locked) {
    int sr = -1;
    if (!cat_cdc_get_parsed_sr(&sr)) {
        return false;
    }
    if (locked) {
        *locked = sr != 0;
    }
    return true;
}

static esp_err_t qmx_swr_lock_clear(void) {
    cat_cdc_reset_parsed_meters();
    return qmx_send_cmd("SR0;SR;", 50);
}

static void qmx_swr_lock_poll_with_meters(bool on) {
    s_poll_sr = on;
}

static const radio_swr_lock_t k_swr_lock = {
    .query = qmx_swr_lock_query,
    .reply = qmx_swr_lock_reply,
    .clear = qmx_swr_lock_clear,
    .poll_with_meters = qmx_swr_lock_poll_with_meters,
};

static esp_err_t qmx_query_state(void) {
    cat_cdc_reset_parsed_if();
    return qmx_send_cmd("IF;", 50);
}

// AI2; asks the radio to send IF; unprompted whenever its state changes (frequency, mode, TX,
// split), AI0; to stop. Firmware without AI answers "?;", which the parser ignores, and the state
// is then read only on request.
static esp_err_t qmx_set_push(bool on) {
    return qmx_send_cmd(on ? "AI2;" : "AI0;", 50);
}

static uint32_t qmx_state_changes(void) {
    return cat_cdc_if_changes();
}

static bool qmx_get_state(radio_state_t* out) {
    int hz = 0, tx = 0, mode = 0, split = 0;
    if (!cat_cdc_get_parsed_if(&hz, &tx, &mode, &split)) {
        return false;
    }
    if (out) {
        out->freq_hz = hz;
        out->transmitting = tx != 0;
        out->mode = mode;
        out->split = split != 0;
    }
    return true;
}

static esp_err_t qmx_query_version(void) {
    return qmx_send_cmd("VN;", 50);
}

// Puts the QMX in IQ mode at each CAT link-up and reads it back: the P4 decodes only its I/Q, and the
// radio forgets IQ mode at power-off (architecture/radio.md, The QMX's receive output). A radio that
// stays in audio mode, or does not answer, is warned about once; neither is retried.
static QmxIqLink s_iq_link;

// Reads, after IQ mode is settled, each band's Transmit flag and PTT outputs and the PTT jack's
// serial setting with the Menu Manager (architecture/radio.md, Menu Manager). Logged on the console.
static QmxMenuRead s_menu_read;
// The finished read, for get_config(): main-loop writes, other tasks read. Valid is cleared before
// the copy changes and set after, and a reader checks it on both sides of its copy.
static radio_config_t s_config;
static volatile bool s_config_valid;

static void publish_config(const QmxRadioSettings& r) {
    s_config_valid = false;
    radio_config_t c = {};
    snprintf(c.model, sizeof(c.model), "%s", r.band_version);
    c.band_count = r.band_count;
    for (int i = 0; i < r.band_count && i < RADIO_CONFIG_BANDS_MAX; ++i) {
        const QmxBandSettings& b = r.bands[i];
        c.bands[i] = {b.band_m, b.flag[kQmxFlagTransmit], b.flag[kQmxFlagTxPtt5v], b.flag[kQmxFlagTxPttGround],
                      b.flag[kQmxFlagRxPtt5v], b.flag[kQmxFlagRxPttGround]};
    }
    c.ptt_is_serial = r.serial6_on_ptt;
    s_config = c;
    s_config_valid = true;
}

static bool qmx_get_config(radio_config_t* out) {
    if (!s_config_valid) {
        return false;
    }
    radio_config_t copy = s_config;
    if (!s_config_valid) {
        return false;
    }
    if (out) {
        *out = copy;
    }
    return true;
}

static void log_radio_settings(const QmxRadioSettings& r) {
    static const char* const kFlag[] = {"?", "off", "on"};
    ESP_LOGI(TAG, "QMX menus: band version \"%s\", %d bands, Serial 6 on PTT %s", r.band_version, r.band_count,
             kFlag[r.serial6_on_ptt + 1]);
    for (int c = 0; c < r.band_count; ++c) {
        const QmxBandSettings& b = r.bands[c];
        ESP_LOGI(TAG, "QMX band %d m: Transmit %s, TX PTT +5V %s, TX PTT grounded %s, RX PTT +5V %s, RX PTT grounded %s",
                 b.band_m, kFlag[b.flag[kQmxFlagTransmit] + 1], kFlag[b.flag[kQmxFlagTxPtt5v] + 1],
                 kFlag[b.flag[kQmxFlagTxPttGround] + 1], kFlag[b.flag[kQmxFlagRxPtt5v] + 1],
                 kFlag[b.flag[kQmxFlagRxPttGround] + 1]);
    }
}

// Setting the PTT outputs (architecture/radio.md, Setting the PTT outputs). Asked for from the Port A
// task, run here in the main loop, which owns the read it plans from.
static QmxMenuWrite s_menu_write;
static volatile int s_write_requested;  // a QmxPttKeying, 0 none
static volatile bool s_write_running;
static char s_write_status[96];
static volatile bool s_write_status_set;

static void set_write_status(const char* text) {
    s_write_status_set = false;
    snprintf(s_write_status, sizeof(s_write_status), "%s", text);
    s_write_status_set = true;
}

static const char* qmx_set_ptt_outputs(radio_ptt_keying_t keying) {
    if (keying != RADIO_PTT_GROUND && keying != RADIO_PTT_5V) return "unknown keying";
    if (!s_config_valid) return "the radio's settings are not read yet";
    if (s_write_requested || s_write_running) return "already writing";
    set_write_status("writing");
    s_write_requested = keying;
    return nullptr;
}

static bool qmx_config_write_status(char* out, size_t cap) {
    if (!s_write_status_set) return false;
    if (out && cap) snprintf(out, cap, "%s", s_write_status);
    return true;
}

// Reads the radio afresh, so the app sees what the write left.
static void reread_radio(void) {
    s_config_valid = false;
    s_menu_read = QmxMenuRead{};
}

// True while a write runs, so the read waits.
static bool qmx_menu_write_tick(bool ready, bool busy) {
    if (s_write_requested && !s_write_running) {
        const QmxPttKeying keying = static_cast<QmxPttKeying>(s_write_requested);
        const int cells = qmx_menu_write_plan(s_menu_read.result, keying, s_menu_write);
        s_write_requested = 0;
        if (cells == 0) {
            set_write_status("nothing to write");
            return false;
        }
        ESP_LOGI(TAG, "QMX PTT outputs: writing %d cells", cells);
        s_write_running = true;
    }
    if (!s_write_running) return false;
    char value[CAT_REPLY_FRAME_MAX];
    const uint32_t mm_count = cat_cdc_get_mm(value, sizeof(value));
    const int64_t now_ms = esp_timer_get_time() / 1000;
    switch (qmx_menu_write_step(s_menu_write, ready, busy, mm_count, value, cat_cdc_refusals(), now_ms)) {
        case QmxMenuWriteAction::None:
            break;
        case QmxMenuWriteAction::Send: {
            char cmd[160];
            qmx_menu_write_command(s_menu_write, cmd, sizeof(cmd));
            if (qmx_send_cmd(cmd, 200) != ESP_OK) {
                ESP_LOGW(TAG, "QMX PTT outputs: %s not sent", cmd);
            }
            break;
        }
        case QmxMenuWriteAction::Reload:
            (void)qmx_send_cmd("MU;", 200);
            break;
        case QmxMenuWriteAction::Done: {
            char text[32];
            snprintf(text, sizeof(text), "done %d", s_menu_write.count);
            ESP_LOGI(TAG, "QMX PTT outputs: %d cells written, MU; sent; reading the radio again",
                     s_menu_write.count);
            set_write_status(text);
            s_write_running = false;
            reread_radio();
            break;
        }
        case QmxMenuWriteAction::Failed: {
            char path[64];
            char text[96];
            qmx_menu_write_path(s_menu_write.items[s_menu_write.next], path, sizeof(path));
            snprintf(text, sizeof(text), "failed at %s: %s", path,
                     !ready ? "radio gone" : s_menu_write.refused ? "refused" : "not read back");
            ESP_LOGW(TAG, "QMX PTT outputs: %s (%d of %d written)", text, s_menu_write.next, s_menu_write.count);
            set_write_status(text);
            s_write_running = false;
            if (ready) reread_radio();
            break;
        }
    }
    return true;
}

static void qmx_menu_read_tick(bool ready, bool busy) {
    char value[CAT_REPLY_FRAME_MAX];
    const uint32_t mm_count = cat_cdc_get_mm(value, sizeof(value));
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!ready) {
        s_config_valid = false;  // the next radio may be another
        if (!s_write_running) s_write_status_set = false;
    }
    switch (qmx_menu_read_step(s_menu_read, ready, busy, mm_count, value, cat_cdc_refusals(), now_ms)) {
        case QmxMenuReadAction::None:
            return;
        case QmxMenuReadAction::Send: {
            char cmd[64];
            qmx_menu_read_command(s_menu_read, cmd, sizeof(cmd));
            if (qmx_send_cmd(cmd, 200) != ESP_OK) {
                // Asked again after a link that stays down resets the read; one that comes back
                // misses this reply and ends the read as unanswered.
                ESP_LOGW(TAG, "QMX menus: %s not sent", cmd);
            }
            return;
        }
        case QmxMenuReadAction::Done:
            log_radio_settings(s_menu_read.result);
            publish_config(s_menu_read.result);
            return;
        case QmxMenuReadAction::NoReply: {
            char cmd[64];
            qmx_menu_read_command(s_menu_read, cmd, sizeof(cmd));
            ESP_LOGW(TAG, "QMX menus: %s unanswered; band and PTT settings are unread", cmd);
            return;
        }
    }
}

static radio_rx_path_event_t qmx_iq_path_tick(bool busy) {
    int reply = -1;
    (void)cat_cdc_get_parsed_q9(&reply);
    switch (qmx_iq_link_step(s_iq_link, cat_cdc_ready(), busy, reply, esp_timer_get_time() / 1000)) {
        case QmxIqLinkAction::None:
            return RADIO_RX_PATH_NONE;
        case QmxIqLinkAction::Send:
            cat_cdc_reset_parsed_q9();
            if (qmx_send_cmd("Q9 1;Q9;", 200) != ESP_OK) {
                // Not sent: retried after a pause, a few times, so a dead write does not stall
                // the main loop for 200 ms every pass.
                if (qmx_iq_link_send_failed(s_iq_link, esp_timer_get_time() / 1000)) {
                    ESP_LOGW(TAG, "QMX Q9 1; not sent after %d tries: IQ mode is unconfirmed",
                             kQmxIqLinkSendTries);
                }
                return RADIO_RX_PATH_NONE;
            }
            ESP_LOGI(TAG, "QMX receive output set to IQ (Q9 1;)");
            return RADIO_RX_PATH_NONE;
        case QmxIqLinkAction::Matched:
            ESP_LOGI(TAG, "QMX Q9; reads IQ, as set");
            return RADIO_RX_PATH_MATCHED;
        case QmxIqLinkAction::Mismatch:
            ESP_LOGW(TAG, "QMX Q9; reads audio, not IQ: nothing will decode");
            return RADIO_RX_PATH_MISMATCH;
        case QmxIqLinkAction::NoReply:
            ESP_LOGW(TAG, "QMX Q9; unanswered: its firmware may lack IQ mode, and nothing will decode");
            return RADIO_RX_PATH_NO_REPLY;
    }
    return RADIO_RX_PATH_NONE;
}

static radio_rx_path_event_t qmx_rx_path_tick(bool busy) {
    const radio_rx_path_event_t event = qmx_iq_path_tick(busy);
    // Stepped on every pass, so a link that drops resets the read for the next radio; it starts
    // once IQ mode is settled, so the two link-up reads do not interleave.
    const bool menus_ready = cat_cdc_ready() && s_iq_link.phase == QmxIqLinkPhase::Done;
    if (!qmx_menu_write_tick(menus_ready, busy)) {
        qmx_menu_read_tick(menus_ready, busy);
    }
    return event;
}

static bool qmx_get_version(char* out, size_t cap) {
    return cat_cdc_get_version(out, cap);
}

static const radio_swr_io_t k_swr_io = {
    .unkey = qmx_swr_unkey,
    .mode_digital = qmx_swr_mode_digital,
    .set_frequency = qmx_swr_set_frequency,
    .query_frequency = qmx_swr_query_frequency,
    .key = qmx_swr_key,
    .query_swr = qmx_swr_query_swr,
    .frequency_reply = qmx_swr_frequency_reply,
    .swr_reply = qmx_swr_swr_reply,
    .clear_replies = cat_cdc_reset_parsed_meters,
    .restore = qmx_sync_frequency_mode,
};

static const radio_control_capabilities_t k_capabilities = {
    .has_wattmeter = true,
    .has_swr = true,
    .can_set_time = true,
    .audio_is_uac = true,
    .can_md8_tune = true,
    .can_ps0 = true,
    .can_sr = true,
};

static const radio_control_ops_t k_ops = {
    .name = "qmx",
    .capabilities = &k_capabilities,
    .ready = qmx_ready,
    .on_audio_start = qmx_on_audio_start,
    .sync_frequency_mode = qmx_sync_frequency_mode,
    .begin_tx = qmx_begin_tx,
    .set_tone_hz = qmx_set_tone_hz,
    .end_tx = qmx_end_tx,
    .key_up = qmx_key_up,
    .set_tune = qmx_set_tune,
    .set_time = qmx_set_time,
    .reset_tx_power_swr = qmx_reset_tx_power_swr,
    .poll_tx_power_swr = qmx_poll_tx_power_swr,
    .get_tx_power_swr = qmx_get_tx_power_swr,
    .begin_cpfsk_tx = nullptr,
    .swr_io = &k_swr_io,
    .swr_lock = &k_swr_lock,
    .query_state = qmx_query_state,
    .get_state = qmx_get_state,
    .query_version = qmx_query_version,
    .get_version = qmx_get_version,
    .rx_path_tick = qmx_rx_path_tick,
    .set_push = qmx_set_push,
    .state_changes = qmx_state_changes,
    .get_config = qmx_get_config,
    .set_ptt_outputs = qmx_set_ptt_outputs,
    .config_write_status = qmx_config_write_status,
};

const radio_control_ops_t* radio_control_qmx_get_ops(void) {
    return &k_ops;
}
