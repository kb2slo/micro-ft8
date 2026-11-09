#include "dial_history.h"
#include "rtc_core.h"
#include "radio_control.h"

#include "radio_control_backend.h"
#include "tx_timing.h"

#include "esp_log.h"

#include <atomic>

static const char* TAG = "RADIO_CTRL";

static radio_control_backend_t s_backend = RADIO_CONTROL_QMX;
// The TX scheduler keys and ends a transmission from one task; the Port A tick reads this from another.
static std::atomic<int> s_tx_retuned_hz{0};

static const radio_control_capabilities_t k_capabilities_none = {};

static const radio_control_ops_t* current_ops(void) {
    switch (s_backend) {
    case RADIO_CONTROL_QMX:
        return radio_control_qmx_get_ops();
    case RADIO_CONTROL_NONE:
        return nullptr;
    }
    return radio_control_qmx_get_ops();
}

void radio_control_set_backend(radio_control_backend_t backend) {
    s_backend = backend;
    const radio_control_ops_t* ops = current_ops();
    ESP_LOGI(TAG, "Selected radio control backend=%s", ops ? ops->name : "none");
}

radio_control_backend_t radio_control_get_backend(void) {
    return s_backend;
}

const char* radio_control_backend_name(radio_control_backend_t backend) {
    switch (backend) {
    case RADIO_CONTROL_QMX:
        return "qmx";
    case RADIO_CONTROL_NONE:
        return "none";
    }
    return "unknown";
}

const radio_control_capabilities_t* radio_control_get_capabilities(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->capabilities) {
        return &k_capabilities_none;
    }
    return ops->capabilities;
}

bool radio_control_ready(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->ready) return false;
    return ops->ready();
}

esp_err_t radio_control_on_audio_start(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->on_audio_start) return ESP_ERR_INVALID_STATE;
    return ops->on_audio_start();
}

esp_err_t radio_control_sync_frequency_mode(int freq_hz) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->sync_frequency_mode) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = ops->sync_frequency_mode(freq_hz);
    // Every board tunes through here, so this is where the dial's history is
    // kept: a decode is labelled with the frequency it was heard on from it
    // (dial_history.h, Port A DECODE's dial_hz).
    if (err == ESP_OK && freq_hz > 0) dial_history_note(rtc_now_ms(), (uint32_t)freq_hz);
    return err;
}

esp_err_t radio_control_begin_tx(int tx_dial_hz, int tx_base_hz, int rx_dial_hz) {
    radio_control_reset_tx_power_swr();
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->begin_tx) return ESP_ERR_INVALID_STATE;
    // Set before the radio is told, so a failed begin is still undone by the end_tx() its caller sends.
    s_tx_retuned_hz.store(tx_dial_hz != rx_dial_hz ? tx_dial_hz : 0);
    return ops->begin_tx(tx_dial_hz, tx_base_hz, rx_dial_hz);
}

esp_err_t radio_control_set_tone_hz(float tone_hz) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->set_tone_hz) return ESP_ERR_INVALID_STATE;
    return ops->set_tone_hz(tone_hz);
}

esp_err_t radio_control_end_tx(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->end_tx) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = ops->end_tx();
    s_tx_retuned_hz.store(0);
    return err;
}

int radio_control_tx_retuned_hz(void) {
    return s_tx_retuned_hz.load();
}

esp_err_t radio_control_key_up(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->key_up) return ESP_ERR_NOT_SUPPORTED;
    return ops->key_up();
}

int radio_control_tone_lead_ms(void) {
    const radio_control_ops_t* ops = current_ops();
    return ops && !ops->begin_cpfsk_tx ? kTxToneLeadMs : 0;
}

esp_err_t radio_control_set_tune(bool enable, int freq_hz, int tone_hz) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->set_tune) return ESP_ERR_INVALID_STATE;
    return ops->set_tune(enable, freq_hz, tone_hz);
}

esp_err_t radio_control_set_time(int hour, int minute, int second) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->set_time) return ESP_ERR_NOT_SUPPORTED;
    return ops->set_time(hour, minute, second);
}

esp_err_t radio_control_begin_cpfsk_tx(float base_hz,
                                       const uint8_t* symbols,
                                       size_t symbol_count,
                                       float tone_spacing_hz,
                                       uint32_t samples_per_symbol) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->begin_cpfsk_tx) return ESP_ERR_NOT_SUPPORTED;
    return ops->begin_cpfsk_tx(base_hz, symbols, symbol_count, tone_spacing_hz,
                               samples_per_symbol);
}

const radio_swr_io_t* radio_control_swr_io(void) {
    if (!radio_control_get_capabilities()->can_md8_tune) {
        return nullptr;
    }
    const radio_control_ops_t* ops = current_ops();
    return ops ? ops->swr_io : nullptr;
}

const radio_swr_lock_t* radio_control_swr_lock(void) {
    if (!radio_control_get_capabilities()->can_sr) {
        return nullptr;
    }
    const radio_control_ops_t* ops = current_ops();
    return ops ? ops->swr_lock : nullptr;
}

esp_err_t radio_control_query_state(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->query_state) return ESP_ERR_NOT_SUPPORTED;
    return ops->query_state();
}

esp_err_t radio_control_query_version(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->query_version) return ESP_ERR_NOT_SUPPORTED;
    return ops->query_version();
}

bool radio_control_get_version(char* out, size_t cap) {
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->get_version && ops->get_version(out, cap);
}

radio_rx_path_event_t radio_control_rx_path_tick(bool busy) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->rx_path_tick) return RADIO_RX_PATH_NONE;
    return ops->rx_path_tick(busy);
}

esp_err_t radio_control_set_push(bool on) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->set_push) return ESP_ERR_NOT_SUPPORTED;
    return ops->set_push(on);
}

uint32_t radio_control_state_changes(void) {
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->state_changes ? ops->state_changes() : 0;
}

bool radio_control_get_state(radio_state_t* out) {
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->get_state && ops->get_state(out);
}

void radio_control_reset_tx_power_swr(void) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->reset_tx_power_swr) return;
    ops->reset_tx_power_swr();
}

static bool power_swr_supported(void) {
    const radio_control_capabilities_t* capabilities = radio_control_get_capabilities();
    return capabilities->has_wattmeter || capabilities->has_swr;
}

void radio_control_poll_tx_power_swr(void) {
    if (!power_swr_supported()) {
        return;
    }
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->poll_tx_power_swr) return;
    ops->poll_tx_power_swr();
}

bool radio_control_get_tx_power_swr(float* power_w, float* swr) {
    if (!power_swr_supported()) {
        return false;
    }
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->get_tx_power_swr) return false;
    return ops->get_tx_power_swr(power_w, swr);
}
