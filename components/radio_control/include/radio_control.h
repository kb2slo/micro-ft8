#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_CONTROL_QMX = 0,
    RADIO_CONTROL_QDX = 1,
} radio_control_backend_t;

typedef struct {
    bool has_wattmeter;
    bool has_swr;
    bool can_set_time;
    bool audio_is_uac;
    bool can_md8_tune;
    bool can_ps0;
} radio_control_capabilities_t;

// What an SWR sweep needs from the radio, one CAT command per call, none of them
// blocking on a reply (swr_sweep.h owns the timing). Replies come back through
// freq_reply()/swr_reply(), which hold a value only after the matching query and
// only until clear_replies(). QMX only; see docs/architecture/radio.md (QMX SWR Tune over CAT).
typedef struct {
    esp_err_t (*unkey)(void);            // RX;
    esp_err_t (*mode_digital)(void);     // MD6;
    esp_err_t (*set_frequency)(int hz);  // FA<hz>;
    esp_err_t (*query_frequency)(void);  // FA;
    esp_err_t (*key)(void);              // MD8; enters SWR Tune, which keys by itself
    esp_err_t (*query_swr)(void);        // SW;
    bool (*frequency_reply)(int* hz);
    bool (*swr_reply)(int* hundredths);
    void (*clear_replies)(void);
    esp_err_t (*restore)(int hz);        // MD6, FR0, FT0, FA: the operating dial again
} radio_swr_io_t;

void radio_control_set_backend(radio_control_backend_t backend);
radio_control_backend_t radio_control_get_backend(void);
const char* radio_control_backend_name(radio_control_backend_t backend);
const radio_control_capabilities_t* radio_control_get_capabilities(void);

bool radio_control_ready(void);

esp_err_t radio_control_on_audio_start(void);
esp_err_t radio_control_sync_frequency_mode(int freq_hz);
esp_err_t radio_control_begin_tx(int freq_hz, int tx_base_hz);
esp_err_t radio_control_set_tone_hz(float tone_hz);
esp_err_t radio_control_end_tx(void);
esp_err_t radio_control_set_tune(bool enable, int freq_hz, int tone_hz);
esp_err_t radio_control_set_time(int hour, int minute, int second);
esp_err_t radio_control_begin_cpfsk_tx(float base_hz,
                                       const uint8_t* symbols,
                                       size_t symbol_count,
                                       float tone_spacing_hz,
                                       uint32_t samples_per_symbol);

// The sweep's radio operations, or NULL when the selected radio cannot sweep
// (no can_md8_tune). The pointer stays valid for the life of the program.
const radio_swr_io_t* radio_control_swr_io(void);

void radio_control_reset_tx_power_swr(void);
void radio_control_poll_tx_power_swr(void);
bool radio_control_get_tx_power_swr(float* power_w, float* swr);

#ifdef __cplusplus
}
#endif
