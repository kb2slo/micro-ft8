#pragma once

#include <stddef.h>
#include <stdint.h>

#include "radio_control.h"

typedef struct {
    const char* name;
    const radio_control_capabilities_t* capabilities;
    bool (*ready)(void);
    esp_err_t (*on_audio_start)(void);
    esp_err_t (*sync_frequency_mode)(int freq_hz);
    // tx_dial_hz differs from rx_dial_hz when the transmission retunes (tx_timing.h, tx_plan()): the radio moves
    // there before keying and back to rx_dial_hz in end_tx(). Equal, nothing is retuned.
    esp_err_t (*begin_tx)(int tx_dial_hz, int tx_base_hz, int rx_dial_hz);
    esp_err_t (*set_tone_hz)(float tone_hz);
    esp_err_t (*end_tx)(void);
    esp_err_t (*key_up)(void);         // stop RF but stay keyed; null = RX; is the only way
    esp_err_t (*set_tune)(bool enable, int freq_hz, int tone_hz);
    esp_err_t (*set_time)(int hour, int minute, int second);
    void (*reset_tx_power_swr)(void);
    void (*poll_tx_power_swr)(void);
    bool (*get_tx_power_swr)(float* power_w, float* swr);
    // Null for a radio that takes its tones over CAT (QMX), which is also the
    // radio whose tones start kTxToneLeadMs after the slot (tx_timing.h).
    esp_err_t (*begin_cpfsk_tx)(float base_hz,
                                const uint8_t* symbols,
                                size_t symbol_count,
                                float tone_spacing_hz,
                                uint32_t samples_per_symbol);
    const radio_swr_io_t* swr_io;  // null = cannot sweep
    const radio_swr_lock_t* swr_lock;  // null = no SWR-protection lock to read
    esp_err_t (*query_state)(void);    // null = the radio cannot report its state
    bool (*get_state)(radio_state_t* out);
    esp_err_t (*query_version)(void);  // null = the radio cannot report its firmware
    bool (*get_version)(char* out, size_t cap);
    radio_rx_path_event_t (*rx_path_tick)(bool busy);  // null = one receive output only
    esp_err_t (*set_push)(bool on);    // null = the radio cannot push its state (QMX AI2; / AI0;)
    uint32_t (*state_changes)(void);   // null = no pushes; counts changes of the radio's reported state
    bool (*get_config)(radio_config_t* out);  // null = the radio cannot report its configuration
    const char* (*set_ptt_outputs)(radio_ptt_keying_t keying);  // null = cannot set them
    bool (*config_write_status)(char* out, size_t cap);
} radio_control_ops_t;

const radio_control_ops_t* radio_control_qmx_get_ops(void);
