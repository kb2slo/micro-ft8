#include "radio_control_backend.h"
#include "qmx_iq_link.h"
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

// Sets the receive output to IQ or audio, whichever the station wants, at each CAT link-up and each time
// the want changes (main.cpp passes the path qmx_rx_mode.h decided for the slot about to begin), and
// reads it back. `Q9 0;` is sent too, deliberately: a radio another tool left in IQ mode streams I/Q
// that the audio path decodes as nothing, silently (RFC 0006). The read-back only warns: the capture
// path was chosen by the schedule and does not follow the radio, except that a change whose write did not
// go out (RADIO_RX_PATH_NOT_SENT) or went out late is taken back by the caller, so the two stay together.
static QmxIqLink s_iq_link;

static radio_rx_path_event_t qmx_rx_path_tick(bool want_iq, bool busy) {
    int reply = -1;
    (void)cat_cdc_get_parsed_q9(&reply);
    const char* want = want_iq ? "IQ" : "audio";
    switch (qmx_iq_link_step(s_iq_link, cat_cdc_ready(), busy, want_iq, reply,
                             esp_timer_get_time() / 1000)) {
        case QmxIqLinkAction::None:
            return RADIO_RX_PATH_NONE;
        case QmxIqLinkAction::Send:
            cat_cdc_reset_parsed_q9();
            if (qmx_send_cmd(want_iq ? "Q9 1;Q9;" : "Q9 0;Q9;", 200) != ESP_OK) {
                // Not sent: retried after a pause, a few times, so a dead write does not stall
                // the main loop for 200 ms every pass.
                if (qmx_iq_link_send_failed(s_iq_link, esp_timer_get_time() / 1000)) {
                    ESP_LOGW(TAG, "QMX Q9 not sent after %d tries: the receive output is unconfirmed",
                             kQmxIqLinkSendTries);
                }
                return RADIO_RX_PATH_NOT_SENT;  // the caller takes back a path change that needed it
            }
            ESP_LOGI(TAG, "QMX receive output set to %s (Q9 %d;)", want, want_iq ? 1 : 0);
            return RADIO_RX_PATH_SENT;
        case QmxIqLinkAction::Matched:
            ESP_LOGI(TAG, "QMX Q9; reads %s, as set", want);
            return RADIO_RX_PATH_MATCHED;
        case QmxIqLinkAction::Mismatch:
            ESP_LOGW(TAG, "QMX Q9; reads %s, but the receive path wants %s: decode will hear the wrong thing",
                     want_iq ? "audio" : "IQ", want);
            return RADIO_RX_PATH_MISMATCH;
        case QmxIqLinkAction::NoReply:
            ESP_LOGW(TAG, "QMX Q9; unanswered: cannot confirm the receive output is %s", want);
            return RADIO_RX_PATH_NO_REPLY;
    }
    return RADIO_RX_PATH_NONE;
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
};

const radio_control_ops_t* radio_control_qmx_get_ops(void) {
    return &k_ops;
}
