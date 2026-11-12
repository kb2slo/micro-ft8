#include <cstdio>

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
// radio_control_lock_tx(): set from the Port A task just before a restart, read by whichever task would key.
static std::atomic<bool> s_tx_locked{false};

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
    // A radio that reports its frequency is believed instead (radio_control_dial_tick()): it can refuse a
    // frequency it has no band for and stay where it was. One that cannot report is where we set it.
    if (err == ESP_OK && freq_hz > 0 && !ops->get_state) dial_history_note(rtc_now_ms(), (uint32_t)freq_hz);
    return err;
}

void radio_control_dial_tick(void) {
    static uint32_t s_seen = 0;
    const uint32_t changes = radio_control_state_changes();
    if (changes == s_seen) return;
    // Not consumed while the radio transmits or is retuned for a transmission: that dial is ours, and the
    // report after it, back on the receive dial, is the one to note.
    if (radio_control_tx_retuned_hz() != 0) return;
    radio_state_t rs;
    if (!radio_control_get_state(&rs) || rs.transmitting) return;
    s_seen = changes;
    if (rs.freq_hz > 0) dial_history_note(rtc_now_ms(), (uint32_t)rs.freq_hz);
}

void radio_control_lock_tx(void) {
    if (!s_tx_locked.exchange(true)) {
        ESP_LOGW(TAG, "Transmit locked until the restart");
    }
}

esp_err_t radio_control_begin_tx(int tx_dial_hz, int tx_base_hz, int rx_dial_hz) {
    if (s_tx_locked.load()) return ESP_ERR_INVALID_STATE;
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
    if (enable && s_tx_locked.load()) return ESP_ERR_INVALID_STATE;
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
    if (s_tx_locked.load()) return ESP_ERR_INVALID_STATE;
    return ops->begin_cpfsk_tx(base_hz, symbols, symbol_count, tone_spacing_hz,
                               samples_per_symbol);
}

// The sweep and the amplifier prime key through the backend's table directly, and keep its pointer for a whole
// run, so the lock is held in the key they are handed, not where the table is asked for.
static esp_err_t swr_key_unless_locked(void) {
    if (s_tx_locked.load()) return ESP_ERR_INVALID_STATE;
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->swr_io && ops->swr_io->key ? ops->swr_io->key() : ESP_ERR_INVALID_STATE;
}

const radio_swr_io_t* radio_control_swr_io(void) {
    if (!radio_control_get_capabilities()->can_md8_tune) {
        return nullptr;
    }
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->swr_io) return nullptr;
    // The backend's table with our key in place of its own. One backend has a table, so one copy serves.
    static radio_swr_io_t s_io;
    static std::atomic<const radio_swr_io_t*> s_io_of{nullptr};
    if (s_io_of.load() != ops->swr_io) {
        s_io = *ops->swr_io;
        if (s_io.key) s_io.key = swr_key_unless_locked;
        s_io_of.store(ops->swr_io);
    }
    return &s_io;
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

bool radio_control_get_config(radio_config_t* out) {
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->get_config && ops->get_config(out);
}

const char* radio_control_set_ptt_outputs(radio_ptt_keying_t keying) {
    const radio_control_ops_t* ops = current_ops();
    if (!ops || !ops->set_ptt_outputs) return "this radio cannot set its PTT outputs";
    return ops->set_ptt_outputs(keying);
}

bool radio_control_config_write_status(char* out, size_t cap) {
    const radio_control_ops_t* ops = current_ops();
    return ops && ops->config_write_status && ops->config_write_status(out, cap);
}

static char flag_char(int8_t flag) {
    return flag == 1 ? '1' : flag == 0 ? '0' : '?';
}

size_t radio_config_format(const radio_config_t* config, char* out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = 0;
    if (!config) {
        return 0;
    }
    size_t len = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (len < cap) {
            const int n = snprintf(out + len, cap - len, fmt, args...);
            len = n < 0 ? len : (len + (size_t)n < cap ? len + (size_t)n : cap - 1);
        }
    };
    const int count = config->band_count < RADIO_CONFIG_BANDS_MAX ? config->band_count : RADIO_CONFIG_BANDS_MAX;
    put("radio_model=%s\nradio_bands=", config->model);
    for (int i = 0; i < count; ++i) {
        put(i ? " %d" : "%d", config->bands[i].band_m);
    }
    put("\nradio_band_tx=");
    for (int i = 0; i < count; ++i) {
        put(i ? " %c" : "%c", flag_char(config->bands[i].transmit));
    }
    put("\nradio_ptt=");
    for (int i = 0; i < count; ++i) {
        const radio_band_config_t& b = config->bands[i];
        put(i ? " %c%c%c%c" : "%c%c%c%c", flag_char(b.tx_ptt_5v), flag_char(b.tx_ptt_ground), flag_char(b.rx_ptt_5v),
            flag_char(b.rx_ptt_ground));
    }
    put("\nradio_ptt_serial=%c\n", flag_char(config->ptt_is_serial));
    return len;
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
